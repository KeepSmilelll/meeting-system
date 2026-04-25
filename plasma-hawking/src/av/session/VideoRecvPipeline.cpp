#include "VideoRecvPipeline.h"

#include "net/media/SocketAddressUtils.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace av::session {
namespace {

constexpr std::size_t kMaxTrackedRecvStreams = 8U;
constexpr uint64_t kRecvStreamIdleTimeoutMs = 30 * 1000U;
constexpr std::size_t kMaxPendingDecodeTasks = 8U;
constexpr std::size_t kMaxPendingDecodeResults = 8U;

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

class VideoRecvDecodeWorker final {
public:
    explicit VideoRecvDecodeWorker(uint32_t remoteMediaSsrc)
        : m_remoteMediaSsrc(remoteMediaSsrc),
          m_thread([this]() { run(); }) {}

    ~VideoRecvDecodeWorker() {
        shutdown();
    }

    VideoRecvDecodeWorker(const VideoRecvDecodeWorker&) = delete;
    VideoRecvDecodeWorker& operator=(const VideoRecvDecodeWorker&) = delete;

    struct DecodeResult {
        av::codec::DecodedVideoFrame frame;
        std::string error;
        uint32_t remoteMediaSsrc{0U};

        bool hasFrame() const {
            return frame.hasRenderableData();
        }
    };

    void enqueue(media::H264AccessUnit accessUnit, int frameRate) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return;
        }
        if (m_tasks.size() >= kMaxPendingDecodeTasks) {
            m_tasks.pop_front();
        }
        m_tasks.push_back(DecodeTask{std::move(accessUnit), frameRate});
        m_cv.notify_one();
    }

    bool tryDequeue(DecodeResult& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_results.empty()) {
            return false;
        }
        out = std::move(m_results.front());
        m_results.pop_front();
        return true;
    }

    bool waitDequeue(DecodeResult& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_results.empty() &&
            !m_cv.wait_for(lock, timeout, [this]() { return !m_results.empty() || m_stopping; })) {
            return false;
        }
        if (m_results.empty()) {
            return false;
        }
        out = std::move(m_results.front());
        m_results.pop_front();
        return true;
    }

    std::thread::id threadId() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_threadId;
    }

private:
    struct DecodeTask {
        media::H264AccessUnit accessUnit;
        int frameRate{5};
    };

    void run() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_threadId = std::this_thread::get_id();
        }

        av::codec::VideoDecoder decoder;
        if (!decoder.configure()) {
            pushResult(DecodeResult{
                {},
                "video decoder configure failed",
                m_remoteMediaSsrc,
            });
            return;
        }

        while (true) {
            DecodeTask task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() { return m_stopping || !m_tasks.empty(); });
                if (m_stopping && m_tasks.empty()) {
                    break;
                }
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }

            av::codec::EncodedVideoFrame encoded;
            encoded.payload = std::move(task.accessUnit.payload);
            encoded.pts = task.accessUnit.pts;
            encoded.payloadType = task.accessUnit.payloadType;
            encoded.frameRate = task.frameRate;

            av::codec::DecodedVideoFrame decoded;
            std::string decodeError;
            if (!decoder.decode(encoded, decoded, &decodeError)) {
                if (decodeError.find("Resource temporarily unavailable") != std::string::npos) {
                    continue;
                }
                pushResult(DecodeResult{
                    {},
                    decodeError.empty() ? "video decode failed" : decodeError,
                    m_remoteMediaSsrc,
                });
                continue;
            }

            pushResult(DecodeResult{
                std::move(decoded),
                {},
                m_remoteMediaSsrc,
            });
        }
    }

    void pushResult(DecodeResult result) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_results.size() >= kMaxPendingDecodeResults) {
            m_results.pop_front();
        }
        m_results.push_back(std::move(result));
        m_cv.notify_all();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_stopping = true;
            m_cv.notify_all();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    const uint32_t m_remoteMediaSsrc{0U};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<DecodeTask> m_tasks;
    std::deque<DecodeResult> m_results;
    std::thread m_thread;
    std::thread::id m_threadId{};
    bool m_stopping{false};
};

VideoRecvPipeline::VideoRecvPipeline(VideoRecvPipelineConfig config)
    : m_config(config) {}

VideoRecvPipeline::~VideoRecvPipeline() = default;

VideoRecvDatagram VideoRecvPipeline::classifyDatagram(const uint8_t* data,
                                                      std::size_t len,
                                                      bool acceptSender,
                                                      bool acceptRtcpFromPeerHost,
                                                      media::RTPReceiver& receiver) const {
    VideoRecvDatagram datagram{};

    const bool isRtcp = media::looksLikeRtcpPacket(data, len);
    if (!acceptSender && (!isRtcp || !acceptRtcpFromPeerHost)) {
        datagram.kind = VideoRecvDatagramKind::Ignore;
        return datagram;
    }

    if (isRtcp) {
        datagram.kind = VideoRecvDatagramKind::Rtcp;
        return datagram;
    }

    if (!receiver.parsePacket(data, len, datagram.rtpPacket)) {
        datagram.kind = VideoRecvDatagramKind::Ignore;
        return datagram;
    }

    datagram.kind = VideoRecvDatagramKind::Rtp;
    return datagram;
}

