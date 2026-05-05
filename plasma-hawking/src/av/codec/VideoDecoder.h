#pragma once

#include "av/FFmpegUtils.h"
#include "av/VideoPipelineProfile.h"
#include "av/codec/VideoEncoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVBufferRef;

namespace av::codec {

struct DecodedVideoFrame {
    using SharedAvFrame = std::shared_ptr<AVFrame>;
    enum class HardwareFrameKind {
        None = 0,
        D3d11Texture2D = 1,
    };
    enum class StorageKind {
        Empty = 0,
        Software = 1,
        Hardware = 2,
    };

    int width{0};
    int height{0};
    int64_t pts{0};
    std::vector<uint8_t> yPlane;
    std::vector<uint8_t> uvPlane;
    std::vector<uint8_t> uPlane;
    std::vector<uint8_t> vPlane;
    SharedAvFrame avFrame;
    SharedAvFrame hardwareAvFrame;
    AVPixelFormat pixelFormat{AV_PIX_FMT_NONE};
    HardwareFrameKind hardwareFrameKind{HardwareFrameKind::None};
    void* hardwareTextureHandle{nullptr};
    uint32_t hardwareSubresourceIndex{0};
    StorageKind storageKind{StorageKind::Empty};
    av::VideoPipelineTelemetry telemetry{};

    void markSoftwareFrame(bool copied, bool transferredFromHardware = false) {
        storageKind = StorageKind::Software;
        hardwareAvFrame.reset();
        hardwareFrameKind = HardwareFrameKind::None;
        hardwareTextureHandle = nullptr;
        hardwareSubresourceIndex = 0U;
        telemetry.cpuFrameCopy = telemetry.cpuFrameCopy || copied;
        telemetry.cpuFrameTransfer = telemetry.cpuFrameTransfer || transferredFromHardware;
        telemetry.hardwareTextureInterop = false;
    }

    void markHardwareFrame(HardwareFrameKind kind) {
        storageKind = StorageKind::Hardware;
        avFrame.reset();
        yPlane.clear();
        uvPlane.clear();
        uPlane.clear();
        vPlane.clear();
        hardwareFrameKind = kind;
        telemetry.hardwareDecode = true;
    }

    bool isSoftwareFrame() const {
        return storageKind == StorageKind::Software ||
               (storageKind == StorageKind::Empty && containsSoftwarePayload());
    }

    bool isHardwareFrame() const {
        return storageKind == StorageKind::Hardware ||
               (storageKind == StorageKind::Empty && containsHardwarePayload() && !containsSoftwarePayload());
    }

    bool containsSoftwarePayload() const {
        return avFrame || !yPlane.empty() || !uvPlane.empty() || !uPlane.empty() || !vPlane.empty();
    }

    bool containsHardwarePayload() const {
        return hardwareFrameKind != HardwareFrameKind::None &&
               hardwareAvFrame &&
               hardwareTextureHandle != nullptr;
    }

    bool hasCpuNv12Planes() const {
        return !isHardwareFrame() && !yPlane.empty() && !uvPlane.empty();
    }

    bool hasCpuYuv420pPlanes() const {
        return !isHardwareFrame() && !yPlane.empty() && !uPlane.empty() && !vPlane.empty();
    }

    bool hasCpuPlanes() const {
        return hasCpuNv12Planes() || hasCpuYuv420pPlanes();
    }

    bool hasAvFrameNv12Planes() const {
        if (isHardwareFrame() || !avFrame || pixelFormat != AV_PIX_FMT_NV12) {
            return false;
        }
        return avFrame->data[0] != nullptr &&
               avFrame->data[1] != nullptr &&
               avFrame->linesize[0] > 0 &&
               avFrame->linesize[1] > 0;
    }

    bool hasAvFrameYuv420pPlanes() const {
        if (isHardwareFrame() ||
            !avFrame ||
            (pixelFormat != AV_PIX_FMT_YUV420P && pixelFormat != AV_PIX_FMT_YUVJ420P)) {
            return false;
        }
        return avFrame->data[0] != nullptr &&
               avFrame->data[1] != nullptr &&
               avFrame->data[2] != nullptr &&
               avFrame->linesize[0] > 0 &&
               avFrame->linesize[1] > 0 &&
               avFrame->linesize[2] > 0;
    }

