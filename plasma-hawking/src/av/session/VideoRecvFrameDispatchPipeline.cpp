#include "VideoRecvFrameDispatchPipeline.h"

#include <QDebug>

namespace av::session {

void VideoRecvFrameDispatchPipeline::reportFirstDecodedFrame(
    bool& loggedFirstDecodedFrame,
    const av::codec::DecodedVideoFrame& frame,
    const std::function<void(std::string)>& statusCallback) const {
    if (loggedFirstDecodedFrame) {
        return;
    }

    loggedFirstDecodedFrame = true;
    qInfo().noquote() << "[screen-session] first frame decoded size="
                      << frame.width << "x" << frame.height
                      << "pts=" << frame.pts;
    if (statusCallback) {
        statusCallback("Video frame decoded");
    }
}

void VideoRecvFrameDispatchPipeline::dispatchFrame(
    std::function<void(av::codec::DecodedVideoFrame)>& callback,
    std::function<void(av::codec::DecodedVideoFrame, uint32_t)>& callbackWithSsrc,
    av::codec::DecodedVideoFrame&& frame,
    uint32_t remoteMediaSsrc) const {
    if (callbackWithSsrc) {
        callbackWithSsrc(std::move(frame), remoteMediaSsrc);
        return;
    }
    if (!callback) {
        return;
    }
    callback(std::move(frame));
}

}  // namespace av::session
