#pragma once

#include "av/FFmpegUtils.h"
#include "av/codec/VideoEncoder.h"

#include <memory>
#include <string>

namespace av::capture {

class WindowsD3D11CameraCapture {
public:
    WindowsD3D11CameraCapture();
    ~WindowsD3D11CameraCapture();

    WindowsD3D11CameraCapture(const WindowsD3D11CameraCapture&) = delete;
    WindowsD3D11CameraCapture& operator=(const WindowsD3D11CameraCapture&) = delete;

    bool initialize(av::codec::VideoEncoder& encoder,
                    const std::string& preferredDeviceName,
                    int targetWidth,
                    int targetHeight,
                    int frameRate,
                    std::string* error = nullptr);
    bool capture(av::codec::VideoEncoder& encoder,
                 int64_t pts,
                 av::AVFramePtr& outFrame,
                 std::string* error = nullptr);
    void shutdown();

    bool isInitialized() const;
    std::string backendName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace av::capture