    bool hasAvFramePlanes() const {
        return hasAvFrameNv12Planes() || hasAvFrameYuv420pPlanes();
    }

    bool hasRenderableData() const {
        if (width <= 0 || height <= 0) {
            return false;
        }
        if (storageKind == StorageKind::Hardware) {
            return hasHardwareTextureShareCandidate();
        }
        if (storageKind == StorageKind::Software) {
            return hasAvFramePlanes() || hasCpuPlanes();
        }
        return hasAvFramePlanes() || hasCpuPlanes() || hasHardwareTextureShareCandidate();
    }

    bool hasHardwareTextureShareCandidate() const {
        return !isSoftwareFrame() && containsHardwarePayload();
    }

    const uint8_t* yData() const {
        if (hasAvFramePlanes()) {
            return avFrame->data[0];
        }
        return hasCpuNv12Planes() || hasCpuYuv420pPlanes() ? yPlane.data() : nullptr;
    }

    const uint8_t* uvData() const {
        if (hasAvFrameNv12Planes()) {
            return avFrame->data[1];
        }
        return hasCpuNv12Planes() ? uvPlane.data() : nullptr;
    }

    const uint8_t* uData() const {
        if (hasAvFrameYuv420pPlanes()) {
            return avFrame->data[1];
        }
        if (hasCpuYuv420pPlanes()) {
            return uPlane.data();
        }
        return nullptr;
    }

    const uint8_t* vData() const {
        if (hasAvFrameYuv420pPlanes()) {
            return avFrame->data[2];
        }
        if (hasCpuYuv420pPlanes()) {
            return vPlane.data();
        }
        return nullptr;
    }

    int yStride() const {
        if (hasAvFramePlanes()) {
            return avFrame->linesize[0];
        }
        return hasCpuNv12Planes() || hasCpuYuv420pPlanes() ? width : 0;
    }

    int uvStride() const {
        if (hasAvFrameNv12Planes()) {
            return avFrame->linesize[1];
        }
        return hasCpuNv12Planes() ? width : 0;
    }

    int uStride() const {
        if (hasAvFrameYuv420pPlanes()) {
            return avFrame->linesize[1];
        }
        if (hasCpuYuv420pPlanes()) {
            return width / 2;
        }
        return 0;
    }

    int vStride() const {
        if (hasAvFrameYuv420pPlanes()) {
            return avFrame->linesize[2];
        }
        if (hasCpuYuv420pPlanes()) {
            return width / 2;
        }
        return 0;
    }
};

bool makeD3D11HardwarePreviewFrame(const AVFrame& frame, DecodedVideoFrame& outFrame);

class VideoDecoder {
public:
    explicit VideoDecoder(av::VideoPipelineProfile profile = av::videoPipelineProfileFromEnvironment());
    ~VideoDecoder();

    bool configure();
    bool decode(const EncodedVideoFrame& inFrame, DecodedVideoFrame& outFrame, std::string* error = nullptr);
    av::VideoPipelineProfile pipelineProfile() const;

private:
    struct AvBufferRefDeleter {
        void operator()(AVBufferRef* ref) const;
    };

    using AvBufferRefPtr = std::unique_ptr<AVBufferRef, AvBufferRefDeleter>;

    static AVPixelFormat selectPixelFormat(AVCodecContext* context, const AVPixelFormat* formats);
    bool configureHardwareDecode(const AVCodec* codec, AVCodecContext& context);

    av::AVCodecContextPtr m_codecContext;
    AvBufferRefPtr m_hwDeviceContext;
    AVPixelFormat m_hwPixelFormat{AV_PIX_FMT_NONE};
    bool m_forceSoftwareDecode{false};
    bool m_enableHardwareTextureShare{false};
    av::VideoPipelineProfile m_pipelineProfile{av::VideoPipelineProfile::SoftwareE2E};
};

}  // namespace av::codec