VideoRecvPacketResult VideoRecvPipeline::consumePacket(const media::RTPPacket& packet,
                                                       uint32_t expectedRemoteSsrc,
                                                       av::codec::DecodedVideoFrame& outFrame,
                                                       uint32_t& outRemoteMediaSsrc,
                                                       std::string* error) {
    if (error != nullptr) {
        error->clear();
    }

    if (packet.header.payloadType != m_config.screenPayloadType &&
        packet.header.payloadType != m_config.cameraPayloadType) {
        outRemoteMediaSsrc = 0U;
        return VideoRecvPacketResult::Ignored;
    }

    if (expectedRemoteSsrc != 0U && packet.header.ssrc != expectedRemoteSsrc) {
        outRemoteMediaSsrc = 0U;
        return VideoRecvPacketResult::Ignored;
    }

    const uint32_t remoteMediaSsrc = expectedRemoteSsrc != 0U ? expectedRemoteSsrc : packet.header.ssrc;
    outRemoteMediaSsrc = remoteMediaSsrc;
    const uint64_t nowMs = steadyNowMs();
    StreamState* streamState = findOrCreateStreamState(remoteMediaSsrc, nowMs, expectedRemoteSsrc, error);
    if (streamState == nullptr || streamState->decodeWorker == nullptr) {
        if (error != nullptr && error->empty()) {
            *error = "video decoder stream state unavailable";
        }
        return VideoRecvPacketResult::DecodeFailed;
    }

    media::H264AccessUnit accessUnit;
    bool packetLossDetected = false;
    if (!streamState->assembler.consume(packet, accessUnit, &packetLossDetected)) {
        const VideoRecvPacketResult popResult =
            popDecodedFrameResult(remoteMediaSsrc, outFrame, outRemoteMediaSsrc, error);
        if (popResult != VideoRecvPacketResult::NeedMore) {
            return popResult;
        }
        return packetLossDetected ? VideoRecvPacketResult::PacketLoss
                                  : VideoRecvPacketResult::NeedMore;
    }

    streamState->decodeWorker->enqueue(std::move(accessUnit), m_config.frameRate);
    VideoRecvDecodeWorker::DecodeResult decodeResult;
    if (streamState->decodeWorker->waitDequeue(decodeResult, std::chrono::milliseconds(4))) {
        outRemoteMediaSsrc = decodeResult.remoteMediaSsrc;
        if (!decodeResult.error.empty()) {
            if (error != nullptr) {
                *error = decodeResult.error;
            }
            return VideoRecvPacketResult::DecodeFailed;
        }
        if (decodeResult.hasFrame()) {
            outFrame = std::move(decodeResult.frame);
            return VideoRecvPacketResult::FrameReady;
        }
    }
    const VideoRecvPacketResult popResult =
        popDecodedFrameResult(remoteMediaSsrc, outFrame, outRemoteMediaSsrc, error);
    if (popResult != VideoRecvPacketResult::NeedMore) {
        return popResult;
    }
    return VideoRecvPacketResult::DecodePending;
}

VideoRecvPacketResult VideoRecvPipeline::pollDecodedFrame(av::codec::DecodedVideoFrame& outFrame,
                                                          uint32_t& outRemoteMediaSsrc,
                                                          std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    return popDecodedFrameResult(0U, outFrame, outRemoteMediaSsrc, error);
}

VideoRecvPipeline::StreamState* VideoRecvPipeline::findOrCreateStreamState(uint32_t ssrc,
                                                                            uint64_t nowMs,
                                                                            uint32_t expectedRemoteSsrc,
                                                                            std::string* error) {
    if (ssrc == 0U) {
        return nullptr;
    }

    auto found = m_streamStates.find(ssrc);
    if (found != m_streamStates.end()) {
        found->second.lastSeenAtMs = nowMs;
        return &found->second;
    }

    pruneStreamStates(nowMs, expectedRemoteSsrc);

    auto [insertedIt, inserted] = m_streamStates.try_emplace(ssrc);
    if (!inserted) {
        return nullptr;
    }

    insertedIt->second.decodeWorker = std::make_shared<VideoRecvDecodeWorker>(ssrc);
    if (!insertedIt->second.decodeWorker) {
        m_streamStates.erase(insertedIt);
        if (error != nullptr) {
            *error = "video decoder worker alloc failed";
        }
        return nullptr;
    }
    insertedIt->second.lastSeenAtMs = nowMs;
    return &insertedIt->second;
}

