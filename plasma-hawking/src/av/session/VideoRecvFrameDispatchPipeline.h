#pragma once

#include "av/codec/VideoDecoder.h"

#include <cstdint>
#include <functional>
#include <string>

namespace av::session {

class VideoRecvFrameDispatchPipeline {
public:
    void reportFirstDecodedFrame(bool& loggedFirstDecodedFrame,
                                 const av::codec::DecodedVideoFrame& frame,
                                 const std::function<void(std::string)>& statusCallback) const;

    void dispatchFrame(std::function<void(av::codec::DecodedVideoFrame)>& callback,
                       std::function<void(av::codec::DecodedVideoFrame, uint32_t)>& callbackWithSsrc,
                       av::codec::DecodedVideoFrame&& frame,
                       uint32_t remoteMediaSsrc) const;
};

}  // namespace av::session
