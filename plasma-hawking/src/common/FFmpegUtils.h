#pragma once

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/packet.h>
}

namespace common {

struct AVPacketDeleter {
    void operator()(AVPacket* p) const {
        if (p != nullptr) {
            av_packet_free(&p);
        }
    }
};

struct AVFrameDeleter {
    void operator()(AVFrame* f) const {
        if (f != nullptr) {
            av_frame_free(&f);
        }
    }
};

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* c) const {
        if (c != nullptr) {
            avcodec_free_context(&c);
        }
    }
};

using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

inline AVPacketPtr makePacket() {
    return AVPacketPtr(av_packet_alloc());
}

inline AVFramePtr makeFrame() {
    return AVFramePtr(av_frame_alloc());
}

}  // namespace common