VideoRecvPacketResult VideoRecvPipeline::popDecodedFrameResult(uint32_t preferredSsrc,
                                                               av::codec::DecodedVideoFrame& outFrame,
                                                               uint32_t& outRemoteMediaSsrc,
                                                               std::string* error) {
    VideoRecvDecodeWorker::DecodeResult decodedResult;
    auto mapResult = [&](VideoRecvDecodeWorker::DecodeResult&& result) -> VideoRecvPacketResult {
        outRemoteMediaSsrc = result.remoteMediaSsrc;
        if (!result.error.empty()) {
            if (error != nullptr) {
                *error = result.error;
            }
            return VideoRecvPacketResult::DecodeFailed;
        }
        if (!result.hasFrame()) {
            return VideoRecvPacketResult::NeedMore;
        }
        outFrame = std::move(result.frame);
        return VideoRecvPacketResult::FrameReady;
    };

    if (preferredSsrc != 0U) {
        auto preferred = m_streamStates.find(preferredSsrc);
        if (preferred != m_streamStates.end() &&
            preferred->second.decodeWorker &&
            preferred->second.decodeWorker->tryDequeue(decodedResult)) {
            return mapResult(std::move(decodedResult));
        }
    }

    for (auto& [ssrc, streamState] : m_streamStates) {
        if (ssrc == preferredSsrc || !streamState.decodeWorker) {
            continue;
        }
        if (!streamState.decodeWorker->tryDequeue(decodedResult)) {
            continue;
        }
        return mapResult(std::move(decodedResult));
    }

    return VideoRecvPacketResult::NeedMore;
}

void VideoRecvPipeline::pruneStreamStates(uint64_t nowMs, uint32_t expectedRemoteSsrc) {
    if (expectedRemoteSsrc != 0U) {
        for (auto it = m_streamStates.begin(); it != m_streamStates.end();) {
            if (it->first == expectedRemoteSsrc) {
                ++it;
                continue;
            }
            it = m_streamStates.erase(it);
        }
    } else {
        for (auto it = m_streamStates.begin(); it != m_streamStates.end();) {
            if (nowMs >= it->second.lastSeenAtMs &&
                (nowMs - it->second.lastSeenAtMs) > kRecvStreamIdleTimeoutMs) {
                it = m_streamStates.erase(it);
                continue;
            }
            ++it;
        }
    }

    while (m_streamStates.size() >= kMaxTrackedRecvStreams) {
        auto oldest = m_streamStates.end();
        uint64_t oldestSeenAt = (std::numeric_limits<uint64_t>::max)();
        for (auto it = m_streamStates.begin(); it != m_streamStates.end(); ++it) {
            if (it->second.lastSeenAtMs < oldestSeenAt) {
                oldestSeenAt = it->second.lastSeenAtMs;
                oldest = it;
            }
        }
        if (oldest == m_streamStates.end()) {
            break;
        }
        m_streamStates.erase(oldest);
    }
}

VideoRecvHandlingDecision VideoRecvPipeline::makeHandlingDecision(VideoRecvPacketResult result,
                                                                  const std::string& pipelineError) const {
    VideoRecvHandlingDecision decision{};
    switch (result) {
    case VideoRecvPacketResult::NeedMore:
    case VideoRecvPacketResult::DecodePending:
    case VideoRecvPacketResult::Ignored:
        decision.action = VideoRecvHandlingAction::Continue;
        return decision;
    case VideoRecvPacketResult::PacketLoss:
        decision.action = VideoRecvHandlingAction::RequestKeyFrame;
        decision.keyFrameReason = "packet loss";
        return decision;
    case VideoRecvPacketResult::DecodeFailed:
        if (pipelineError.find("Invalid data found when processing input") != std::string::npos) {
            decision.action = VideoRecvHandlingAction::RequestKeyFrame;
            decision.keyFrameReason = "decode recovery";
            return decision;
        }
        decision.action = VideoRecvHandlingAction::RequestKeyFrameAndError;
        decision.keyFrameReason = "decode failure";
        decision.errorMessage = pipelineError.empty() ? "video decode failed" : pipelineError;
        return decision;
    case VideoRecvPacketResult::FrameReady:
        decision.action = VideoRecvHandlingAction::DeliverFrame;
        return decision;
    default:
        decision.action = VideoRecvHandlingAction::Continue;
        return decision;
    }
}

std::size_t VideoRecvPipeline::decodeWorkerCount() const {
    return m_streamStates.size();
}

std::size_t VideoRecvPipeline::distinctDecodeThreadCount() const {
    std::unordered_set<std::thread::id> threadIds;
    for (const auto& [ssrc, streamState] : m_streamStates) {
        (void)ssrc;
        if (!streamState.decodeWorker) {
            continue;
        }
        const std::thread::id workerId = streamState.decodeWorker->threadId();
        if (workerId != std::thread::id{}) {
            threadIds.insert(workerId);
        }
    }
    return threadIds.size();
}

}  // namespace av::session
