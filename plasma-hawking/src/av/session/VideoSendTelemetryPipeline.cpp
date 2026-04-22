#include "VideoSendTelemetryPipeline.h"

#include <QDebug>

namespace av::session {

void VideoSendTelemetryPipeline::onEncodePending(
    VideoSendTelemetryState& state,
    const std::function<void(std::string)>& statusCallback) const {
    if (state.loggedFirstEncodePending) {
        return;
    }
    state.loggedFirstEncodePending = true;
    if (statusCallback) {
        statusCallback("Video encode pending");
    }
}

void VideoSendTelemetryPipeline::onEncodeError(
    VideoSendTelemetryState& state,
    const std::string& pipelineError,
    const std::function<void(std::string)>& statusCallback) const {
    if (state.loggedFirstEncodeError) {
        return;
    }
    state.loggedFirstEncodeError = true;
    if (statusCallback) {
        statusCallback(std::string("Video encode error: ") + pipelineError);
    }
}

void VideoSendTelemetryPipeline::onEncodedPacketObserved(
    VideoSendTelemetryState& state,
    const std::function<void(std::string)>& statusCallback) const {
    if (state.loggedFirstEncodedPacket) {
        return;
    }
    state.loggedFirstEncodedPacket = true;
    if (statusCallback) {
        statusCallback("Video encoded packet observed");
    }
}

void VideoSendTelemetryPipeline::onPacketSent(
    VideoSendTelemetryState& state,
    const VideoSendPipelinePacket& packet,
    const std::function<void(std::string)>& statusCallback) const {
    if (state.loggedFirstPacket) {
        return;
    }

    state.loggedFirstPacket = true;
    qInfo().noquote() << "[screen-session] first RTP sent ts=" << packet.timestamp
                      << "bytes=" << packet.bytes.size();
    if (statusCallback) {
        statusCallback("Video RTP packet sent");
    }
}

}  // namespace av::session
