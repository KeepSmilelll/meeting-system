#include "av/capture/ScreenCapture.h"
#include "av/codec/VideoEncoder.h"
#include "av/session/VideoRecvPipeline.h"
#include "av/session/VideoSendPipeline.h"
#include "net/media/RTPReceiver.h"
#include "net/media/RTPSender.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kFrameRate = 15;
constexpr int kBitrate = 900 * 1000;
constexpr uint8_t kPayloadType = 97;
constexpr std::size_t kMaxPayloadBytes = 1200U;
constexpr uint32_t kSsrcA = 0x11111111U;
constexpr uint32_t kSsrcB = 0x22222222U;

av::capture::ScreenFrame makeSyntheticFrame(int64_t pts) {
    av::capture::ScreenFrame frame;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.pts = pts;
    frame.bgra.resize(static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) * 4U);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(kWidth) + static_cast<std::size_t>(x)) * 4U;
            frame.bgra[offset + 0] = static_cast<uint8_t>((x + pts) & 0xFF);       // B
            frame.bgra[offset + 1] = static_cast<uint8_t>((y + pts * 2) & 0xFF);   // G
            frame.bgra[offset + 2] = static_cast<uint8_t>((x + y + pts) & 0xFF);   // R
            frame.bgra[offset + 3] = 255U;                                          // A
        }
    }
    return frame;
}

bool encodePacketsForSsrc(av::codec::VideoEncoder& encoder,
                          media::RTPSender& sender,
                          std::vector<av::session::VideoSendPipelinePacket>& outPackets,
                          std::string* error) {
    av::session::VideoSendPipeline pipeline(
        av::session::VideoSendPipelineConfig{kFrameRate, kMaxPayloadBytes});
    outPackets.clear();
    if (error != nullptr) {
        error->clear();
    }

    for (int attempt = 0; attempt < 40; ++attempt) {
        const av::capture::ScreenFrame frame = makeSyntheticFrame(attempt);
        std::string pipelineError;
        bool encodedKeyFrame = false;
        if (pipeline.encodeAndPacketize(encoder,
                                        frame,
                                        kPayloadType,
                                        true,
                                        sender,
                                        outPackets,
                                        &encodedKeyFrame,
                                        &pipelineError)) {
            if (outPackets.empty()) {
                if (error != nullptr) {
                    *error = "encoded frame has no RTP packets";
                }
                return false;
            }
            return true;
        }
        if (!pipelineError.empty()) {
            if (error != nullptr) {
                *error = pipelineError;
            }
            return false;
        }
    }

    if (error != nullptr) {
        *error = "encode pending for too many attempts";
    }
    return false;
}

bool feedPacket(av::session::VideoRecvPipeline& recvPipeline,
                media::RTPReceiver& receiver,
                const std::vector<uint8_t>& bytes,
                std::unordered_set<uint32_t>& readySsrcs,
                std::string* error) {
    media::RTPPacket parsed;
    if (!receiver.parsePacket(bytes.data(), bytes.size(), parsed)) {
        if (error != nullptr) {
            *error = "failed to parse RTP packet";
        }
        return false;
    }

    av::codec::DecodedVideoFrame decoded;
    uint32_t remoteMediaSsrc = 0U;
    std::string recvError;
    const av::session::VideoRecvPacketResult result = recvPipeline.consumePacket(
        parsed,
        0U,
        decoded,
        remoteMediaSsrc,
        &recvError);
    if (result == av::session::VideoRecvPacketResult::DecodeFailed) {
        if (error != nullptr) {
            *error = recvError.empty() ? "decode failed" : recvError;
        }
        return false;
    }
    if (result == av::session::VideoRecvPacketResult::FrameReady) {
        readySsrcs.insert(remoteMediaSsrc);
    }
    return true;
}

}  // namespace

int main() {
    av::codec::VideoEncoder encoderA;
    av::codec::VideoEncoder encoderB;
    if (!encoderA.configure(kWidth, kHeight, kFrameRate, kBitrate, kPayloadType) ||
        !encoderB.configure(kWidth, kHeight, kFrameRate, kBitrate, kPayloadType)) {
        std::cerr << "SKIP video encoder configure failed" << std::endl;
        return 77;
    }

    media::RTPSender senderA(kSsrcA, 0);
    media::RTPSender senderB(kSsrcB, 0);
    std::vector<av::session::VideoSendPipelinePacket> packetsA;
    std::vector<av::session::VideoSendPipelinePacket> packetsB;
    std::string error;
    if (!encodePacketsForSsrc(encoderA, senderA, packetsA, &error)) {
        std::cerr << "stream A encode failed: " << error << std::endl;
        return 1;
    }
    if (!encodePacketsForSsrc(encoderB, senderB, packetsB, &error)) {
        std::cerr << "stream B encode failed: " << error << std::endl;
        return 1;
    }

    av::session::VideoRecvPipeline recvPipeline(
        av::session::VideoRecvPipelineConfig{kPayloadType, 96, kFrameRate});
    media::RTPReceiver receiver;
    std::unordered_set<uint32_t> readySsrcs;
    const std::size_t maxPackets = std::max(packetsA.size(), packetsB.size());
    for (std::size_t i = 0; i < maxPackets; ++i) {
        if (i < packetsA.size() &&
            !feedPacket(recvPipeline, receiver, packetsA[i].bytes, readySsrcs, &error)) {
            std::cerr << "feed stream A failed: " << error << std::endl;
            return 1;
        }
        if (i < packetsB.size() &&
            !feedPacket(recvPipeline, receiver, packetsB[i].bytes, readySsrcs, &error)) {
            std::cerr << "feed stream B failed: " << error << std::endl;
            return 1;
        }
    }
    for (int attempt = 0;
         attempt < 120 &&
         (readySsrcs.count(kSsrcA) == 0U || readySsrcs.count(kSsrcB) == 0U);
         ++attempt) {
        av::codec::DecodedVideoFrame decoded;
        uint32_t remoteMediaSsrc = 0U;
        std::string pollError;
        const av::session::VideoRecvPacketResult result =
            recvPipeline.pollDecodedFrame(decoded, remoteMediaSsrc, &pollError);
        if (result == av::session::VideoRecvPacketResult::DecodeFailed) {
            std::cerr << "poll decode failed: " << pollError << std::endl;
            return 1;
        }
        if (result == av::session::VideoRecvPacketResult::FrameReady) {
            readySsrcs.insert(remoteMediaSsrc);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (readySsrcs.count(kSsrcA) == 0U || readySsrcs.count(kSsrcB) == 0U) {
        std::cerr << "missing decoded stream: ready_count=" << readySsrcs.size() << std::endl;
        return 1;
    }
    if (recvPipeline.decodeWorkerCount() < 2U || recvPipeline.distinctDecodeThreadCount() < 2U) {
        std::cerr << "missing per-ssrc decode workers: workers="
                  << recvPipeline.decodeWorkerCount()
                  << " threads=" << recvPipeline.distinctDecodeThreadCount()
                  << std::endl;
        return 1;
    }

    return 0;
}
