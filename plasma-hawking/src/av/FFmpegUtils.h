#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include <memory>

namespace av {

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        av_frame_free(&frame);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        av_packet_free(&packet);
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        avcodec_free_context(&ctx);
    }
};

using AVFramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using AVPacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

inline AVFramePtr makeFrame() {
    return AVFramePtr(av_frame_alloc());
}

inline AVPacketPtr makePacket() {
    return AVPacketPtr(av_packet_alloc());
}

inline AVCodecContextPtr makeCodecContext(const AVCodec* codec) {
    return AVCodecContextPtr(avcodec_alloc_context3(codec));
}

inline bool setDefaultChannelLayout(AVChannelLayout& layout, int channels) {
    if (channels <= 0) {
        return false;
    }
    av_channel_layout_uninit(&layout);
    av_channel_layout_default(&layout, channels);
    return true;
}

inline bool copyChannelLayout(AVChannelLayout& dst, const AVChannelLayout& src) {
    av_channel_layout_uninit(&dst);
    return av_channel_layout_copy(&dst, &src) == 0;
}

inline bool configureAudioFrame(AVFrame& frame,
                                int sampleRate,
                                const AVChannelLayout& channelLayout,
                                AVSampleFormat sampleFormat,
                                int nbSamples) {
    frame.nb_samples = nbSamples;
    frame.sample_rate = sampleRate;
    frame.format = static_cast<int>(sampleFormat);

    if (!copyChannelLayout(frame.ch_layout, channelLayout)) {
        return false;
    }

    return av_frame_get_buffer(&frame, 0) == 0;
}

}  // namespace av

