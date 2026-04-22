#pragma once

#include "av/FFmpegUtils.h"
#include "av/capture/CameraCapture.h"
#include "av/capture/ScreenCapture.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

#include <QMutex>
#include <QWaitCondition>

namespace av::session {

struct CameraFrameForEncode {
    av::AVFramePtr avFrame{nullptr};
    av::capture::ScreenFrame screenFrame;
    int64_t pts{0};

    bool hasAvFrame() const {
        return static_cast<bool>(avFrame);
    }

    bool hasScreenFrame() const {
        return !screenFrame.bgra.empty();
    }
};

class CameraFrameRelay {
public:
    enum class EnqueueDropReason {
        None,
        InvalidFrame,
        DisabledOrSharing,
        GenerationMismatch,
        Throttled,
        ConvertFailed
    };

    explicit CameraFrameRelay(int targetWidth, int targetHeight, int targetFrameRate);

    uint64_t beginCapture();
    void invalidate();
    void setCameraEnabled(bool enabled);
    void setSharingEnabled(bool enabled);

    bool popFrame(av::capture::ScreenFrame& outFrame, std::chrono::milliseconds timeout);
    bool popFrameForEncode(CameraFrameForEncode& outFrame, std::chrono::milliseconds timeout);
    bool enqueueFrame(uint64_t expectedGeneration,
                      av::capture::CameraCaptureFrame frame,
                      EnqueueDropReason* dropReason = nullptr);

private:
    struct QueuedCameraFrame {
        av::capture::CameraCaptureFrame frame;
        int64_t pts{0};
    };

    const int m_width;
    const int m_height;
    const int m_targetFrameRate;
    const std::chrono::milliseconds m_frameInterval;
    static constexpr std::size_t kCapacity = 3U;

    QMutex m_mutex;
    QWaitCondition m_cv;
    std::deque<QueuedCameraFrame> m_frames;
    std::chrono::steady_clock::time_point m_lastAcceptedAt;
    std::chrono::steady_clock::time_point m_ptsOriginAt;
    int64_t m_lastAssignedPts{-1};
    std::atomic<bool> m_cameraEnabled{false};
    std::atomic<bool> m_sharingEnabled{false};
    std::atomic<uint64_t> m_generation{0};
};

}  // namespace av::session
