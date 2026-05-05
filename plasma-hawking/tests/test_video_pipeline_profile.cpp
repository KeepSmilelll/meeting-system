#include "av/codec/VideoDecoder.h"
#include "av/session/VideoSendPipeline.h"
#include "net/media/RTPSender.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

av::capture::ScreenFrame makeSoftwareScreenFrame() {
    av::capture::ScreenFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.pts = 1;
    frame.bgra.assign(2U * 2U * 4U, 0x80U);
    return frame;
}

}  // namespace

int main() {
    if (!require(av::parseVideoPipelineProfile("hardware") == av::VideoPipelineProfile::HardwareE2E,
                 "hardware profile parse failed") ||
        !require(av::parseVideoPipelineProfile("software") == av::VideoPipelineProfile::SoftwareE2E,
                 "software profile parse failed")) {
        return 1;
    }
#ifdef _WIN32
    if (!require(av::defaultVideoPipelineProfile() == av::VideoPipelineProfile::HardwareE2E,
                 "windows default profile should prefer hardware")) {
        return 1;
    }
#endif

    av::codec::DecodedVideoFrame hardwareFrame;
    hardwareFrame.width = 16;
    hardwareFrame.height = 16;
    hardwareFrame.yPlane = {1, 2, 3, 4};
    hardwareFrame.uvPlane = {5, 6};
    hardwareFrame.hardwareAvFrame = av::codec::DecodedVideoFrame::SharedAvFrame(
        reinterpret_cast<AVFrame*>(0x1),
        [](AVFrame*) {});
    hardwareFrame.hardwareTextureHandle = reinterpret_cast<void*>(0x2);
    hardwareFrame.markHardwareFrame(av::codec::DecodedVideoFrame::HardwareFrameKind::D3d11Texture2D);
    if (!require(hardwareFrame.isHardwareFrame(), "hardware frame not marked as hardware") ||
        !require(!hardwareFrame.hasCpuPlanes(), "hardware frame exposes CPU planes") ||
        !require(hardwareFrame.yData() == nullptr, "hardware frame exposes Y data") ||
        !require(hardwareFrame.hasHardwareTextureShareCandidate(),
                 "hardware frame lost texture share candidate")) {
        return 1;
    }

    av::codec::DecodedVideoFrame softwareFrame;
    softwareFrame.width = 2;
    softwareFrame.height = 2;
    softwareFrame.pixelFormat = AV_PIX_FMT_NV12;
    softwareFrame.yPlane = {0, 16, 32, 48};
    softwareFrame.uvPlane = {64, 96};
    softwareFrame.hardwareAvFrame = hardwareFrame.hardwareAvFrame;
    softwareFrame.hardwareTextureHandle = reinterpret_cast<void*>(0x3);
    softwareFrame.markSoftwareFrame(true);
    if (!require(softwareFrame.isSoftwareFrame(), "software frame not marked as software") ||
        !require(softwareFrame.hasCpuPlanes(), "software frame lost CPU planes") ||
        !require(!softwareFrame.hasHardwareTextureShareCandidate(),
                 "software frame exposes hardware texture candidate")) {
        return 1;
    }

    av::session::VideoSendPipeline hardwareSend(
        av::session::VideoSendPipelineConfig{
            30,
            1200,
            av::VideoPipelineProfile::HardwareE2E,
        });
    av::codec::VideoEncoder encoder;
    media::RTPSender sender(0x11111111U, 0);
    std::vector<av::session::VideoSendPipelinePacket> packets;
    std::string error;
    bool keyFrame = false;
    if (hardwareSend.encodeAndPacketize(encoder,
                                        makeSoftwareScreenFrame(),
                                        96,
                                        true,
                                        sender,
                                        packets,
                                        &keyFrame,
                                        &error)) {
        std::cerr << "hardware send accepted software screen frame" << std::endl;
        return 1;
    }
    if (!require(error.find("hardware video pipeline") != std::string::npos,
                 "hardware send rejection did not report profile boundary")) {
        return 1;
    }

    av::AVFramePtr cpuAvFrame = av::makeFrame();
    cpuAvFrame->format = AV_PIX_FMT_NV12;
    cpuAvFrame->width = 2;
    cpuAvFrame->height = 2;
    cpuAvFrame->pts = 2;
    error.clear();
    av::session::VideoSendPipelineInputFrame inputFrame;
    inputFrame.avFrame = std::move(cpuAvFrame);
    if (hardwareSend.encodeAndPacketize(encoder,
                                        inputFrame,
                                        96,
                                        true,
                                        sender,
                                        packets,
                                        &keyFrame,
                                        &error)) {
        std::cerr << "hardware send accepted software AVFrame" << std::endl;
        return 1;
    }
    if (!require(error.find("D3D11 GPU AVFrame") != std::string::npos,
                 "hardware send software AVFrame rejection did not report D3D11 boundary")) {
        return 1;
    }

    return 0;
}
