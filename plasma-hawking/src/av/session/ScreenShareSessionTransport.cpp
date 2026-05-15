#include "ScreenShareSession.h"

#include "VideoRecvConsumePipeline.h"
#include "VideoRecvErrorPipeline.h"
#include "VideoRecvFrameDispatchPipeline.h"
#include "VideoRecvIngressPipeline.h"
#include "VideoRecvKeyFramePipeline.h"
#include "VideoRecvPipeline.h"
#include "VideoRecvRtcpPipeline.h"
#include "VideoRecvTelemetryPipeline.h"
#include "VideoSendControlActions.h"
#include "VideoSendLoopPipeline.h"
#include "VideoSendPipeline.h"
#include "VideoSendSourcePipeline.h"
#include "VideoSendTelemetryPipeline.h"
#include "VideoSessionStateMachine.h"

#include "av/capture/WindowsD3D11CameraCapture.h"
#include "net/media/JitterBuffer.h"
#include "net/media/RTCPHandler.h"
#include "net/media/SocketAddressUtils.h"

#include <QDebug>
#include <QByteArray>
#include <QMutexLocker>
#include <QThread>
#include <QtGlobal>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <limits>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
extern "C" {
#include <libavutil/hwcontext.h>
}
#endif

namespace av::session {
namespace {

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool looksLikeStunPacket(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 20U) {
        return false;
    }
    if ((data[0] & 0xC0U) != 0U) {
        return false;
    }
    return data[4] == 0x21U && data[5] == 0x12U && data[6] == 0xA4U && data[7] == 0x42U;
}

constexpr std::size_t kMaxRecvPeerWorkers = 16U;
constexpr std::size_t kMaxRecvPeerQueuedPackets = 64U;
constexpr std::size_t kMinRecvPeerBufferedPackets = 3U;
constexpr uint64_t kRecvPeerJitterGapTimeoutMs = 20U;
constexpr uint64_t kRecvPeerWorkerIdleTimeoutMs = 30 * 1000U;
constexpr std::size_t kRecvWorkerPollBudgetAfterPacket = 2U;
constexpr std::size_t kRecvWorkerPollBudgetWhenIdle = 2U;
constexpr std::size_t kRecvWorkerPollBudgetOnStop = 8U;
constexpr int kRecvLoopWaitTimeoutMs = 20;
constexpr uint64_t kNackRetryIntervalMs = 60U;
constexpr uint64_t kNackPliFallbackMs = 1000U;
constexpr uint32_t kNackAttemptsBeforePli = 3U;
constexpr std::size_t kMaxPendingNackRequests = 64U;
constexpr uint64_t kVideoSenderReportIntervalMs = 1000U;
constexpr int kMaxScreenShareWidth = 2560;
constexpr int kMaxScreenShareHeight = 1600;
constexpr int kMaxScreenShareFrameRate = 30;

int normalizeEvenDimension(int value) {
    value = std::max(2, value);
    value &= ~1;
    return std::max(2, value);
}

std::pair<int, int> clampScreenShareDimensions(int width, int height) {
    width = normalizeEvenDimension(width);
    height = normalizeEvenDimension(height);
    if (width <= kMaxScreenShareWidth && height <= kMaxScreenShareHeight) {
        return {width, height};
    }
    const double scale = std::min(static_cast<double>(kMaxScreenShareWidth) / static_cast<double>(width),
                                  static_cast<double>(kMaxScreenShareHeight) / static_cast<double>(height));
    const int scaledWidth = normalizeEvenDimension(static_cast<int>(std::floor(width * scale)));
    const int scaledHeight = normalizeEvenDimension(static_cast<int>(std::floor(height * scale)));
    return {std::min(kMaxScreenShareWidth, scaledWidth),
            std::min(kMaxScreenShareHeight, scaledHeight)};
}

uint8_t clampToByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint8_t lumaFromBgra(uint8_t b, uint8_t g, uint8_t r) {
    const int y = ((66 * static_cast<int>(r)) + (129 * static_cast<int>(g)) + (25 * static_cast<int>(b)) + 128) >> 8;
    return clampToByte(y + 16);
}

uint8_t chromaUFromBgra(uint8_t b, uint8_t g, uint8_t r) {
    const int u = ((-38 * static_cast<int>(r)) - (74 * static_cast<int>(g)) + (112 * static_cast<int>(b)) + 128) >> 8;
    return clampToByte(u + 128);
}

uint8_t chromaVFromBgra(uint8_t b, uint8_t g, uint8_t r) {
    const int v = ((112 * static_cast<int>(r)) - (94 * static_cast<int>(g)) - (18 * static_cast<int>(b)) + 128) >> 8;
    return clampToByte(v + 128);
}

bool makeSoftwarePreviewFrame(const av::capture::ScreenFrame& frame,
                              av::codec::DecodedVideoFrame& outFrame) {
    if (frame.width <= 0 || frame.height <= 0 ||
        (frame.width % 2) != 0 || (frame.height % 2) != 0) {
        return false;
    }
    const std::size_t expectedBytes =
        static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4U;
    if (frame.bgra.size() != expectedBytes) {
        return false;
    }

    outFrame = av::codec::DecodedVideoFrame{};
    outFrame.width = frame.width;
    outFrame.height = frame.height;
    outFrame.pts = frame.pts;
    outFrame.pixelFormat = AV_PIX_FMT_NV12;
    outFrame.yPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
    outFrame.uvPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height / 2));

    const int sourceStride = frame.width * 4;
    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* srcRow = frame.bgra.data() + static_cast<std::size_t>(y) * sourceStride;
        uint8_t* yRow = outFrame.yPlane.data() + static_cast<std::size_t>(y) * frame.width;
        for (int x = 0; x < frame.width; ++x) {
            const uint8_t* pixel = srcRow + static_cast<std::ptrdiff_t>(x) * 4;
            yRow[x] = lumaFromBgra(pixel[0], pixel[1], pixel[2]);
        }
    }
    for (int y = 0; y < frame.height; y += 2) {
        const uint8_t* row0 = frame.bgra.data() + static_cast<std::size_t>(y) * sourceStride;
        const uint8_t* row1 = frame.bgra.data() + static_cast<std::size_t>(std::min(y + 1, frame.height - 1)) * sourceStride;
        uint8_t* uvRow = outFrame.uvPlane.data() + static_cast<std::size_t>(y / 2) * frame.width;
        for (int x = 0; x < frame.width; x += 2) {
            const uint8_t* p00 = row0 + static_cast<std::ptrdiff_t>(x) * 4;
            const uint8_t* p01 = row0 + static_cast<std::ptrdiff_t>(std::min(x + 1, frame.width - 1)) * 4;
            const uint8_t* p10 = row1 + static_cast<std::ptrdiff_t>(x) * 4;
            const uint8_t* p11 = row1 + static_cast<std::ptrdiff_t>(std::min(x + 1, frame.width - 1)) * 4;
            uvRow[x] = clampToByte((static_cast<int>(chromaUFromBgra(p00[0], p00[1], p00[2])) +
                                    static_cast<int>(chromaUFromBgra(p01[0], p01[1], p01[2])) +
                                    static_cast<int>(chromaUFromBgra(p10[0], p10[1], p10[2])) +
                                    static_cast<int>(chromaUFromBgra(p11[0], p11[1], p11[2])) + 2) /
                                   4);
            uvRow[x + 1] = clampToByte((static_cast<int>(chromaVFromBgra(p00[0], p00[1], p00[2])) +
                                        static_cast<int>(chromaVFromBgra(p01[0], p01[1], p01[2])) +
                                        static_cast<int>(chromaVFromBgra(p10[0], p10[1], p10[2])) +
                                        static_cast<int>(chromaVFromBgra(p11[0], p11[1], p11[2])) + 2) /
                                       4);
        }
    }
    outFrame.telemetry.profile = av::VideoPipelineProfile::SoftwareE2E;
    outFrame.telemetry.cpuFrameCopy = true;
    outFrame.telemetry.backendName = "dxgi-readback-screen-preview";
    outFrame.markSoftwareFrame(true, true);
    return true;
}

#ifdef _WIN32
std::string hresultString(HRESULT hr) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(hr));
    return buffer;
}

const char* dxgiFormatName(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_NV12:
        return "NV12";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT";
    default:
        return "other";
    }
}

std::string textureDescString(const D3D11_TEXTURE2D_DESC& desc) {
    char buffer[256]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "format=%s(%u) size=%ux%u bind=0x%08x usage=%u array=%u mips=%u sample=%u misc=0x%08x",
                  dxgiFormatName(desc.Format),
                  static_cast<unsigned>(desc.Format),
                  desc.Width,
                  desc.Height,
                  desc.BindFlags,
                  static_cast<unsigned>(desc.Usage),
                  desc.ArraySize,
                  desc.MipLevels,
                  desc.SampleDesc.Count,
                  desc.MiscFlags);
    return buffer;
}

template <typename T>
void releaseComObject(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

class WindowsD3D11ScreenCapture final {
public:
    WindowsD3D11ScreenCapture() = default;
    ~WindowsD3D11ScreenCapture() {
        shutdown();
    }

    WindowsD3D11ScreenCapture(const WindowsD3D11ScreenCapture&) = delete;
    WindowsD3D11ScreenCapture& operator=(const WindowsD3D11ScreenCapture&) = delete;

    bool initialize(av::codec::VideoEncoder& encoder, int targetWidth, int targetHeight, std::string* error) {
        shutdown();
        m_targetWidth = std::max(2, targetWidth & ~1);
        m_targetHeight = std::max(2, targetHeight & ~1);
        m_device = static_cast<ID3D11Device*>(encoder.d3d11Device());
        if (m_device == nullptr || encoder.hardwareFramesContext() == nullptr) {
            setError(error, "hardware screen capture requires D3D11 encoder device");
            return false;
        }
        m_device->AddRef();
        m_device->GetImmediateContext(&m_context);
        if (m_context == nullptr) {
            setError(error, "D3D11 immediate context unavailable");
            shutdown();
            return false;
        }
        if (FAILED(m_device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&m_videoDevice))) ||
            m_videoDevice == nullptr ||
            FAILED(m_context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&m_videoContext))) ||
            m_videoContext == nullptr) {
            setError(error, "D3D11 video processor interfaces unavailable");
            shutdown();
            return false;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIOutput* output = nullptr;
        IDXGIOutput1* output1 = nullptr;
        IDXGIOutput5* output5 = nullptr;
        HRESULT hr = m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice != nullptr) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        if (SUCCEEDED(hr) && adapter != nullptr) {
            hr = adapter->EnumOutputs(0, &output);
        }
        if (SUCCEEDED(hr) && output != nullptr) {
            DXGI_OUTPUT_DESC outputDesc{};
            if (SUCCEEDED(output->GetDesc(&outputDesc))) {
                m_sourceWidth = std::max<LONG>(1, outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left);
                m_sourceHeight = std::max<LONG>(1, outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);
            }
            hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        }
        if (SUCCEEDED(hr) && output1 != nullptr) {
            bool duplicateOutput1Attempted = false;
            bool duplicateOutput1Succeeded = false;
            if (SUCCEEDED(output1->QueryInterface(__uuidof(IDXGIOutput5),
                                                  reinterpret_cast<void**>(&output5))) &&
                output5 != nullptr) {
                duplicateOutput1Attempted = true;
                const DXGI_FORMAT requestedFormats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
                hr = output5->DuplicateOutput1(m_device,
                                               0,
                                               1U,
                                               requestedFormats,
                                               &m_duplication);
                duplicateOutput1Succeeded = SUCCEEDED(hr) && m_duplication != nullptr;
            }
            if (FAILED(hr) || m_duplication == nullptr) {
                releaseComObject(m_duplication);
                hr = output1->DuplicateOutput(m_device, &m_duplication);
            }
            m_duplicateOutput1Attempted = duplicateOutput1Attempted;
            m_duplicateOutput1Succeeded = duplicateOutput1Succeeded;
        }
        releaseComObject(output5);
        releaseComObject(output1);
        releaseComObject(output);
        releaseComObject(adapter);
        releaseComObject(dxgiDevice);
        if (FAILED(hr) || m_duplication == nullptr) {
            setError(error, "DXGI desktop duplication unavailable: " + hresultString(hr));
            shutdown();
            return false;
        }

        if (!ensureVideoProcessor(std::max(1, m_sourceWidth), std::max(1, m_sourceHeight), error)) {
            shutdown();
            return false;
        }
        return true;
    }

    bool capture(av::codec::VideoEncoder& encoder,
                 int64_t pts,
                 av::AVFramePtr& outFrame,
                 std::string* error) {
        if ((m_duplication == nullptr ||
             m_targetWidth != std::max(2, encoder.width() & ~1) ||
             m_targetHeight != std::max(2, encoder.height() & ~1)) &&
            !initialize(encoder, encoder.width(), encoder.height(), error)) {
            return false;
        }

        outFrame = av::makeFrame();
        if (!outFrame) {
            setError(error, "hardware frame allocation failed");
            return false;
        }
        if (av_hwframe_get_buffer(encoder.hardwareFramesContext(), outFrame.get(), 0) < 0) {
            outFrame.reset();
            setError(error, "D3D11 hardware frame pool allocation failed");
            return false;
        }
        outFrame->pts = pts;
        outFrame->width = encoder.width();
        outFrame->height = encoder.height();

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        IDXGIResource* resource = nullptr;
        HRESULT hr = m_duplication->AcquireNextFrame(33, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            outFrame.reset();
            if (error != nullptr) {
                error->clear();
            }
            return false;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            outFrame.reset();
            shutdown();
            setError(error, "DXGI desktop duplication access lost");
            return false;
        }
        if (FAILED(hr) || resource == nullptr) {
            outFrame.reset();
            setError(error, "DXGI AcquireNextFrame failed: " + hresultString(hr));
            return false;
        }

        ID3D11Texture2D* sourceTexture = nullptr;
        hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&sourceTexture));
        releaseComObject(resource);
        if (FAILED(hr) || sourceTexture == nullptr) {
            m_duplication->ReleaseFrame();
            outFrame.reset();
            setError(error, "DXGI frame is not a D3D11 texture");
            return false;
        }

        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceTexture->GetDesc(&sourceDesc);
        bool converted = false;
        if (ensureVideoProcessor(static_cast<int>(sourceDesc.Width), static_cast<int>(sourceDesc.Height), error)) {
            converted = blitToEncoderFrame(sourceTexture, *outFrame, error);
        }
        releaseComObject(sourceTexture);
        m_duplication->ReleaseFrame();
        if (!converted) {
            outFrame.reset();
            return false;
        }
        return true;
    }

private:
    static void setError(std::string* error, std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
    }

    bool ensureVideoProcessor(int sourceWidth, int sourceHeight, std::string* error) {
        sourceWidth = std::max(1, sourceWidth);
        sourceHeight = std::max(1, sourceHeight);
        if (m_videoProcessor != nullptr &&
            m_processorEnumerator != nullptr &&
            m_sourceWidth == sourceWidth &&
            m_sourceHeight == sourceHeight) {
            return true;
        }
        releaseComObject(m_videoProcessor);
        releaseComObject(m_processorEnumerator);
        m_sourceWidth = sourceWidth;
        m_sourceHeight = sourceHeight;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = static_cast<UINT>(sourceWidth);
        contentDesc.InputHeight = static_cast<UINT>(sourceHeight);
        contentDesc.OutputWidth = static_cast<UINT>(m_targetWidth);
        contentDesc.OutputHeight = static_cast<UINT>(m_targetHeight);
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &m_processorEnumerator);
        if (FAILED(hr) || m_processorEnumerator == nullptr) {
            setError(error, "CreateVideoProcessorEnumerator failed: " + hresultString(hr));
            return false;
        }
        hr = m_videoDevice->CreateVideoProcessor(m_processorEnumerator, 0, &m_videoProcessor);
        if (FAILED(hr) || m_videoProcessor == nullptr) {
            setError(error, "CreateVideoProcessor failed: " + hresultString(hr));
            return false;
        }
        return true;
    }

    bool blitToEncoderFrame(ID3D11Texture2D* sourceTexture, AVFrame& outFrame, std::string* error) {
        auto* destTexture = reinterpret_cast<ID3D11Texture2D*>(outFrame.data[0]);
        if (sourceTexture == nullptr || destTexture == nullptr || outFrame.hw_frames_ctx == nullptr) {
            setError(error, "invalid D3D11 hardware frame textures");
            return false;
        }

        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceTexture->GetDesc(&sourceDesc);
        UINT sourceFormatSupport = 0;
        if (m_processorEnumerator != nullptr) {
            (void)m_processorEnumerator->CheckVideoProcessorFormat(sourceDesc.Format, &sourceFormatSupport);
        }
        if ((sourceFormatSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0U) {
            setError(error,
                "DXGI desktop duplication format unsupported by video processor: " +
                textureDescString(sourceDesc) +
                " formatSupport=0x" + formatSupportString(sourceFormatSupport) +
                " duplicateOutput1Attempted=" + (m_duplicateOutput1Attempted ? "1" : "0") +
                " duplicateOutput1Succeeded=" + (m_duplicateOutput1Succeeded ? "1" : "0"));
            return false;
        }
        if (!ensureInputTexture(sourceDesc, error)) {
            return false;
        }
        m_context->CopySubresourceRegion(m_inputTexture, 0, 0, 0, 0, sourceTexture, 0, nullptr);
        D3D11_TEXTURE2D_DESC inputTextureDesc{};
        m_inputTexture->GetDesc(&inputTextureDesc);
        UINT inputFormatSupport = 0;
        if (m_processorEnumerator != nullptr) {
            (void)m_processorEnumerator->CheckVideoProcessorFormat(inputTextureDesc.Format, &inputFormatSupport);
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;
        inputDesc.Texture2D.ArraySlice = 0;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        outputDesc.Texture2DArray.MipSlice = 0;
        outputDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(reinterpret_cast<std::uintptr_t>(outFrame.data[1]));
        outputDesc.Texture2DArray.ArraySize = 1;

        ID3D11VideoProcessorInputView* inputView = nullptr;
        ID3D11VideoProcessorOutputView* outputView = nullptr;
        HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(m_inputTexture,
                                                                  m_processorEnumerator,
                                                                  &inputDesc,
                                                                  &inputView);
        if (FAILED(hr) || inputView == nullptr) {
            setError(error,
                "CreateVideoProcessorInputView failed: " + hresultString(hr) +
                " source{" + textureDescString(sourceDesc) + "}" +
                " input{" + textureDescString(inputTextureDesc) + "}" +
                " formatSupport=0x" + [&]() {
                    char support[16]{};
                    std::snprintf(support, sizeof(support), "%08x", inputFormatSupport);
                    return std::string(support);
                }());
            return false;
        }
        hr = m_videoDevice->CreateVideoProcessorOutputView(destTexture,
                                                           m_processorEnumerator,
                                                           &outputDesc,
                                                           &outputView);
        if (FAILED(hr) || outputView == nullptr) {
            releaseComObject(inputView);
            setError(error, "CreateVideoProcessorOutputView failed: " + hresultString(hr));
            return false;
        }

        RECT sourceRect{0, 0, m_sourceWidth, m_sourceHeight};
        RECT targetRect{0, 0, m_targetWidth, m_targetHeight};
        m_videoContext->VideoProcessorSetStreamSourceRect(m_videoProcessor, 0, TRUE, &sourceRect);
        m_videoContext->VideoProcessorSetStreamDestRect(m_videoProcessor, 0, TRUE, &targetRect);
        m_videoContext->VideoProcessorSetOutputTargetRect(m_videoProcessor, TRUE, &targetRect);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inputView;
        hr = m_videoContext->VideoProcessorBlt(m_videoProcessor, outputView, 0, 1, &stream);
        releaseComObject(outputView);
        releaseComObject(inputView);
        if (FAILED(hr)) {
            setError(error, "VideoProcessorBlt desktop frame failed: " + hresultString(hr));
            return false;
        }
        return true;
    }

    bool ensureInputTexture(const D3D11_TEXTURE2D_DESC& sourceDesc, std::string* error) {
        D3D11_TEXTURE2D_DESC existingDesc{};
        if (m_inputTexture != nullptr) {
            m_inputTexture->GetDesc(&existingDesc);
            if (existingDesc.Width == sourceDesc.Width &&
                existingDesc.Height == sourceDesc.Height &&
                existingDesc.Format == sourceDesc.Format) {
                return true;
            }
            releaseComObject(m_inputTexture);
        }

        D3D11_TEXTURE2D_DESC inputDesc{};
        inputDesc.Width = sourceDesc.Width;
        inputDesc.Height = sourceDesc.Height;
        inputDesc.MipLevels = 1;
        inputDesc.ArraySize = 1;
        inputDesc.Format = sourceDesc.Format;
        inputDesc.SampleDesc.Count = 1;
        inputDesc.Usage = D3D11_USAGE_DEFAULT;
        inputDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = m_device->CreateTexture2D(&inputDesc, nullptr, &m_inputTexture);
        if (FAILED(hr) || m_inputTexture == nullptr) {
            setError(error, "CreateTexture2D desktop video processor input failed: " + hresultString(hr));
            return false;
        }
        return true;
    }

    static std::string formatSupportString(UINT supportValue) {
        char support[16]{};
        std::snprintf(support, sizeof(support), "%08x", supportValue);
        return std::string(support);
    }

    void shutdown() {
        releaseComObject(m_videoProcessor);
        releaseComObject(m_processorEnumerator);
        releaseComObject(m_inputTexture);
        releaseComObject(m_duplication);
        releaseComObject(m_videoContext);
        releaseComObject(m_videoDevice);
        releaseComObject(m_context);
        releaseComObject(m_device);
        m_sourceWidth = 0;
        m_sourceHeight = 0;
        m_duplicateOutput1Attempted = false;
        m_duplicateOutput1Succeeded = false;
    }

    ID3D11Device* m_device{nullptr};
    ID3D11DeviceContext* m_context{nullptr};
    ID3D11VideoDevice* m_videoDevice{nullptr};
    ID3D11VideoContext* m_videoContext{nullptr};
    IDXGIOutputDuplication* m_duplication{nullptr};
    ID3D11VideoProcessorEnumerator* m_processorEnumerator{nullptr};
    ID3D11VideoProcessor* m_videoProcessor{nullptr};
    ID3D11Texture2D* m_inputTexture{nullptr};
    int m_sourceWidth{0};
    int m_sourceHeight{0};
    int m_targetWidth{0};
    int m_targetHeight{0};
    bool m_duplicateOutput1Attempted{false};
    bool m_duplicateOutput1Succeeded{false};
};

constexpr const char* kGpuScaleVertexShaderHlsl = R"hlsl(
void main(uint id : SV_VertexID,
          out float4 pos : SV_Position,
          out float2 uv  : TEXCOORD0) {
    uv  = float2((id << 1) & 2, id & 2);
    pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
)hlsl";

constexpr const char* kGpuScalePixelShaderHlsl = R"hlsl(
Texture2D    tex  : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(samp, uv);
}
)hlsl";

// BT.601 BGRA -> Y luma. GPU samples BGRA texture as RGBA, so r=R, g=G, b=B.
constexpr const char* kBgraToYShaderHlsl = R"hlsl(
Texture2D    tex  : register(t0);
SamplerState samp : register(s0);
float main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c = tex.Sample(samp, uv);
    return 16.0/255.0 + c.r * (65.481/255.0) + c.g * (128.553/255.0) + c.b * (24.966/255.0);
}
)hlsl";

// BT.601 BGRA -> UV chroma at half resolution. Point-samples 2x2 block and averages.
constexpr const char* kBgraToUvShaderHlsl = R"hlsl(
Texture2D tex  : register(t0);
cbuffer cb : register(b0) { float2 texelSize; };
float2 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 half_texel = texelSize * 0.5;
    float4 c00 = tex.Sample(SamplerState_dummy, uv + float2(-half_texel.x, -half_texel.y));
    float4 c10 = tex.Sample(SamplerState_dummy, uv + float2( half_texel.x, -half_texel.y));
    float4 c01 = tex.Sample(SamplerState_dummy, uv + float2(-half_texel.x,  half_texel.y));
    float4 c11 = tex.Sample(SamplerState_dummy, uv + float2( half_texel.x,  half_texel.y));
    float4 c = (c00 + c10 + c01 + c11) * 0.25;
    float u = 128.0/255.0 + c.r * (-37.797/255.0) + c.g * (-74.203/255.0) + c.b * (112.0/255.0);
    float v = 128.0/255.0 + c.r * (112.0/255.0)   + c.g * (-93.786/255.0) + c.b * (-18.214/255.0);
    return float2(u, v);
}
)hlsl";

// Simplified UV shader that uses bilinear hardware filtering (the sampler does the 2x2 average).
constexpr const char* kBgraToUvSimpleShaderHlsl = R"hlsl(
Texture2D    tex  : register(t0);
SamplerState samp : register(s0);
float2 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c = tex.Sample(samp, uv);
    float u = 128.0/255.0 + c.r * (-37.797/255.0) + c.g * (-74.203/255.0) + c.b * (112.0/255.0);
    float v = 128.0/255.0 + c.r * (112.0/255.0)   + c.g * (-93.786/255.0) + c.b * (-18.214/255.0);
    return float2(u, v);
}
)hlsl";

bool compileHlslShader(const char* source, const char* target, const char* entryPoint,
                       ID3DBlob** blob, std::string* error) {
    ID3DBlob* errorBlob = nullptr;
    HRESULT hr = D3DCompile(source,
                            std::strlen(source),
                            nullptr, nullptr, nullptr,
                            entryPoint, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            blob, &errorBlob);
    if (FAILED(hr)) {
        if (error != nullptr && errorBlob != nullptr) {
            *error = std::string("shader compile failed: ") +
                     std::string(static_cast<const char*>(errorBlob->GetBufferPointer()),
                                 errorBlob->GetBufferSize());
        }
        releaseComObject(errorBlob);
        return false;
    }
    releaseComObject(errorBlob);
    return *blob != nullptr;
}

class WindowsD3D11ScreenReadbackCapture final {
public:
    WindowsD3D11ScreenReadbackCapture() = default;
    ~WindowsD3D11ScreenReadbackCapture() {
        shutdown();
    }

    WindowsD3D11ScreenReadbackCapture(const WindowsD3D11ScreenReadbackCapture&) = delete;
    WindowsD3D11ScreenReadbackCapture& operator=(const WindowsD3D11ScreenReadbackCapture&) = delete;

    bool initialize(int targetWidth, int targetHeight, std::string* error) {
        shutdown();
        m_targetWidth = std::max(2, targetWidth & ~1);
        m_targetHeight = std::max(2, targetHeight & ~1);

        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL selectedFeatureLevel{};
        HRESULT hr = D3D11CreateDevice(nullptr,
                                       D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       featureLevels,
                                       static_cast<UINT>(std::size(featureLevels)),
                                       D3D11_SDK_VERSION,
                                       &m_device,
                                       &selectedFeatureLevel,
                                       &m_context);
        if (FAILED(hr) || m_device == nullptr || m_context == nullptr) {
            setError(error, "D3D11 readback device create failed: " + hresultString(hr));
            shutdown();
            return false;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIOutput* output = nullptr;
        IDXGIOutput1* output1 = nullptr;
        IDXGIOutput5* output5 = nullptr;
        hr = m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice != nullptr) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        if (SUCCEEDED(hr) && adapter != nullptr) {
            hr = adapter->EnumOutputs(0, &output);
        }
        if (SUCCEEDED(hr) && output != nullptr) {
            DXGI_OUTPUT_DESC outputDesc{};
            if (SUCCEEDED(output->GetDesc(&outputDesc))) {
                m_sourceWidth = std::max<LONG>(1, outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left);
                m_sourceHeight = std::max<LONG>(1, outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);
            }
            hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        }
        if (SUCCEEDED(hr) && output1 != nullptr) {
            if (SUCCEEDED(output1->QueryInterface(__uuidof(IDXGIOutput5),
                                                  reinterpret_cast<void**>(&output5))) &&
                output5 != nullptr) {
                const DXGI_FORMAT requestedFormats[] = {
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                };
                hr = output5->DuplicateOutput1(m_device,
                                               0,
                                               static_cast<UINT>(std::size(requestedFormats)),
                                               requestedFormats,
                                               &m_duplication);
            }
            if (FAILED(hr) || m_duplication == nullptr) {
                releaseComObject(m_duplication);
                hr = output1->DuplicateOutput(m_device, &m_duplication);
            }
        }
        releaseComObject(output5);
        releaseComObject(output1);
        releaseComObject(output);
        releaseComObject(adapter);
        releaseComObject(dxgiDevice);
        if (FAILED(hr) || m_duplication == nullptr) {
            setError(error, "DXGI readback desktop duplication unavailable: " + hresultString(hr));
            shutdown();
            return false;
        }

        initGpuScalePipeline();
        initNv12Pipeline();
        return true;
    }

    bool capture(int targetWidth,
                 int targetHeight,
                 int64_t pts,
                 av::capture::ScreenFrame& outFrame,
                 std::string* error) {
        outFrame = av::capture::ScreenFrame{};
        if ((m_duplication == nullptr ||
             m_targetWidth != std::max(2, targetWidth & ~1) ||
             m_targetHeight != std::max(2, targetHeight & ~1)) &&
            !initialize(targetWidth, targetHeight, error)) {
            return false;
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        IDXGIResource* resource = nullptr;
        HRESULT hr = m_duplication->AcquireNextFrame(33, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (error != nullptr) {
                error->clear();
            }
            return false;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            shutdown();
            setError(error, "DXGI readback desktop duplication access lost");
            return false;
        }
        if (FAILED(hr) || resource == nullptr) {
            setError(error, "DXGI readback AcquireNextFrame failed: " + hresultString(hr));
            return false;
        }

        ID3D11Texture2D* sourceTexture = nullptr;
        hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&sourceTexture));
        releaseComObject(resource);
        if (FAILED(hr) || sourceTexture == nullptr) {
            m_duplication->ReleaseFrame();
            setError(error, "DXGI readback frame is not a D3D11 texture");
            return false;
        }

        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceTexture->GetDesc(&sourceDesc);
        bool converted = false;

        // GPU scaling path: scale on GPU then download the smaller texture.
        if (m_gpuScaleReady && gpuScaleBlit(sourceTexture, sourceDesc, error)) {
            D3D11_TEXTURE2D_DESC scaledDesc{};
            m_scaledTexture->GetDesc(&scaledDesc);
            if (ensureStagingTexture(scaledDesc, error)) {
                m_context->CopyResource(m_stagingTexture, m_scaledTexture);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = m_context->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    converted = convertMappedFrame(scaledDesc, mapped, pts, outFrame, error);
                    m_context->Unmap(m_stagingTexture, 0);
                } else {
                    setError(error, "DXGI readback staging map failed: " + hresultString(hr));
                }
            }
            // GPU NV12 conversion: produce NV12 planes for the encoder.
            if (converted && m_nv12Ready) {
                gpuNv12Convert(outFrame);
            }
        }

        // CPU fallback: download full source and scale on CPU.
        if (!converted) {
            if (ensureStagingTexture(sourceDesc, error)) {
                m_context->CopyResource(m_stagingTexture, sourceTexture);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = m_context->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    converted = convertMappedFrame(sourceDesc, mapped, pts, outFrame, error);
                    m_context->Unmap(m_stagingTexture, 0);
                } else {
                    setError(error, "DXGI readback staging map failed: " + hresultString(hr));
                }
            }
        }

        releaseComObject(sourceTexture);
        m_duplication->ReleaseFrame();
        return converted;
    }

private:
    static void setError(std::string* error, std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
    }

    static uint8_t clampFloatToByte(float value) {
        if (!std::isfinite(value)) {
            return 0U;
        }
        const float clamped = std::max(0.0f, std::min(1.0f, value));
        return static_cast<uint8_t>(clamped * 255.0f + 0.5f);
    }

    static float halfToFloat(uint16_t value) {
        const uint16_t exponent = static_cast<uint16_t>((value >> 10U) & 0x1FU);
        const uint16_t mantissa = static_cast<uint16_t>(value & 0x03FFU);
        const float sign = (value & 0x8000U) != 0U ? -1.0f : 1.0f;
        if (exponent == 0U) {
            return mantissa == 0U ? sign * 0.0f : sign * std::ldexp(static_cast<float>(mantissa), -24);
        }
        if (exponent == 0x1FU) {
            return sign * 1.0f;
        }
        return sign * std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f,
                                 static_cast<int>(exponent) - 15);
    }

    static uint16_t readLe16(const uint8_t* data) {
        return static_cast<uint16_t>(data[0]) |
               static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
    }

    bool ensureStagingTexture(const D3D11_TEXTURE2D_DESC& sourceDesc, std::string* error) {
        D3D11_TEXTURE2D_DESC existingDesc{};
        if (m_stagingTexture != nullptr) {
            m_stagingTexture->GetDesc(&existingDesc);
            if (existingDesc.Width == sourceDesc.Width &&
                existingDesc.Height == sourceDesc.Height &&
                existingDesc.Format == sourceDesc.Format) {
                return true;
            }
            releaseComObject(m_stagingTexture);
        }

        D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        HRESULT hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTexture);
        if (FAILED(hr) || m_stagingTexture == nullptr) {
            setError(error, "DXGI readback staging texture create failed: " + hresultString(hr));
            return false;
        }
        return true;
    }

    bool convertMappedFrame(const D3D11_TEXTURE2D_DESC& sourceDesc,
                            const D3D11_MAPPED_SUBRESOURCE& mapped,
                            int64_t pts,
                            av::capture::ScreenFrame& outFrame,
                            std::string* error) const {
        if (mapped.pData == nullptr || sourceDesc.Width == 0U || sourceDesc.Height == 0U) {
            setError(error, "DXGI readback mapped frame is empty");
            return false;
        }
        if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            sourceDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            setError(error, "DXGI readback unsupported source format: " + textureDescString(sourceDesc));
            return false;
        }

        outFrame.width = m_targetWidth;
        outFrame.height = m_targetHeight;
        outFrame.pts = pts;
        const std::size_t targetBytes = static_cast<std::size_t>(m_targetWidth) *
                                        static_cast<std::size_t>(m_targetHeight) * 4U;
        outFrame.bgra.resize(targetBytes);

        // Fast path: GPU-scaled BGRA at target resolution — row-by-row memcpy.
        if (sourceDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
            sourceDesc.Width == static_cast<UINT>(m_targetWidth) &&
            sourceDesc.Height == static_cast<UINT>(m_targetHeight)) {
            const auto* sourceBase = static_cast<const uint8_t*>(mapped.pData);
            const std::size_t rowBytes = static_cast<std::size_t>(m_targetWidth) * 4U;
            for (int y = 0; y < m_targetHeight; ++y) {
                std::memcpy(outFrame.bgra.data() + static_cast<std::size_t>(y) * rowBytes,
                            sourceBase + static_cast<std::size_t>(y) * mapped.RowPitch,
                            rowBytes);
            }
            return true;
        }

        // Slow path: CPU scaling + format conversion for non-BGRA or mismatched sizes.
        const auto* sourceBase = static_cast<const uint8_t*>(mapped.pData);
        const UINT sourceWidth = sourceDesc.Width;
        const UINT sourceHeight = sourceDesc.Height;
        const UINT bytesPerPixel = sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8U : 4U;
        ensureSampleMaps(sourceWidth, sourceHeight);

        for (int y = 0; y < m_targetHeight; ++y) {
            const UINT sourceY = m_sourceYMap[static_cast<std::size_t>(y)];
            const uint8_t* sourceRow = sourceBase + static_cast<std::size_t>(sourceY) * mapped.RowPitch;
            uint8_t* destRow = outFrame.bgra.data() +
                static_cast<std::size_t>(y) * static_cast<std::size_t>(m_targetWidth) * 4U;
            for (int x = 0; x < m_targetWidth; ++x) {
                const UINT sourceX = m_sourceXMap[static_cast<std::size_t>(x)];
                const uint8_t* pixel = sourceRow + static_cast<std::size_t>(sourceX) * bytesPerPixel;
                uint8_t* dest = destRow + static_cast<std::size_t>(x) * 4U;
                if (sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    const uint8_t r = clampFloatToByte(halfToFloat(readLe16(pixel + 0)));
                    const uint8_t g = clampFloatToByte(halfToFloat(readLe16(pixel + 2)));
                    const uint8_t b = clampFloatToByte(halfToFloat(readLe16(pixel + 4)));
                    dest[0] = b;
                    dest[1] = g;
                    dest[2] = r;
                    dest[3] = 0xFF;
                } else if (sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    dest[0] = pixel[2];
                    dest[1] = pixel[1];
                    dest[2] = pixel[0];
                    dest[3] = 0xFF;
                } else {
                    dest[0] = pixel[0];
                    dest[1] = pixel[1];
                    dest[2] = pixel[2];
                    dest[3] = 0xFF;
                }
            }
        }
        return true;
    }

    void initGpuScalePipeline() {
        m_gpuScaleReady = false;
        if (m_device == nullptr) {
            return;
        }
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        std::string compileError;
        if (!compileHlslShader(kGpuScaleVertexShaderHlsl, "vs_4_0", "main", &vsBlob, &compileError) ||
            !compileHlslShader(kGpuScalePixelShaderHlsl, "ps_4_0", "main", &psBlob, &compileError)) {
            releaseComObject(vsBlob);
            releaseComObject(psBlob);
            return;
        }

        HRESULT hr = m_device->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
        releaseComObject(vsBlob);
        if (FAILED(hr) || m_vertexShader == nullptr) {
            releaseComObject(psBlob);
            return;
        }

        hr = m_device->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
        releaseComObject(psBlob);
        if (FAILED(hr) || m_pixelShader == nullptr) {
            return;
        }

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = m_device->CreateSamplerState(&samplerDesc, &m_sampler);
        if (FAILED(hr) || m_sampler == nullptr) {
            return;
        }

        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        hr = m_device->CreateRasterizerState(&rasterDesc, &m_rasterizerState);
        if (FAILED(hr) || m_rasterizerState == nullptr) {
            return;
        }

        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = m_device->CreateBlendState(&blendDesc, &m_blendState);
        if (FAILED(hr) || m_blendState == nullptr) {
            return;
        }

        D3D11_DEPTH_STENCIL_DESC dsDesc{};
        dsDesc.DepthEnable = FALSE;
        dsDesc.StencilEnable = FALSE;
        hr = m_device->CreateDepthStencilState(&dsDesc, &m_depthStencilState);
        if (FAILED(hr) || m_depthStencilState == nullptr) {
            return;
        }

        m_gpuScaleReady = true;
    }

    bool ensureGpuScaleTextures(UINT sourceWidth, UINT sourceHeight,
                                DXGI_FORMAT sourceFormat, std::string* error) {
        // Recreate source copy texture if source dimensions/format changed.
        if (m_gpuSourceTexture != nullptr) {
            D3D11_TEXTURE2D_DESC existing{};
            m_gpuSourceTexture->GetDesc(&existing);
            if (existing.Width != sourceWidth ||
                existing.Height != sourceHeight ||
                existing.Format != sourceFormat) {
                releaseComObject(m_gpuSourceSrv);
                releaseComObject(m_gpuSourceTexture);
            }
        }
        if (m_gpuSourceTexture == nullptr) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = sourceWidth;
            desc.Height = sourceHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = sourceFormat;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_gpuSourceTexture);
            if (FAILED(hr) || m_gpuSourceTexture == nullptr) {
                setError(error, "GPU scale source texture create failed: " + hresultString(hr));
                return false;
            }
            hr = m_device->CreateShaderResourceView(m_gpuSourceTexture, nullptr, &m_gpuSourceSrv);
            if (FAILED(hr) || m_gpuSourceSrv == nullptr) {
                setError(error, "GPU scale source SRV create failed: " + hresultString(hr));
                return false;
            }
        }

        // Recreate scaled render target if target dimensions changed.
        if (m_scaledTexture != nullptr) {
            D3D11_TEXTURE2D_DESC existing{};
            m_scaledTexture->GetDesc(&existing);
            if (existing.Width != static_cast<UINT>(m_targetWidth) ||
                existing.Height != static_cast<UINT>(m_targetHeight)) {
                releaseComObject(m_scaledRtv);
                releaseComObject(m_scaledTexture);
            }
        }
        if (m_scaledTexture == nullptr) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(m_targetWidth);
            desc.Height = static_cast<UINT>(m_targetHeight);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_scaledTexture);
            if (FAILED(hr) || m_scaledTexture == nullptr) {
                setError(error, "GPU scale target texture create failed: " + hresultString(hr));
                return false;
            }
            hr = m_device->CreateRenderTargetView(m_scaledTexture, nullptr, &m_scaledRtv);
            if (FAILED(hr) || m_scaledRtv == nullptr) {
                setError(error, "GPU scale target RTV create failed: " + hresultString(hr));
                return false;
            }
        }
        return true;
    }

    bool gpuScaleBlit(ID3D11Texture2D* sourceTexture,
                      const D3D11_TEXTURE2D_DESC& sourceDesc,
                      std::string* error) {
        if (!m_gpuScaleReady || m_context == nullptr) {
            return false;
        }
        if (!ensureGpuScaleTextures(sourceDesc.Width, sourceDesc.Height,
                                    sourceDesc.Format, error)) {
            return false;
        }

        // Copy DDA texture (has special misc flags) to a regular SRV-capable texture.
        m_context->CopyResource(m_gpuSourceTexture, sourceTexture);

        // Set up the fullscreen-triangle draw to scale the source to the target.
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertexShader, nullptr, 0);
        m_context->PSSetShader(m_pixelShader, nullptr, 0);
        m_context->PSSetShaderResources(0, 1, &m_gpuSourceSrv);
        m_context->PSSetSamplers(0, 1, &m_sampler);
        m_context->RSSetState(m_rasterizerState);
        m_context->OMSetBlendState(m_blendState, nullptr, 0xFFFFFFFF);
        m_context->OMSetDepthStencilState(m_depthStencilState, 0);
        m_context->OMSetRenderTargets(1, &m_scaledRtv, nullptr);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(m_targetWidth);
        viewport.Height = static_cast<float>(m_targetHeight);
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);

        m_context->Draw(3, 0);

        // Unbind SRV to avoid hazard on next frame.
        ID3D11ShaderResourceView* nullSrv = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSrv);
        return true;
    }

    void initNv12Pipeline() {
        m_nv12Ready = false;
        if (m_device == nullptr) {
            return;
        }
        ID3DBlob* yBlob = nullptr;
        ID3DBlob* uvBlob = nullptr;
        std::string err;
        if (!compileHlslShader(kBgraToYShaderHlsl, "ps_4_0", "main", &yBlob, &err) ||
            !compileHlslShader(kBgraToUvSimpleShaderHlsl, "ps_4_0", "main", &uvBlob, &err)) {
            releaseComObject(yBlob);
            releaseComObject(uvBlob);
            return;
        }
        HRESULT hr = m_device->CreatePixelShader(
            yBlob->GetBufferPointer(), yBlob->GetBufferSize(), nullptr, &m_yPixelShader);
        releaseComObject(yBlob);
        if (FAILED(hr) || m_yPixelShader == nullptr) {
            releaseComObject(uvBlob);
            return;
        }
        hr = m_device->CreatePixelShader(
            uvBlob->GetBufferPointer(), uvBlob->GetBufferSize(), nullptr, &m_uvPixelShader);
        releaseComObject(uvBlob);
        if (FAILED(hr) || m_uvPixelShader == nullptr) {
            return;
        }
        m_nv12Ready = true;
    }

    bool ensureNv12Textures(std::string* error) {
        const UINT w = static_cast<UINT>(m_targetWidth);
        const UINT h = static_cast<UINT>(m_targetHeight);
        if (m_yTexture == nullptr) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
            HRESULT hr = m_device->CreateTexture2D(&d, nullptr, &m_yTexture);
            if (FAILED(hr)) { setError(error, "Y tex failed"); return false; }
            hr = m_device->CreateRenderTargetView(m_yTexture, nullptr, &m_yRtv);
            if (FAILED(hr)) { setError(error, "Y RTV failed"); return false; }
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = m_device->CreateTexture2D(&d, nullptr, &m_yStaging);
            if (FAILED(hr)) { setError(error, "Y staging failed"); return false; }
        }
        if (m_uvTexture == nullptr) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w / 2; d.Height = h / 2; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
            HRESULT hr = m_device->CreateTexture2D(&d, nullptr, &m_uvTexture);
            if (FAILED(hr)) { setError(error, "UV tex failed"); return false; }
            hr = m_device->CreateRenderTargetView(m_uvTexture, nullptr, &m_uvRtv);
            if (FAILED(hr)) { setError(error, "UV RTV failed"); return false; }
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = m_device->CreateTexture2D(&d, nullptr, &m_uvStaging);
            if (FAILED(hr)) { setError(error, "UV staging failed"); return false; }
        }
        return true;
    }

    void gpuNv12Convert(av::capture::ScreenFrame& outFrame) {
        std::string nv12Err;
        if (!ensureNv12Textures(&nv12Err)) { return; }
        // Create SRV for the scaled BGRA texture.
        ID3D11ShaderResourceView* scaledSrv = nullptr;
        HRESULT hr = m_device->CreateShaderResourceView(m_scaledTexture, nullptr, &scaledSrv);
        if (FAILED(hr) || scaledSrv == nullptr) { return; }

        // Pass 1: Y plane (full resolution).
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertexShader, nullptr, 0);
        m_context->PSSetShader(m_yPixelShader, nullptr, 0);
        m_context->PSSetShaderResources(0, 1, &scaledSrv);
        m_context->PSSetSamplers(0, 1, &m_sampler);
        m_context->RSSetState(m_rasterizerState);
        m_context->OMSetBlendState(m_blendState, nullptr, 0xFFFFFFFF);
        m_context->OMSetDepthStencilState(m_depthStencilState, 0);
        m_context->OMSetRenderTargets(1, &m_yRtv, nullptr);
        D3D11_VIEWPORT vpY{};
        vpY.Width = static_cast<float>(m_targetWidth);
        vpY.Height = static_cast<float>(m_targetHeight);
        vpY.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vpY);
        m_context->Draw(3, 0);

        // Pass 2: UV plane (half resolution).
        m_context->PSSetShader(m_uvPixelShader, nullptr, 0);
        m_context->OMSetRenderTargets(1, &m_uvRtv, nullptr);
        D3D11_VIEWPORT vpUV{};
        vpUV.Width = static_cast<float>(m_targetWidth / 2);
        vpUV.Height = static_cast<float>(m_targetHeight / 2);
        vpUV.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vpUV);
        m_context->Draw(3, 0);

        // Unbind.
        ID3D11ShaderResourceView* nullSrv = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSrv);
        releaseComObject(scaledSrv);

        // Download NV12 planes.
        m_context->CopyResource(m_yStaging, m_yTexture);
        m_context->CopyResource(m_uvStaging, m_uvTexture);
        const int w = m_targetWidth;
        const int h = m_targetHeight;
        const std::size_t ySize = static_cast<std::size_t>(w) * h;
        const std::size_t uvSize = static_cast<std::size_t>(w) * (h / 2);
        outFrame.nv12.resize(ySize + uvSize);

        D3D11_MAPPED_SUBRESOURCE yMap{};
        hr = m_context->Map(m_yStaging, 0, D3D11_MAP_READ, 0, &yMap);
        if (SUCCEEDED(hr)) {
            const auto* s = static_cast<const uint8_t*>(yMap.pData);
            for (int y = 0; y < h; ++y) {
                std::memcpy(outFrame.nv12.data() + static_cast<std::size_t>(y) * w,
                            s + static_cast<std::size_t>(y) * yMap.RowPitch,
                            static_cast<std::size_t>(w));
            }
            m_context->Unmap(m_yStaging, 0);
        }
        D3D11_MAPPED_SUBRESOURCE uvMap{};
        hr = m_context->Map(m_uvStaging, 0, D3D11_MAP_READ, 0, &uvMap);
        if (SUCCEEDED(hr)) {
            const auto* s = static_cast<const uint8_t*>(uvMap.pData);
            const int uvRowBytes = w;  // R8G8 has w/2 texels * 2 bytes = w bytes.
            const int uvH = h / 2;
            for (int y = 0; y < uvH; ++y) {
                std::memcpy(outFrame.nv12.data() + ySize + static_cast<std::size_t>(y) * uvRowBytes,
                            s + static_cast<std::size_t>(y) * uvMap.RowPitch,
                            static_cast<std::size_t>(uvRowBytes));
            }
            m_context->Unmap(m_uvStaging, 0);
        }
    }

    void ensureSampleMaps(UINT sourceWidth, UINT sourceHeight) const {
        if (m_mapSourceWidth == sourceWidth &&
            m_mapSourceHeight == sourceHeight &&
            m_mapTargetWidth == m_targetWidth &&
            m_mapTargetHeight == m_targetHeight &&
            m_sourceXMap.size() == static_cast<std::size_t>(m_targetWidth) &&
            m_sourceYMap.size() == static_cast<std::size_t>(m_targetHeight)) {
            return;
        }
        m_sourceXMap.resize(static_cast<std::size_t>(m_targetWidth));
        m_sourceYMap.resize(static_cast<std::size_t>(m_targetHeight));
        for (int x = 0; x < m_targetWidth; ++x) {
            m_sourceXMap[static_cast<std::size_t>(x)] = static_cast<UINT>(
                (static_cast<uint64_t>(x) * static_cast<uint64_t>(sourceWidth)) /
                static_cast<uint64_t>(m_targetWidth));
        }
        for (int y = 0; y < m_targetHeight; ++y) {
            m_sourceYMap[static_cast<std::size_t>(y)] = static_cast<UINT>(
                (static_cast<uint64_t>(y) * static_cast<uint64_t>(sourceHeight)) /
                static_cast<uint64_t>(m_targetHeight));
        }
        m_mapSourceWidth = sourceWidth;
        m_mapSourceHeight = sourceHeight;
        m_mapTargetWidth = m_targetWidth;
        m_mapTargetHeight = m_targetHeight;
    }

    void shutdown() {
        releaseComObject(m_uvStaging);
        releaseComObject(m_uvRtv);
        releaseComObject(m_uvTexture);
        releaseComObject(m_yStaging);
        releaseComObject(m_yRtv);
        releaseComObject(m_yTexture);
        releaseComObject(m_uvPixelShader);
        releaseComObject(m_yPixelShader);
        releaseComObject(m_scaledRtv);
        releaseComObject(m_scaledTexture);
        releaseComObject(m_gpuSourceSrv);
        releaseComObject(m_gpuSourceTexture);
        releaseComObject(m_depthStencilState);
        releaseComObject(m_blendState);
        releaseComObject(m_rasterizerState);
        releaseComObject(m_sampler);
        releaseComObject(m_pixelShader);
        releaseComObject(m_vertexShader);
        releaseComObject(m_stagingTexture);
        releaseComObject(m_duplication);
        releaseComObject(m_context);
        releaseComObject(m_device);
        m_sourceWidth = 0;
        m_sourceHeight = 0;
        m_sourceXMap.clear();
        m_sourceYMap.clear();
        m_mapSourceWidth = 0;
        m_mapSourceHeight = 0;
        m_mapTargetWidth = 0;
        m_mapTargetHeight = 0;
        m_gpuScaleReady = false;
        m_nv12Ready = false;
    }

    ID3D11Device* m_device{nullptr};
    ID3D11DeviceContext* m_context{nullptr};
    IDXGIOutputDuplication* m_duplication{nullptr};
    ID3D11Texture2D* m_stagingTexture{nullptr};
    int m_sourceWidth{0};
    int m_sourceHeight{0};
    int m_targetWidth{0};
    int m_targetHeight{0};
    mutable std::vector<UINT> m_sourceXMap;
    mutable std::vector<UINT> m_sourceYMap;
    mutable UINT m_mapSourceWidth{0};
    mutable UINT m_mapSourceHeight{0};
    mutable int m_mapTargetWidth{0};
    mutable int m_mapTargetHeight{0};

    // GPU scale pipeline resources.
    bool m_gpuScaleReady{false};
    ID3D11VertexShader* m_vertexShader{nullptr};
    ID3D11PixelShader* m_pixelShader{nullptr};
    ID3D11SamplerState* m_sampler{nullptr};
    ID3D11RasterizerState* m_rasterizerState{nullptr};
    ID3D11BlendState* m_blendState{nullptr};
    ID3D11DepthStencilState* m_depthStencilState{nullptr};
    ID3D11Texture2D* m_gpuSourceTexture{nullptr};
    ID3D11ShaderResourceView* m_gpuSourceSrv{nullptr};
    ID3D11Texture2D* m_scaledTexture{nullptr};
    ID3D11RenderTargetView* m_scaledRtv{nullptr};

    // GPU NV12 conversion resources.
    bool m_nv12Ready{false};
    ID3D11PixelShader* m_yPixelShader{nullptr};
    ID3D11PixelShader* m_uvPixelShader{nullptr};
    ID3D11Texture2D* m_yTexture{nullptr};
    ID3D11RenderTargetView* m_yRtv{nullptr};
    ID3D11Texture2D* m_yStaging{nullptr};
    ID3D11Texture2D* m_uvTexture{nullptr};
    ID3D11RenderTargetView* m_uvRtv{nullptr};
    ID3D11Texture2D* m_uvStaging{nullptr};
};
#endif

uint64_t ntpNow() {
    constexpr uint64_t kNtpUnixEpochOffsetSeconds = 2208988800ULL;
    const auto now = std::chrono::system_clock::now();
    const auto sinceEpoch = now.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - seconds);
    const uint64_t ntpSeconds =
        static_cast<uint64_t>(seconds.count()) + kNtpUnixEpochOffsetSeconds;
    const uint64_t ntpFraction =
        (static_cast<uint64_t>(nanoseconds.count()) << 32U) / 1000000000ULL;
    return (ntpSeconds << 32U) | ntpFraction;
}

uint32_t compactNtp(uint64_t ntpTimestamp) {
    return static_cast<uint32_t>((ntpTimestamp >> 16U) & 0xFFFFFFFFU);
}

bool computeRttFromReceiverReport(uint32_t lastSenderReport,
                                  uint32_t delaySinceLastSenderReport,
                                  uint32_t* outRttMs) {
    if (lastSenderReport == 0U || outRttMs == nullptr) {
        return false;
    }
    const uint32_t arrival = compactNtp(ntpNow());
    const uint32_t rttUnits = arrival - lastSenderReport - delaySinceLastSenderReport;
    const uint64_t rttMs = (static_cast<uint64_t>(rttUnits) * 1000ULL) / 65536ULL;
    *outRttMs = rttMs > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(rttMs);
    return true;
}

class VideoRecvPeerRuntime final {
public:
    using PacketOutcomeCallback = std::function<void(const media::RTPPacket&,
                                                     const VideoRecvConsumeOutcome&,
                                                     av::codec::DecodedVideoFrame&&)>;
    using PollOutcomeCallback = std::function<void(const VideoRecvConsumeOutcome&,
                                                   av::codec::DecodedVideoFrame&&)>;

    VideoRecvPeerRuntime(uint32_t remoteMediaSsrc,
                         VideoRecvPipelineConfig recvPipelineConfig,
                         std::atomic<uint32_t>& adaptiveJitterTargetMs,
                         PacketOutcomeCallback packetOutcomeCallback,
                         PollOutcomeCallback pollOutcomeCallback)
        : m_remoteMediaSsrc(remoteMediaSsrc),
          m_expectedFrameRate(std::max(1, recvPipelineConfig.frameRate)),
          m_recvPipeline(std::move(recvPipelineConfig)),
          m_adaptiveJitterTargetMs(adaptiveJitterTargetMs),
          m_packetOutcomeCallback(std::move(packetOutcomeCallback)),
          m_pollOutcomeCallback(std::move(pollOutcomeCallback)),
          m_lastActiveAtMs(steadyNowMs()) {}

    ~VideoRecvPeerRuntime() {
        stop();
    }

    void start() {
        QMutexLocker locker(&m_mutex);
        if (m_thread != nullptr) {
            return;
        }
        m_stopping.store(false, std::memory_order_release);
        m_thread = QThread::create([this]() { run(); });
        if (m_thread != nullptr) {
            m_thread->start();
        }
    }

    void stop() {
        QThread* threadToJoin = nullptr;
        {
            QMutexLocker locker(&m_mutex);
            if (m_thread == nullptr) {
                return;
            }
            m_stopping.store(true, std::memory_order_release);
            threadToJoin = m_thread;
            m_thread = nullptr;
        }
        threadToJoin->wait();
        delete threadToJoin;

        QMutexLocker locker(&m_mutex);
        m_jitterBuffer.clear();
    }

    bool enqueue(media::RTPPacket packet) {
        QMutexLocker locker(&m_mutex);
        if (m_thread == nullptr || m_stopping.load(std::memory_order_acquire)) {
            return false;
        }
        m_jitterBuffer.push(packet);
        m_lastActiveAtMs.store(steadyNowMs(), std::memory_order_release);
        return true;
    }

    uint32_t remoteMediaSsrc() const {
        return m_remoteMediaSsrc;
    }

    uint64_t lastActiveAtMs() const {
        return m_lastActiveAtMs.load(std::memory_order_acquire);
    }

private:
    void run() {
        while (true) {
            media::RTPPacket packet;
            bool hasPacket = false;
            const bool stopping = m_stopping.load(std::memory_order_acquire);
            const uint32_t jitterTargetMs = m_adaptiveJitterTargetMs.load(std::memory_order_acquire);
            const std::size_t minBufferedPackets =
                stopping
                    ? 1U
                    : jitterTargetMs >= 100U
                    ? std::max<std::size_t>(kMinRecvPeerBufferedPackets,
                                             static_cast<std::size_t>(
                                                 (m_expectedFrameRate * jitterTargetMs + 999U) / 1000U))
                    : kMinRecvPeerBufferedPackets;
            m_jitterBuffer.setMinBufferedPackets(minBufferedPackets);
            m_jitterBuffer.setGapTimeout(std::chrono::milliseconds(kRecvPeerJitterGapTimeoutMs));

            if (stopping && m_jitterBuffer.size() == 0U) {
                break;
            }
            hasPacket = m_jitterBuffer.popWait(
                packet,
                std::chrono::milliseconds(kRecvPeerJitterGapTimeoutMs));

            if (hasPacket) {
                processPacket(packet);
            } else {
                pollDecodedFrames(kRecvWorkerPollBudgetWhenIdle);
            }
        }

        pollDecodedFrames(kRecvWorkerPollBudgetOnStop);
    }

    void processPacket(const media::RTPPacket& packet) {
        av::codec::DecodedVideoFrame decoded;
        VideoRecvConsumeOutcome consumeOutcome;
        if (!m_recvConsumePipeline.consumeAndDecide(
                packet,
                m_remoteMediaSsrc,
                m_recvPipeline,
                decoded,
                consumeOutcome)) {
            return;
        }
        m_lastActiveAtMs.store(steadyNowMs(), std::memory_order_release);
        if (m_packetOutcomeCallback) {
            m_packetOutcomeCallback(packet, consumeOutcome, std::move(decoded));
        }
        pollDecodedFrames(kRecvWorkerPollBudgetAfterPacket);
    }

    void pollDecodedFrames(std::size_t budget) {
        for (std::size_t index = 0; index < budget; ++index) {
            av::codec::DecodedVideoFrame decoded;
            VideoRecvConsumeOutcome consumeOutcome;
            if (!m_recvConsumePipeline.pollAndDecide(
                    m_recvPipeline,
                    decoded,
                    consumeOutcome)) {
                break;
            }
            m_lastActiveAtMs.store(steadyNowMs(), std::memory_order_release);
            if (m_pollOutcomeCallback) {
                m_pollOutcomeCallback(consumeOutcome, std::move(decoded));
            }
        }
    }

    const uint32_t m_remoteMediaSsrc{0U};
    const int m_expectedFrameRate{30};
    VideoRecvConsumePipeline m_recvConsumePipeline;
    VideoRecvPipeline m_recvPipeline;
    std::atomic<uint32_t>& m_adaptiveJitterTargetMs;
    PacketOutcomeCallback m_packetOutcomeCallback;
    PollOutcomeCallback m_pollOutcomeCallback;
    media::JitterBuffer m_jitterBuffer{
        kMaxRecvPeerQueuedPackets,
        kMinRecvPeerBufferedPackets,
        std::chrono::milliseconds(kRecvPeerJitterGapTimeoutMs)};

    mutable QMutex m_mutex;
    QThread* m_thread{nullptr};
    std::atomic<bool> m_stopping{false};
    std::atomic<uint64_t> m_lastActiveAtMs{0};
};

}  // namespace

bool ScreenShareSession::openSocketLocked() {
    std::string socketError;
    if (!m_mediaSocket.open(m_config.localAddress, m_config.localPort, &socketError)) {
        setErrorLocked(socketError.empty() ? "socket setup failed" : socketError);
        closeSocketLocked();
        return false;
    }

    // Real dual-camera 1080p can burst RTP packets fast enough to overflow default UDP buffers.
    // Keep larger buffers by default and allow env overrides for machine-specific tuning.
    const auto socketBufferBytesFromEnv = [](const char* key, int fallbackBytes) {
        const int raw = qEnvironmentVariableIntValue(key);
        const int configured = raw > 0 ? raw : fallbackBytes;
        return std::clamp(configured, 64 * 1024, 16 * 1024 * 1024);
    };
    const int recvBufferBytes = socketBufferBytesFromEnv("MEETING_VIDEO_SOCKET_RECVBUF_BYTES", 4 * 1024 * 1024);
    const int sendBufferBytes = socketBufferBytesFromEnv("MEETING_VIDEO_SOCKET_SNDBUF_BYTES", 4 * 1024 * 1024);
    std::string socketOptionError;
    if (!m_mediaSocket.configureSocketBuffers(recvBufferBytes, sendBufferBytes, &socketOptionError)) {
        setErrorLocked(socketOptionError.empty() ? "socket buffer setup failed" : socketOptionError);
    }

    (void)m_mediaSocket.setPeer(m_config.peerAddress, m_config.peerPort);
    m_mediaSocket.setReadTimeoutMs(50);
    return true;
}

void ScreenShareSession::closeSocketLocked() {
    m_mediaSocket.close();
}

bool ScreenShareSession::applyRtcpDispatchPlanLocked(
    const VideoRtcpFeedbackDispatchPlan& dispatchPlan) {
    if (!dispatchPlan.hasActions()) {
        return false;
    }

    bool handled = false;
    const auto publishAdaptiveTarget = [this]() {
        const VideoBwePolicyTarget& target = m_videoBwePolicy.target();
        const uint32_t previousTarget =
            m_targetBitrateBps.exchange(target.bitrateBps, std::memory_order_acq_rel);
        if (previousTarget != target.bitrateBps) {
            m_targetBitrateUpdatedAtMs.store(steadyNowMs(), std::memory_order_release);
        }

        const bool wasSuspended = m_adaptiveVideoSuspended.exchange(
            target.videoSuspended, std::memory_order_acq_rel);
        m_adaptiveVideoWidth.store(target.width, std::memory_order_release);
        m_adaptiveVideoHeight.store(target.height, std::memory_order_release);
        m_adaptiveVideoFrameRate.store(target.frameRate, std::memory_order_release);
        m_adaptiveJitterTargetMs.store(target.jitterTargetMs, std::memory_order_release);
        m_adaptiveProfileVersion.store(target.version, std::memory_order_release);
        if (wasSuspended && !target.videoSuspended) {
            m_forceKeyFramePending.store(true, std::memory_order_release);
        }

        if (target.requestTurnRelay) {
            m_adaptiveTurnRelayRequested.store(true, std::memory_order_release);
            if (m_adaptiveTurnRelayRequestCallback) {
                m_adaptiveTurnRelayRequestCallback();
            }
        }
    };
    for (const uint16_t sequenceNumber : dispatchPlan.retransmitSequenceNumbers) {
        std::string retransmitError;
        if (!m_rtcpActionPipeline.retransmitPacket(sequenceNumber, m_mediaSocket, &retransmitError)) {
            if (!retransmitError.empty()) {
                setErrorLocked(retransmitError);
            }
            continue;
        }
        m_retransmitPacketCount.fetch_add(1, std::memory_order_acq_rel);
        handled = true;
    }

    if (dispatchPlan.requestKeyFrame) {
        m_forceKeyFramePending.store(true, std::memory_order_release);
        m_keyframeRequestCount.fetch_add(1, std::memory_order_acq_rel);
        handled = true;
    }

    if (dispatchPlan.hasTargetBitrate) {
        (void)m_videoBwePolicy.onRembTarget(dispatchPlan.targetBitrateBps, steadyNowMs());
        publishAdaptiveTarget();
        handled = true;
    }

    for (const auto& report : dispatchPlan.receiverReports) {
        VideoBwePolicySample sample{};
        sample.fractionLost = report.fractionLost;
        sample.nowMs = steadyNowMs();
        sample.hasRtt = computeRttFromReceiverReport(report.lastSenderReport,
                                                     report.delaySinceLastSenderReport,
                                                     &sample.rttMs);
        (void)m_videoBwePolicy.onReceiverReport(sample);
        publishAdaptiveTarget();
        handled = true;
    }

    return handled;
}

void ScreenShareSession::setErrorLocked(std::string message) {
    m_lastError = std::move(message);
    qWarning().noquote() << "[screen-session]" << QString::fromStdString(m_lastError);
    if (m_statusCallback) {
        m_statusCallback(QStringLiteral("Video session error: %1")
                             .arg(QString::fromStdString(m_lastError))
                             .toStdString());
    }
}

void ScreenShareSession::captureLoop() {
    const VideoSendSourcePipeline sendSourcePipeline;
    const VideoSendLoopPipeline sendLoopPipeline;
    CameraFrameTimeoutState cameraFrameTimeoutState;
    const auto statusCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_statusCallback;
    }();
    const auto errorCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_errorCallback;
    }();
    const av::VideoPipelineProfile requestedProfile = av::videoPipelineProfileFromEnvironment();

    while (m_running.load(std::memory_order_acquire) &&
           (m_sharingEnabled.load(std::memory_order_acquire) ||
            m_cameraSendingEnabled.load(std::memory_order_acquire))) {
        {
            QMutexLocker locker(&m_mutex);
            while (m_running.load(std::memory_order_acquire) &&
                   (m_sharingEnabled.load(std::memory_order_acquire) ||
                    m_cameraSendingEnabled.load(std::memory_order_acquire)) &&
                   !m_mediaSocket.hasPeer()) {
                m_stateWaitCondition.wait(&m_mutex, 100);
            }
            if (!m_running.load(std::memory_order_acquire) ||
                (!m_sharingEnabled.load(std::memory_order_acquire) &&
                 !m_cameraSendingEnabled.load(std::memory_order_acquire))) {
                break;
            }
        }

        const VideoSendSource source = VideoSessionStateMachine::resolveSendSource(
            m_sharingEnabled.load(std::memory_order_acquire),
            m_cameraSendingEnabled.load(std::memory_order_acquire));
        if (av::isHardwareE2E(requestedProfile)) {
            bool bypassCpuCapture = source == VideoSendSource::Screen &&
                                    !m_disableHardwareScreenCapture.load(std::memory_order_acquire);
            if (source == VideoSendSource::Camera) {
                QMutexLocker locker(&m_mutex);
                bypassCpuCapture = !(m_cameraCapture && m_cameraCapture->isRunning()) &&
                                   !(m_cameraFallbackCapture && m_cameraFallbackCapture->isRunning());
            }
            if (bypassCpuCapture) {
                QThread::msleep(10);
                continue;
            }
        }

        VideoSendLoopState loopState{};
        loopState.source = source;
        {
            QMutexLocker locker(&m_mutex);
            loopState.sharingEnabled = m_sharingEnabled.load(std::memory_order_acquire);
            loopState.cameraSendingEnabled = m_cameraSendingEnabled.load(std::memory_order_acquire);
            loopState.screenCapture = m_capture;
            loopState.cameraRelay = m_cameraRelay;
            loopState.cameraFallbackCapture = m_cameraFallbackCapture;
            loopState.cameraCaptureRunning = m_cameraCapture && m_cameraCapture->isRunning();
            loopState.preferredCameraName = m_preferredCameraDeviceName;
            loopState.peerReady = m_mediaSocket.hasPeer();
        }
        const VideoSendLoopSnapshot loopSnapshot = sendLoopPipeline.makeSnapshot(std::move(loopState));

        VideoSendPipelineInputFrame frame;
        std::string sourceStatusMessage;
        const VideoSendFrameFetchResult frameFetchResult = sendSourcePipeline.pullFrame(
            loopSnapshot.sourceSnapshot,
            std::chrono::milliseconds(100),
            cameraFrameTimeoutState,
            frame,
            &sourceStatusMessage);
        if (!sourceStatusMessage.empty() && statusCallback) {
            statusCallback(sourceStatusMessage);
        }
        if (frameFetchResult == VideoSendFrameFetchResult::Retry) {
            continue;
        }
        if (frameFetchResult == VideoSendFrameFetchResult::Abort) {
            if (source == VideoSendSource::Camera) {
                const std::string errorMessage = sourceStatusMessage.empty()
                    ? std::string{"camera capture produced no frames"}
                    : sourceStatusMessage;
                {
                    QMutexLocker locker(&m_mutex);
                    setErrorLocked(errorMessage);
                    m_cameraSendingEnabled.store(false, std::memory_order_release);
                    stopCameraCaptureLocked();
                }
                if (errorCallback) {
                    errorCallback(errorMessage);
                }
            }
            break;
        }

        VideoSendCapturedFrame capturedFrame;
        capturedFrame.source = source;
        capturedFrame.inputFrame = std::move(frame);
        if (!m_sendFrameRingBuffer.push(std::move(capturedFrame))) {
            break;
        }
    }

    m_sendFrameRingBuffer.close();
}

void ScreenShareSession::sendLoop() {
    av::codec::VideoEncoder encoder;
    const av::VideoPipelineProfile requestedProfile = av::videoPipelineProfileFromEnvironment();
    const bool profileForced = av::videoPipelineProfileExplicitlySet();
    av::VideoPipelineProfile sendProfile = requestedProfile;
    VideoSendPipeline sendPipeline(
        VideoSendPipelineConfig{m_config.frameRate, m_config.maxPayloadBytes, sendProfile});
    auto configureEncoderForProfile = [&](av::VideoPipelineProfile profile,
                                          int width,
                                          int height,
                                          int frameRate,
                                          int bitrate,
                                          uint8_t payloadType) {
        if (av::isHardwareE2E(profile)) {
#ifdef _WIN32
            return encoder.configureHardwareD3D11(width,
                                                  height,
                                                  frameRate,
                                                  bitrate,
                                                  payloadType,
                                                  m_config.encoderPreset);
#else
            qWarning().noquote() << "[screen-session] hardware video pipeline unsupported on this platform"
                                 << "profile=" << av::videoPipelineProfileName(profile);
            return false;
#endif
        }
        return encoder.configure(width,
                                 height,
                                 frameRate,
                                 bitrate,
                                 payloadType,
                                 m_config.encoderPreset);
    };
    bool encoderConfigured = configureEncoderForProfile(sendProfile,
                                                        m_config.width,
                                                        m_config.height,
                                                        m_config.frameRate,
                                                        m_config.bitrate,
                                                        m_config.cameraPayloadType);
    if (!encoderConfigured &&
        av::isHardwareE2E(sendProfile) &&
        !profileForced) {
        sendProfile = av::VideoPipelineProfile::SoftwareE2E;
        {
            QMutexLocker locker(&m_mutex);
            m_disableHardwareScreenCapture.store(true, std::memory_order_release);
            (void)startCaptureLocked(false);
            if (m_cameraSendingEnabled.load(std::memory_order_acquire)) {
                (void)startCameraCaptureLocked(false);
            }
        }
        sendPipeline = VideoSendPipeline(
            VideoSendPipelineConfig{m_config.frameRate, m_config.maxPayloadBytes, sendProfile});
        qInfo().noquote() << "[screen-session] video send profile fallback profile=software reason=hardware encoder unavailable";
        encoderConfigured = configureEncoderForProfile(sendProfile,
                                                       m_config.width,
                                                       m_config.height,
                                                       m_config.frameRate,
                                                       m_config.bitrate,
                                                       m_config.cameraPayloadType);
    }
    if (!encoderConfigured) {
        std::function<void(std::string)> errorCallback;
        {
            QMutexLocker locker(&m_mutex);
            setErrorLocked(av::isHardwareE2E(sendProfile)
                               ? "hardware video pipeline unsupported on this platform"
                               : "video encoder configure failed");
            errorCallback = m_errorCallback;
        }
        if (errorCallback) {
            errorCallback(av::isHardwareE2E(sendProfile)
                              ? "hardware video pipeline unsupported on this platform"
                              : "video encoder configure failed");
        }
        return;
    }
    qInfo().noquote() << "[screen-session] video send profile="
                      << av::videoPipelineProfileName(sendProfile)
                      << "fps=" << m_config.frameRate
                      << "hardwareInput=" << (encoder.usesHardwareInput() ? 1 : 0);
    m_appliedBitrateBps.store(static_cast<uint32_t>(encoder.bitrate()), std::memory_order_release);

    VideoSendTelemetryPipeline sendTelemetryPipeline;
    VideoSendTelemetryState sendTelemetryState;
    media::RTCPHandler rtcpHandler;
    uint64_t lastSenderReportAtMs = 0U;
    uint32_t lastRtpTimestamp = 0U;
    uint32_t rtpPacketCount = 0U;
    uint32_t rtpOctetCount = 0U;
    const auto hardwareCaptureStartedAt = std::chrono::steady_clock::now();
    int64_t lastHardwareCapturePts = -1;
#ifdef _WIN32
    std::unique_ptr<WindowsD3D11ScreenCapture> hardwareScreenCapture;
    std::unique_ptr<WindowsD3D11ScreenReadbackCapture> gpuReadbackScreenCapture;
    std::unique_ptr<av::capture::WindowsD3D11CameraCapture> hardwareCameraCapture;
    uint64_t activeHardwareCameraDeviceGeneration = std::numeric_limits<uint64_t>::max();
    uint64_t observedHardwareCameraDeviceGeneration = std::numeric_limits<uint64_t>::max();
#endif
    bool firstHardwareCameraFrameObserved = false;
    bool firstHardwareCameraPreviewFrameStored = false;
    bool firstHardwareScreenPreviewFrameStored = false;
    bool firstGpuReadbackScreenFrameObserved = false;
    bool firstSoftwareScreenFrameObserved = false;
    bool gpuReadbackScreenCaptureFailed = false;
    const auto statusCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_statusCallback;
    }();
    const auto cameraSourceCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_cameraSourceCallback;
    }();
    const auto localCameraPreviewCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_localCameraPreviewCallback;
    }();
    while (m_running.load(std::memory_order_acquire) &&
           (m_sharingEnabled.load(std::memory_order_acquire) ||
            m_cameraSendingEnabled.load(std::memory_order_acquire))) {
        {
            QMutexLocker locker(&m_mutex);
            while (m_running.load(std::memory_order_acquire) &&
                   (m_sharingEnabled.load(std::memory_order_acquire) ||
                    m_cameraSendingEnabled.load(std::memory_order_acquire)) &&
                   !m_mediaSocket.hasPeer() &&
                   (!m_sharingEnabled.load(std::memory_order_acquire) ||
                    !localCameraPreviewCallback)) {
                m_stateWaitCondition.wait(&m_mutex, 100);
            }
            if (!m_running.load(std::memory_order_acquire) ||
                (!m_sharingEnabled.load(std::memory_order_acquire) &&
                 !m_cameraSendingEnabled.load(std::memory_order_acquire))) {
                break;
            }
        }

        const VideoSendSource currentSource = VideoSessionStateMachine::resolveSendSource(
            m_sharingEnabled.load(std::memory_order_acquire),
            m_cameraSendingEnabled.load(std::memory_order_acquire));
        if (currentSource == VideoSendSource::None) {
            continue;
        }
        const bool useHardwareScreenCapture =
            av::isHardwareE2E(sendProfile) &&
            currentSource == VideoSendSource::Screen &&
            !m_disableHardwareScreenCapture.load(std::memory_order_acquire);
        const bool useGpuReadbackScreenCapture =
            av::isHardwareE2E(requestedProfile) &&
            !profileForced &&
            currentSource == VideoSendSource::Screen &&
            m_disableHardwareScreenCapture.load(std::memory_order_acquire) &&
            !gpuReadbackScreenCaptureFailed;
        const bool useHardwareCameraCapture =
            av::isHardwareE2E(sendProfile) && currentSource == VideoSendSource::Camera;
        VideoSendCapturedFrame capturedFrame;
        VideoSendSource source = currentSource;
        if (!useHardwareScreenCapture &&
            !useGpuReadbackScreenCapture &&
            !useHardwareCameraCapture) {
            if (!m_sendFrameRingBuffer.popWait(capturedFrame, std::chrono::milliseconds(100))) {
                if (m_sendFrameRingBuffer.closed()) {
                    break;
                }
                continue;
            }
            source = capturedFrame.source;
            if (source == VideoSendSource::None || currentSource != source) {
                continue;
            }
        } else {
            capturedFrame.source = currentSource;
        }
        media::UdpEndpoint peer{};
        {
            QMutexLocker locker(&m_mutex);
            if (m_mediaSocket.hasPeer()) {
                peer = m_mediaSocket.peer();
            }
        }

        const uint8_t payloadType = source == VideoSendSource::Screen ? m_config.payloadType
                                                                      : m_config.cameraPayloadType;
        int targetWidth = m_adaptiveVideoWidth.load(std::memory_order_acquire) > 0
            ? m_adaptiveVideoWidth.load(std::memory_order_acquire)
            : m_config.width;
        int targetHeight = m_adaptiveVideoHeight.load(std::memory_order_acquire) > 0
            ? m_adaptiveVideoHeight.load(std::memory_order_acquire)
            : m_config.height;
        int targetFrameRate = m_adaptiveVideoFrameRate.load(std::memory_order_acquire) > 0
            ? m_adaptiveVideoFrameRate.load(std::memory_order_acquire)
            : m_config.frameRate;
        if (source == VideoSendSource::Screen) {
            const auto [cappedWidth, cappedHeight] = clampScreenShareDimensions(targetWidth, targetHeight);
            targetWidth = cappedWidth;
            targetHeight = cappedHeight;
            targetFrameRate = std::min(std::max(1, targetFrameRate), kMaxScreenShareFrameRate);
        }
        const uint32_t targetBitrate = m_targetBitrateBps.load(std::memory_order_acquire);
        const bool videoSuspended = m_adaptiveVideoSuspended.load(std::memory_order_acquire);

#ifdef _WIN32
        if (currentSource == VideoSendSource::Camera) {
            const uint64_t cameraDeviceGeneration =
                m_cameraDeviceGeneration.load(std::memory_order_acquire);
            if (observedHardwareCameraDeviceGeneration != cameraDeviceGeneration) {
                observedHardwareCameraDeviceGeneration = cameraDeviceGeneration;
                activeHardwareCameraDeviceGeneration = std::numeric_limits<uint64_t>::max();
                hardwareCameraCapture.reset();
                firstHardwareCameraFrameObserved = false;
                firstHardwareCameraPreviewFrameStored = false;
                if (statusCallback) {
                    statusCallback("Video hardware camera capture reset for selected device");
                }
                qInfo().noquote() << "[screen-session] video hardware camera capture reset for selected device"
                                  << "profile=" << av::videoPipelineProfileName(sendProfile);
                if (av::isHardwareE2E(requestedProfile) && !av::isHardwareE2E(sendProfile)) {
                    {
                        QMutexLocker locker(&m_mutex);
                        stopCameraCaptureLocked();
                    }
                    sendProfile = requestedProfile;
                    if (!configureEncoderForProfile(sendProfile,
                                                    targetWidth,
                                                    targetHeight,
                                                    targetFrameRate,
                                                    static_cast<int>(targetBitrate > 0U
                                                                         ? targetBitrate
                                                                         : static_cast<uint32_t>(m_config.bitrate)),
                                                    payloadType)) {
                        sendTelemetryPipeline.onEncodeError(sendTelemetryState,
                                                            "hardware video encoder configure failed",
                                                            statusCallback);
                        QMutexLocker locker(&m_mutex);
                        setErrorLocked("hardware video encoder configure failed");
                        continue;
                    }
                    sendPipeline = VideoSendPipeline(
                        VideoSendPipelineConfig{targetFrameRate, m_config.maxPayloadBytes, sendProfile});
                    qInfo().noquote() << "[screen-session] video send profile restored profile=hardware reason=camera-device-switch";
                }
            }
        }
#endif

        const auto maybeSendSenderReport = [&]() {
            if (!peer.isValid()) {
                return;
            }
            const uint64_t nowMs = steadyNowMs();
            if (lastSenderReportAtMs != 0U &&
                nowMs - lastSenderReportAtMs < kVideoSenderReportIntervalMs) {
                return;
            }
            media::RTCPSenderReport report{};
            report.senderSsrc = m_sender.ssrc();
            if (report.senderSsrc == 0U) {
                return;
            }
            report.ntpTimestamp = ntpNow();
            report.rtpTimestamp = lastRtpTimestamp;
            report.packetCount = rtpPacketCount;
            report.octetCount = rtpOctetCount;
            std::vector<uint8_t> senderReport = rtcpHandler.buildSenderReport(report);
            if (senderReport.empty()) {
                return;
            }
            {
                QMutexLocker locker(&m_mutex);
                if (!protectRtcpLocked(&senderReport)) {
                    return;
                }
            }
            const int sent = m_mediaSocket.sendTo(senderReport.data(), senderReport.size(), peer);
            if (sent == static_cast<int>(senderReport.size())) {
                lastSenderReportAtMs = nowMs;
            }
        };

        const bool keepLocalScreenPreviewActive =
            source == VideoSendSource::Screen && static_cast<bool>(localCameraPreviewCallback);
        if (videoSuspended && !keepLocalScreenPreviewActive) {
            maybeSendSenderReport();
            continue;
        }

        std::string bitrateError;
        if (!maybeApplyAdaptiveEncoderProfile(encoder,
                                              targetWidth,
                                              targetHeight,
                                              targetFrameRate,
                                              targetBitrate,
                                              payloadType,
                                              m_config.encoderPreset,
                                              sendProfile,
                                              m_appliedBitrateBps,
                                              m_bitrateReconfigureCount,
                                              m_targetBitrateUpdatedAtMs,
                                              m_lastBitrateApplyDelayMs,
                                              steadyNowMs(),
                                              &bitrateError)) {
            QMutexLocker locker(&m_mutex);
            setErrorLocked(bitrateError.empty() ? "video encoder adaptive profile update failed" : bitrateError);
            continue;
        }
        sendPipeline = VideoSendPipeline(
            VideoSendPipelineConfig{targetFrameRate, m_config.maxPayloadBytes, sendProfile});
        bool localScreenPreviewDispatched = false;

        if (useHardwareScreenCapture) {
#ifdef _WIN32
            if (!hardwareScreenCapture) {
                hardwareScreenCapture = std::make_unique<WindowsD3D11ScreenCapture>();
            }
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - hardwareCaptureStartedAt).count();
            int64_t framePts = static_cast<int64_t>((elapsedMs * std::max(1, targetFrameRate)) / 1000);
            if (framePts <= lastHardwareCapturePts) {
                framePts = lastHardwareCapturePts + 1;
            }
            std::string captureError;
            av::AVFramePtr hardwareFrame;
            if (!hardwareScreenCapture->capture(encoder, framePts, hardwareFrame, &captureError)) {
                if (captureError.empty()) {
                    sendTelemetryPipeline.onEncodePending(sendTelemetryState, statusCallback);
                    continue;
                }
                if (!profileForced) {
                    {
                        QMutexLocker locker(&m_mutex);
                        m_disableHardwareScreenCapture.store(true, std::memory_order_release);
                    }
                    sendProfile = av::VideoPipelineProfile::SoftwareE2E;
                    if (!configureEncoderForProfile(sendProfile,
                                                    targetWidth,
                                                    targetHeight,
                                                    targetFrameRate,
                                                    static_cast<int>(targetBitrate > 0U
                                                                         ? targetBitrate
                                                                         : static_cast<uint32_t>(m_config.bitrate)),
                                                    payloadType)) {
                        sendTelemetryPipeline.onEncodeError(sendTelemetryState,
                                                            "video software fallback encoder configure failed",
                                                            statusCallback);
                        QMutexLocker locker(&m_mutex);
                        setErrorLocked("video software fallback encoder configure failed");
                        continue;
                    }
                    qInfo().noquote() << "[screen-session] video send profile fallback profile=software reason="
                                      << QString::fromStdString(captureError);
                    sendPipeline = VideoSendPipeline(
                        VideoSendPipelineConfig{targetFrameRate, m_config.maxPayloadBytes, sendProfile});
                    continue;
                }
                sendTelemetryPipeline.onEncodeError(sendTelemetryState, captureError, statusCallback);
                QMutexLocker locker(&m_mutex);
                setErrorLocked(captureError);
                continue;
            }
            lastHardwareCapturePts = framePts;
            if (localCameraPreviewCallback) {
                av::codec::DecodedVideoFrame previewFrame;
                if (av::codec::makeD3D11HardwarePreviewFrame(*hardwareFrame, previewFrame)) {
                    previewFrame.telemetry.backendName = "dxgi-duplication-screen-preview";
                    localCameraPreviewCallback(std::move(previewFrame), VideoSendSource::Screen);
                    localScreenPreviewDispatched = true;
                    if (!firstHardwareScreenPreviewFrameStored) {
                        firstHardwareScreenPreviewFrameStored = true;
                        if (statusCallback) {
                            statusCallback("Video hardware screen localPreview=hardware_interop screenBackend=dxgi-duplication screenInterop=d3d11");
                        }
                        qInfo().noquote() << "[screen-session] screen local preview screenBackend=dxgi-duplication screenInterop=d3d11"
                                          << "profile=" << av::videoPipelineProfileName(sendProfile)
                                          << "hardwareInput=1";
                    }
                } else if (!firstHardwareScreenPreviewFrameStored && statusCallback) {
                    statusCallback("Video hardware screen localPreview=unavailable screenBackend=dxgi-duplication screenInterop=d3d11");
                }
            }
            capturedFrame.inputFrame.avFrame = std::move(hardwareFrame);
#else
            sendTelemetryPipeline.onEncodeError(sendTelemetryState,
                                                "hardware video pipeline unsupported on this platform",
                                                statusCallback);
            QMutexLocker locker(&m_mutex);
            setErrorLocked("hardware video pipeline unsupported on this platform");
            continue;
#endif
        } else if (useGpuReadbackScreenCapture) {
#ifdef _WIN32
            if (av::isHardwareE2E(sendProfile)) {
                sendProfile = av::VideoPipelineProfile::SoftwareE2E;
                if (!configureEncoderForProfile(sendProfile,
                                                targetWidth,
                                                targetHeight,
                                                targetFrameRate,
                                                static_cast<int>(targetBitrate > 0U
                                                                     ? targetBitrate
                                                                     : static_cast<uint32_t>(m_config.bitrate)),
                                                payloadType)) {
                    sendTelemetryPipeline.onEncodeError(sendTelemetryState,
                                                        "video software fallback encoder configure failed",
                                                        statusCallback);
                    QMutexLocker locker(&m_mutex);
                    setErrorLocked("video software fallback encoder configure failed");
                    continue;
                }
                sendPipeline = VideoSendPipeline(
                    VideoSendPipelineConfig{targetFrameRate, m_config.maxPayloadBytes, sendProfile});
            }
            if (!gpuReadbackScreenCapture) {
                gpuReadbackScreenCapture = std::make_unique<WindowsD3D11ScreenReadbackCapture>();
            }
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - hardwareCaptureStartedAt).count();
            int64_t framePts = static_cast<int64_t>((elapsedMs * std::max(1, targetFrameRate)) / 1000);
            if (framePts <= lastHardwareCapturePts) {
                framePts = lastHardwareCapturePts + 1;
            }
            std::string captureError;
            av::capture::ScreenFrame readbackFrame;
            if (!gpuReadbackScreenCapture->capture(targetWidth,
                                                   targetHeight,
                                                   framePts,
                                                   readbackFrame,
                                                   &captureError)) {
                if (captureError.empty()) {
                    sendTelemetryPipeline.onEncodePending(sendTelemetryState, statusCallback);
                    continue;
                }
                {
                    QMutexLocker locker(&m_mutex);
                    (void)startCaptureLocked(false);
                }
                gpuReadbackScreenCaptureFailed = true;
                qInfo().noquote() << "[screen-session] video send profile fallback profile=software-cpu-capture reason="
                                  << QString::fromStdString(captureError);
                continue;
            }
            lastHardwareCapturePts = framePts;
            if (localCameraPreviewCallback) {
                av::codec::DecodedVideoFrame previewFrame;
                if (makeSoftwarePreviewFrame(readbackFrame, previewFrame)) {
                    localCameraPreviewCallback(std::move(previewFrame), VideoSendSource::Screen);
                    localScreenPreviewDispatched = true;
                }
            }
            capturedFrame.inputFrame.screenFrame = std::move(readbackFrame);
            if (!firstGpuReadbackScreenFrameObserved) {
                firstGpuReadbackScreenFrameObserved = true;
                if (statusCallback) {
                    statusCallback("Video screen capture active screenBackend=dxgi-duplication screenInterop=cpu-readback encoder=software");
                }
                qInfo().noquote() << "[screen-session] video screen capture active"
                                  << "screenBackend=dxgi-duplication"
                                  << "screenInterop=cpu-readback"
                                  << "profile=software";
            }
#else
            sendTelemetryPipeline.onEncodeError(sendTelemetryState,
                                                "DXGI readback screen capture unsupported on this platform",
                                                statusCallback);
            QMutexLocker locker(&m_mutex);
            setErrorLocked("DXGI readback screen capture unsupported on this platform");
            continue;
#endif
        } else if (useHardwareCameraCapture) {
#ifdef _WIN32
            std::string preferredCameraDeviceName;
            uint64_t observedCameraDeviceGeneration = 0;
            {
                QMutexLocker locker(&m_mutex);
                preferredCameraDeviceName = m_preferredCameraDeviceName;
                observedCameraDeviceGeneration =
                    m_cameraDeviceGeneration.load(std::memory_order_acquire);
            }
            if (hardwareCameraCapture &&
                activeHardwareCameraDeviceGeneration != std::numeric_limits<uint64_t>::max() &&
                activeHardwareCameraDeviceGeneration != observedCameraDeviceGeneration) {
                hardwareCameraCapture.reset();
                firstHardwareCameraFrameObserved = false;
                firstHardwareCameraPreviewFrameStored = false;
                if (statusCallback) {
                    statusCallback("Video hardware camera switching device cameraBackend=mf-d3d11 cameraInterop=dxgi");
                }
                qInfo().noquote() << "[screen-session] video hardware camera switching device"
                                  << "profile=" << av::videoPipelineProfileName(sendProfile)
                                  << "cameraBackend=mf-d3d11 cameraInterop=dxgi";
            }
            if (!hardwareCameraCapture) {
                hardwareCameraCapture = std::make_unique<av::capture::WindowsD3D11CameraCapture>();
            }
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - hardwareCaptureStartedAt).count();
            int64_t framePts = static_cast<int64_t>((elapsedMs * std::max(1, targetFrameRate)) / 1000);
            if (framePts <= lastHardwareCapturePts) {
                framePts = lastHardwareCapturePts + 1;
            }
            std::string captureError;
            if (!hardwareCameraCapture->isInitialized() &&
                !hardwareCameraCapture->initialize(encoder,
                                                   preferredCameraDeviceName,
                                                   targetWidth,
                                                   targetHeight,
                                                   targetFrameRate,
                                                   &captureError)) {
                if (!profileForced && !av::isHardwareE2E(requestedProfile)) {
                    {
                        QMutexLocker locker(&m_mutex);
                        (void)startCameraCaptureLocked(false);
                    }
                    sendProfile = av::VideoPipelineProfile::SoftwareE2E;
                    if (!configureEncoderForProfile(sendProfile,
                                                    targetWidth,
                                                    targetHeight,
                                                    targetFrameRate,
                                                    static_cast<int>(targetBitrate > 0U
                                                                         ? targetBitrate
                                                                         : static_cast<uint32_t>(m_config.bitrate)),
                                                    payloadType)) {
                        sendTelemetryPipeline.onEncodeError(sendTelemetryState,
                                                            "video software fallback encoder configure failed",
                                                            statusCallback);
                        QMutexLocker locker(&m_mutex);
                        setErrorLocked("video software fallback encoder configure failed");
                        continue;
                    }
                    qInfo().noquote() << "[screen-session] video send profile fallback profile=software reason="
                                      << QString::fromStdString(captureError);
                    sendPipeline = VideoSendPipeline(
                        VideoSendPipelineConfig{targetFrameRate, m_config.maxPayloadBytes, sendProfile});
                    continue;
                }
                sendTelemetryPipeline.onEncodeError(sendTelemetryState, captureError, statusCallback);
                QMutexLocker locker(&m_mutex);
                setErrorLocked(captureError);
                continue;
            }
            activeHardwareCameraDeviceGeneration = observedCameraDeviceGeneration;

            av::AVFramePtr hardwareFrame;
            captureError.clear();
            if (!hardwareCameraCapture->capture(encoder, framePts, hardwareFrame, &captureError)) {
                if (captureError.empty()) {
                    sendTelemetryPipeline.onEncodePending(sendTelemetryState, statusCallback);
                    continue;
                }
                if (!profileForced && !av::isHardwareE2E(requestedProfile)) {
                    {
                        QMutexLocker locker(&m_mutex);
                        (void)startCameraCaptureLocked(false);
                    }
                    sendProfile = av::VideoPipelineProfile::SoftwareE2E;
                    if (!configureEncoderForProfile(sendProfile,
                                                    targetWidth,
                                                    targetHeight,
                                                    targetFrameRate,
                                                    static_cast<int>(targetBitrate > 0U
                                                                         ? targetBitrate
                                                                         : static_cast<uint32_t>(m_config.bitrate)),
                                                    payloadType)) {
                        sendTelemetryPipeline.onEncodeError(sendTelemetryState,
                                                            "video software fallback encoder configure failed",
                                                            statusCallback);
                        QMutexLocker locker(&m_mutex);
                        setErrorLocked("video software fallback encoder configure failed");
                        continue;
                    }
                    qInfo().noquote() << "[screen-session] video send profile fallback profile=software reason="
                                      << QString::fromStdString(captureError);
                    sendPipeline = VideoSendPipeline(
                        VideoSendPipelineConfig{targetFrameRate, m_config.maxPayloadBytes, sendProfile});
                    continue;
                }
                sendTelemetryPipeline.onEncodeError(sendTelemetryState, captureError, statusCallback);
                QMutexLocker locker(&m_mutex);
                setErrorLocked(captureError);
                continue;
            }
            lastHardwareCapturePts = framePts;
            if (localCameraPreviewCallback) {
                av::codec::DecodedVideoFrame previewFrame;
                if (av::codec::makeD3D11HardwarePreviewFrame(*hardwareFrame, previewFrame)) {
                    localCameraPreviewCallback(std::move(previewFrame), VideoSendSource::Camera);
                    if (!firstHardwareCameraPreviewFrameStored) {
                        firstHardwareCameraPreviewFrameStored = true;
                        if (statusCallback) {
                            statusCallback("Video hardware camera localPreview=hardware_interop cameraBackend=mf-d3d11 cameraInterop=dxgi");
                        }
                        qInfo().noquote() << "[screen-session] video local preview cameraBackend=mf-d3d11 cameraInterop=dxgi"
                                          << "profile=" << av::videoPipelineProfileName(sendProfile)
                                          << "hardwareInput=1";
                    }
                } else if (!firstHardwareCameraPreviewFrameStored && statusCallback) {
                    statusCallback("Video hardware camera localPreview=unavailable cameraBackend=mf-d3d11 cameraInterop=dxgi");
                }
            }
            capturedFrame.inputFrame.avFrame = std::move(hardwareFrame);
            if (!firstHardwareCameraFrameObserved) {
                firstHardwareCameraFrameObserved = true;
                if (cameraSourceCallback) {
                    cameraSourceCallback(false);
                }
                if (statusCallback) {
                    statusCallback("Video camera frame observed cameraBackend=mf-d3d11 cameraInterop=dxgi localPreview=hardware_interop");
                }
                qInfo().noquote() << "[screen-session] video send cameraBackend=mf-d3d11 cameraInterop=dxgi"
                                  << "profile=" << av::videoPipelineProfileName(sendProfile)
                                  << "hardwareInput=" << (encoder.usesHardwareInput() ? 1 : 0);
            }
#else
            sendTelemetryPipeline.onEncodeError(sendTelemetryState,
                                                "hardware video pipeline unsupported on this platform",
                                                statusCallback);
            QMutexLocker locker(&m_mutex);
            setErrorLocked("hardware video pipeline unsupported on this platform");
            continue;
#endif
        }

        if (!localScreenPreviewDispatched &&
            source == VideoSendSource::Screen &&
            localCameraPreviewCallback &&
            capturedFrame.inputFrame.hasScreenFrame()) {
            av::codec::DecodedVideoFrame previewFrame;
            if (makeSoftwarePreviewFrame(capturedFrame.inputFrame.screenFrame, previewFrame)) {
                localCameraPreviewCallback(std::move(previewFrame), VideoSendSource::Screen);
                localScreenPreviewDispatched = true;
            }
        }
#ifndef _WIN32
        if (!firstSoftwareScreenFrameObserved &&
            source == VideoSendSource::Screen &&
            capturedFrame.inputFrame.hasScreenFrame()) {
            firstSoftwareScreenFrameObserved = true;
            if (statusCallback) {
                statusCallback("Video screen capture active screenBackend=portal-qt screenInterop=cpu-upload encoder=software");
            }
            qInfo().noquote() << "[screen-session] screen capture active"
                              << "profile=" << av::videoPipelineProfileName(sendProfile)
                              << "screenBackend=portal-qt"
                              << "screenInterop=cpu-upload"
                              << "encoder=software";
        }
#endif

        if (!peer.isValid() || videoSuspended) {
            maybeSendSenderReport();
            continue;
        }

        const bool forceKeyFrame = m_forceKeyFramePending.exchange(false, std::memory_order_acq_rel);
        std::vector<VideoSendPipelinePacket> packets;
        std::string pipelineError;
        bool encodedKeyFrame = false;
        VideoSendPipelineInputFrame adaptedFrame;
        const VideoSendPipelineInputFrame* frameForEncode = &capturedFrame.inputFrame;
        const bool frameAlreadyMatches =
            (capturedFrame.inputFrame.hasAvFrame() &&
             capturedFrame.inputFrame.avFrame->width == encoder.width() &&
             capturedFrame.inputFrame.avFrame->height == encoder.height()) ||
            (capturedFrame.inputFrame.hasScreenFrame() &&
             capturedFrame.inputFrame.screenFrame.width == encoder.width() &&
             capturedFrame.inputFrame.screenFrame.height == encoder.height());
        if (av::isHardwareE2E(sendProfile)) {
            if (!capturedFrame.inputFrame.hasHardwareD3D11Frame() &&
                !profileForced &&
                !av::isHardwareE2E(requestedProfile)) {
                sendProfile = av::VideoPipelineProfile::SoftwareE2E;
                if (!configureEncoderForProfile(sendProfile,
                                                targetWidth,
                                                targetHeight,
                                                targetFrameRate,
                                                static_cast<int>(targetBitrate > 0U
                                                                     ? targetBitrate
                                                                     : static_cast<uint32_t>(m_config.bitrate)),
                                                payloadType)) {
                    sendTelemetryPipeline.onEncodeError(sendTelemetryState,
                                                        "video software fallback encoder configure failed",
                                                        statusCallback);
                    QMutexLocker locker(&m_mutex);
                    setErrorLocked("video software fallback encoder configure failed");
                    continue;
                }
                qInfo().noquote() << "[screen-session] video send profile fallback profile=software reason=gpu input unavailable";
                sendPipeline = VideoSendPipeline(
                    VideoSendPipelineConfig{targetFrameRate, m_config.maxPayloadBytes, sendProfile});
            }
        }

        if (!frameAlreadyMatches) {
            std::string adaptError;
            if (!adaptVideoSendInputFrame(capturedFrame.inputFrame,
                                          encoder.width(),
                                          encoder.height(),
                                          sendProfile,
                                          adaptedFrame,
                                          &adaptError)) {
                sendTelemetryPipeline.onEncodeError(sendTelemetryState, adaptError, statusCallback);
                QMutexLocker locker(&m_mutex);
                setErrorLocked(adaptError.empty() ? "video adaptive frame prepare failed" : adaptError);
                continue;
            }
            frameForEncode = &adaptedFrame;
        }
        if (!sendPipeline.encodeAndPacketize(encoder,
                                             *frameForEncode,
                                             payloadType,
                                             forceKeyFrame,
                                             m_sender,
                                             packets,
                                             &encodedKeyFrame,
                                             &pipelineError)) {
            if (pipelineError.empty()) {
                sendTelemetryPipeline.onEncodePending(sendTelemetryState, statusCallback);
                continue;
            }
            sendTelemetryPipeline.onEncodeError(sendTelemetryState, pipelineError, statusCallback);
            QMutexLocker locker(&m_mutex);
            setErrorLocked(pipelineError);
            continue;
        }
        sendTelemetryPipeline.onEncodedPacketObserved(sendTelemetryState, statusCallback);
        if (forceKeyFrame && !encodedKeyFrame) {
            m_forceKeyFramePending.store(true, std::memory_order_release);
        }

        for (const auto& packet : packets) {
            {
                QMutexLocker locker(&m_mutex);
                if (m_dtlsStarted.load(std::memory_order_acquire) &&
                    !m_srtpReady.load(std::memory_order_acquire)) {
                    (void)sendCachedDtlsHandshakeLocked(peer);
                    continue;
                }
            }

            std::vector<uint8_t> outboundPacket = packet.bytes;
            {
                QMutexLocker locker(&m_mutex);
                if (!protectRtpLocked(&outboundPacket)) {
                    continue;
                }
            }
            const int sent = m_mediaSocket.sendTo(outboundPacket.data(), outboundPacket.size(), peer);
            if (sent != static_cast<int>(outboundPacket.size())) {
                QMutexLocker locker(&m_mutex);
                setErrorLocked("sendto failed");
                break;
            }
            m_sentPacketCount.fetch_add(1, std::memory_order_acq_rel);
            ++rtpPacketCount;
            const uint32_t payloadOctets = packet.bytes.size() > media::kRtpMinHeaderSize
                ? static_cast<uint32_t>(packet.bytes.size() - media::kRtpMinHeaderSize)
                : 0U;
            rtpOctetCount += payloadOctets;
            lastRtpTimestamp = packet.timestamp;
            {
                QMutexLocker locker(&m_mutex);
                m_rtcpActionPipeline.cacheSentPacket(packet.sequenceNumber, outboundPacket);
            }
            sendTelemetryPipeline.onPacketSent(sendTelemetryState, packet, statusCallback);
        }
        maybeSendSenderReport();
    }
}

void ScreenShareSession::recvLoop() {
    std::array<uint8_t, 1500> buffer{};
    bool loggedFirstPacket = false;
    const auto statusCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_statusCallback;
    }();
    bool loggedFirstDecodedFrame = false;
    VideoRecvConsumePipeline recvConsumePipeline;
    VideoRecvErrorPipeline recvErrorPipeline;
    VideoRecvFrameDispatchPipeline frameDispatchPipeline;
    VideoRecvIngressPipeline recvIngressPipeline;
    VideoRecvRtcpPipeline recvRtcpPipeline;
    VideoRecvTelemetryPipeline recvTelemetryPipeline;
    const VideoRecvPipelineConfig recvPipelineConfig{
        m_config.payloadType,
        m_config.cameraPayloadType,
        m_config.frameRate};
    VideoRecvPipeline datagramPipeline(recvPipelineConfig);
    VideoRecvKeyFramePipeline keyFramePipeline;
    std::unordered_map<uint32_t, std::unique_ptr<VideoRecvPeerRuntime>> recvPeerWorkers;
    QMutex consumeActionMutex;
    QMutex telemetryMutex;
    QMutex pendingKeyFrameMutex;
    QMutex pendingNackMutex;
    struct PendingKeyFrameRequest {
        uint32_t remoteMediaSsrc{0U};
        std::string reason;
    };
    struct PendingNackRequest {
        uint32_t remoteMediaSsrc{0U};
        std::vector<uint16_t> missingSequences;
        uint64_t firstRequestedAtMs{0U};
        uint64_t lastSentAtMs{0U};
        uint32_t attempts{0U};
    };
    std::deque<PendingKeyFrameRequest> pendingKeyFrameRequests;
    std::unordered_map<uint32_t, PendingNackRequest> pendingNackRequests;

    const auto stopAllRecvPeerWorkers = [&recvPeerWorkers]() {
        for (auto& [remoteMediaSsrc, worker] : recvPeerWorkers) {
            (void)remoteMediaSsrc;
            if (worker) {
                worker->stop();
            }
        }
        recvPeerWorkers.clear();
    };

    const auto pruneRecvPeerWorkers = [&recvPeerWorkers](uint64_t nowMs,
                                                         uint32_t expectedRemoteSsrc) {
        for (auto it = recvPeerWorkers.begin(); it != recvPeerWorkers.end();) {
            const uint32_t remoteMediaSsrc = it->first;
            const bool keepExpectedOnly =
                expectedRemoteSsrc != 0U && remoteMediaSsrc != expectedRemoteSsrc;
            const bool idleExpired =
                expectedRemoteSsrc == 0U &&
                nowMs >= it->second->lastActiveAtMs() &&
                (nowMs - it->second->lastActiveAtMs()) > kRecvPeerWorkerIdleTimeoutMs;
            if (!keepExpectedOnly && !idleExpired) {
                ++it;
                continue;
            }
            if (it->second) {
                it->second->stop();
            }
            it = recvPeerWorkers.erase(it);
        }
    };

    const auto resetRequestedRecvPeerWorkers = [this, &recvPeerWorkers]() {
        std::vector<uint32_t> resetRequests;
        {
            QMutexLocker locker(&m_mutex);
            resetRequests.swap(m_remoteVideoStreamResetRequests);
        }

        for (const uint32_t remoteMediaSsrc : resetRequests) {
            const auto it = recvPeerWorkers.find(remoteMediaSsrc);
            if (it == recvPeerWorkers.end()) {
                continue;
            }
            if (it->second) {
                it->second->stop();
            }
            recvPeerWorkers.erase(it);
        }
    };

    const auto enqueueKeyFrameRequest = [&pendingKeyFrameMutex, &pendingKeyFrameRequests](
                                            uint32_t remoteMediaSsrc,
                                            std::string reason) {
        if (remoteMediaSsrc == 0U) {
            return;
        }
        QMutexLocker locker(&pendingKeyFrameMutex);
        if (pendingKeyFrameRequests.size() >= 64U) {
            pendingKeyFrameRequests.pop_front();
        }
        pendingKeyFrameRequests.push_back(PendingKeyFrameRequest{
            remoteMediaSsrc,
            std::move(reason),
        });
    };

    const auto enqueueNackRequest =
        [&pendingNackMutex, &pendingNackRequests](uint32_t remoteMediaSsrc,
                                                  const std::vector<uint16_t>& missingSequences) {
            if (remoteMediaSsrc == 0U || missingSequences.empty()) {
                return;
            }
            QMutexLocker locker(&pendingNackMutex);
            if (pendingNackRequests.size() >= kMaxPendingNackRequests &&
                pendingNackRequests.find(remoteMediaSsrc) == pendingNackRequests.end()) {
                pendingNackRequests.erase(pendingNackRequests.begin());
            }

            PendingNackRequest& request = pendingNackRequests[remoteMediaSsrc];
            request.remoteMediaSsrc = remoteMediaSsrc;
            if (request.firstRequestedAtMs == 0U) {
                request.firstRequestedAtMs = steadyNowMs();
            }
            for (const uint16_t sequenceNumber : missingSequences) {
                if (std::find(request.missingSequences.begin(),
                              request.missingSequences.end(),
                              sequenceNumber) != request.missingSequences.end()) {
                    continue;
                }
                request.missingSequences.push_back(sequenceNumber);
                if (request.missingSequences.size() >= 32U) {
                    break;
                }
            }
        };

    const auto clearPendingNackRequest =
        [&pendingNackMutex, &pendingNackRequests](uint32_t remoteMediaSsrc) {
            if (remoteMediaSsrc == 0U) {
                return;
            }
            QMutexLocker locker(&pendingNackMutex);
            pendingNackRequests.erase(remoteMediaSsrc);
        };

    const auto flushPendingNackRequests =
        [this,
         &pendingNackMutex,
         &pendingNackRequests,
         &enqueueKeyFrameRequest]() {
            struct NackSendRequest {
                uint32_t remoteMediaSsrc{0U};
                std::vector<uint16_t> missingSequences;
            };
            std::vector<NackSendRequest> nackSends;
            std::vector<uint32_t> fallbackKeyFrames;
            const uint64_t nowMs = steadyNowMs();
            {
                QMutexLocker locker(&pendingNackMutex);
                for (auto it = pendingNackRequests.begin(); it != pendingNackRequests.end();) {
                    PendingNackRequest& request = it->second;
                    if (request.remoteMediaSsrc == 0U || request.missingSequences.empty()) {
                        it = pendingNackRequests.erase(it);
                        continue;
                    }
                    if (request.attempts >= kNackAttemptsBeforePli &&
                        nowMs >= request.firstRequestedAtMs &&
                        nowMs - request.firstRequestedAtMs >= kNackPliFallbackMs) {
                        fallbackKeyFrames.push_back(request.remoteMediaSsrc);
                        it = pendingNackRequests.erase(it);
                        continue;
                    }
                    if (request.attempts == 0U ||
                        (nowMs >= request.lastSentAtMs &&
                         nowMs - request.lastSentAtMs >= kNackRetryIntervalMs)) {
                        nackSends.push_back(NackSendRequest{
                            request.remoteMediaSsrc,
                            request.missingSequences,
                        });
                        request.lastSentAtMs = nowMs;
                        ++request.attempts;
                    }
                    ++it;
                }
            }

            for (const NackSendRequest& request : nackSends) {
                bool nackSent = false;
                {
                    QMutexLocker locker(&m_mutex);
                    std::string nackError;
                    std::vector<uint8_t> nackPacket =
                        m_rtcpActionPipeline.buildNackFeedback(
                            m_sender.ssrc(),
                            request.remoteMediaSsrc,
                            request.missingSequences);
                    bool readyToSend = !nackPacket.empty();
                    if (readyToSend && m_dtlsStarted.load(std::memory_order_acquire)) {
                        readyToSend = protectRtcpLocked(&nackPacket);
                        if (!readyToSend) {
                            nackError = "NACK SRTCP protect failed";
                        }
                    }
                    if (readyToSend) {
                        const int sent = m_mediaSocket.sendToPeer(nackPacket.data(), nackPacket.size());
                        nackSent = sent == static_cast<int>(nackPacket.size());
                        if (!nackSent) {
                            nackError = "NACK sendto failed";
                        }
                    } else if (nackPacket.empty()) {
                        nackError = "NACK packet build failed";
                    }
                    if (!nackSent) {
                        setErrorLocked(nackError);
                    }
                }
            }

            for (const uint32_t remoteMediaSsrc : fallbackKeyFrames) {
                enqueueKeyFrameRequest(remoteMediaSsrc, "packet loss");
            }
        };

    const auto flushPendingKeyFrameRequests =
        [this,
         &pendingKeyFrameMutex,
         &pendingKeyFrameRequests,
         &keyFramePipeline,
         &statusCallback]() {
            std::deque<PendingKeyFrameRequest> requests;
            {
                QMutexLocker locker(&pendingKeyFrameMutex);
                requests.swap(pendingKeyFrameRequests);
            }

            for (auto& request : requests) {
                if (request.remoteMediaSsrc == 0U) {
                    continue;
                }

                const uint64_t nowMs = steadyNowMs();
                if (!keyFramePipeline.shouldSendPli(request.remoteMediaSsrc, nowMs)) {
                    continue;
                }

                bool pliSent = false;
                {
                    QMutexLocker locker(&m_mutex);
                    std::string pliError;
                    std::vector<uint8_t> pliPacket =
                        m_rtcpActionPipeline.buildPictureLossIndication(
                            m_sender.ssrc(), request.remoteMediaSsrc);
                    bool readyToSend = !pliPacket.empty();
                    if (readyToSend && m_dtlsStarted.load(std::memory_order_acquire)) {
                        readyToSend = protectRtcpLocked(&pliPacket);
                        if (!readyToSend) {
                            pliError = "PLI SRTCP protect failed";
                        }
                    }
                    if (readyToSend) {
                        const int sent = m_mediaSocket.sendToPeer(pliPacket.data(), pliPacket.size());
                        pliSent = sent == static_cast<int>(pliPacket.size());
                        if (!pliSent) {
                            pliError = "PLI sendto failed";
                        }
                    } else if (pliPacket.empty()) {
                        pliError = "PLI packet build failed";
                    }
                    if (!pliSent) {
                        setErrorLocked(pliError);
                    }
                }

                if (!pliSent) {
                    continue;
                }

                keyFramePipeline.markPliSent(request.remoteMediaSsrc, nowMs);
                m_keyframeRequestCount.fetch_add(1, std::memory_order_acq_rel);
                if (statusCallback) {
                    statusCallback(std::string("Video keyframe requested: ") + request.reason);
                }
            }
        };

    const auto handleConsumeOutcome =
        [this,
         &recvErrorPipeline,
         &frameDispatchPipeline,
         &enqueueKeyFrameRequest,
         &enqueueNackRequest,
         &clearPendingNackRequest,
         &statusCallback,
         &consumeActionMutex,
         &loggedFirstDecodedFrame](const VideoRecvConsumeOutcome& consumeOutcome,
                                   av::codec::DecodedVideoFrame decoded) {
            QMutexLocker consumeLocker(&consumeActionMutex);
            const VideoRecvHandlingDecision& decision = consumeOutcome.decision;
            if (decision.action == VideoRecvHandlingAction::Continue) {
                return;
            }

            if (decision.action == VideoRecvHandlingAction::RequestRetransmit) {
                enqueueNackRequest(consumeOutcome.remoteMediaSsrc,
                                   decision.retransmitSequenceNumbers);
                return;
            }

            if (decision.action == VideoRecvHandlingAction::RequestKeyFrame ||
                decision.action == VideoRecvHandlingAction::RequestKeyFrameAndError) {
                const char* keyFrameReason =
                    decision.keyFrameReason != nullptr ? decision.keyFrameReason : "decode failure";
                enqueueKeyFrameRequest(consumeOutcome.remoteMediaSsrc, keyFrameReason);

                std::string decisionError;
                if (recvErrorPipeline.extractDecisionError(decision, decisionError)) {
                    QMutexLocker locker(&m_mutex);
                    setErrorLocked(decisionError);
                }
                return;
            }

            if (decision.action != VideoRecvHandlingAction::DeliverFrame) {
                return;
            }
            clearPendingNackRequest(consumeOutcome.remoteMediaSsrc);

            std::function<void(av::codec::DecodedVideoFrame)> callback;
            std::function<void(av::codec::DecodedVideoFrame, uint32_t)> callbackWithSsrc;
            {
                QMutexLocker locker(&m_mutex);
                callback = m_decodedFrameCallback;
                callbackWithSsrc = m_decodedFrameWithSsrcCallback;
            }
            frameDispatchPipeline.reportFirstDecodedFrame(
                loggedFirstDecodedFrame, decoded, statusCallback);
            frameDispatchPipeline.dispatchFrame(
                callback,
                callbackWithSsrc,
                std::move(decoded),
                consumeOutcome.remoteMediaSsrc);
        };

    const auto ensureRecvPeerWorker =
        [&recvPeerWorkers,
         &recvPipelineConfig,
         &handleConsumeOutcome,
         &recvTelemetryPipeline,
         &telemetryMutex,
         &loggedFirstPacket,
         &statusCallback,
         this](uint32_t remoteMediaSsrc) -> VideoRecvPeerRuntime* {
            if (remoteMediaSsrc == 0U) {
                return nullptr;
            }

            auto found = recvPeerWorkers.find(remoteMediaSsrc);
            if (found != recvPeerWorkers.end()) {
                return found->second.get();
            }

            if (recvPeerWorkers.size() >= kMaxRecvPeerWorkers) {
                auto oldest = recvPeerWorkers.end();
                uint64_t oldestActiveAtMs = (std::numeric_limits<uint64_t>::max)();
                for (auto it = recvPeerWorkers.begin(); it != recvPeerWorkers.end(); ++it) {
                    const uint64_t activeAtMs = it->second->lastActiveAtMs();
                    if (activeAtMs < oldestActiveAtMs) {
                        oldestActiveAtMs = activeAtMs;
                        oldest = it;
                    }
                }
                if (oldest != recvPeerWorkers.end()) {
                    if (oldest->second) {
                        oldest->second->stop();
                    }
                    recvPeerWorkers.erase(oldest);
                }
            }

            auto worker = std::make_unique<VideoRecvPeerRuntime>(
                remoteMediaSsrc,
                recvPipelineConfig,
                m_adaptiveJitterTargetMs,
                [this, &recvTelemetryPipeline, &telemetryMutex, &loggedFirstPacket, &statusCallback, &handleConsumeOutcome](
                    const media::RTPPacket& packet,
                    const VideoRecvConsumeOutcome& consumeOutcome,
                    av::codec::DecodedVideoFrame&& decoded) mutable {
                    {
                        QMutexLocker telemetryLocker(&telemetryMutex);
                        recvTelemetryPipeline.onPacketAccepted(
                            m_receivedPacketCount,
                            loggedFirstPacket,
                            packet,
                            statusCallback);
                    }
                    handleConsumeOutcome(consumeOutcome, std::move(decoded));
                },
                [&handleConsumeOutcome](const VideoRecvConsumeOutcome& consumeOutcome,
                                        av::codec::DecodedVideoFrame&& decoded) mutable {
                    handleConsumeOutcome(consumeOutcome, std::move(decoded));
                });
            worker->start();
            VideoRecvPeerRuntime* workerRaw = worker.get();
            recvPeerWorkers.emplace(remoteMediaSsrc, std::move(worker));
            return workerRaw;
        };

    while (m_running.load(std::memory_order_acquire)) {
        flushPendingNackRequests();
        flushPendingKeyFrameRequests();
        resetRequestedRecvPeerWorkers();
        const uint32_t expectedRemoteSsrc =
            m_expectedRemoteVideoSsrc.load(std::memory_order_acquire);
        pruneRecvPeerWorkers(steadyNowMs(), expectedRemoteSsrc);

        const int waitResult = m_mediaSocket.waitForReadable(kRecvLoopWaitTimeoutMs);
        if (waitResult < 0) {
            const bool transientSocketError = m_mediaSocket.isTransientSocketError();
            if (!recvErrorPipeline.shouldReportSocketError(
                    m_running.load(std::memory_order_acquire), transientSocketError)) {
                continue;
            }
            QMutexLocker locker(&m_mutex);
            setErrorLocked("wait readable failed");
            continue;
        }

        if (waitResult > 0) {
            media::UdpEndpoint from{};
            const int received = m_mediaSocket.recvFrom(buffer.data(), buffer.size(), from);
            if (received <= 0) {
                const bool transientSocketError = m_mediaSocket.isTransientSocketError();
                if (!recvErrorPipeline.shouldReportSocketError(
                        m_running.load(std::memory_order_acquire), transientSocketError)) {
                    continue;
                }
                QMutexLocker locker(&m_mutex);
                setErrorLocked("recvfrom failed");
                continue;
            }

            const QByteArray receivedDatagram(reinterpret_cast<const char*>(buffer.data()), received);
            if (media::DtlsTransportClient::looksLikeDtlsRecord(receivedDatagram)) {
                QMutexLocker locker(&m_mutex);
                (void)handleDtlsPacketLocked(buffer.data(), static_cast<std::size_t>(received), from);
                continue;
            }
            if (looksLikeStunPacket(buffer.data(), static_cast<std::size_t>(received))) {
                m_iceConnected.store(true, std::memory_order_release);
                if (statusCallback) {
                    statusCallback("Video ice-connected binding-response received");
                }
                continue;
            }

            std::vector<uint8_t> mediaPacket(buffer.begin(), buffer.begin() + received);
            if (m_dtlsStarted.load(std::memory_order_acquire)) {
                QMutexLocker locker(&m_mutex);
                const bool looksLikeRtcp = media::looksLikeRtcpPacket(mediaPacket.data(), mediaPacket.size());
                const bool unprotected = looksLikeRtcp
                    ? unprotectRtcpLocked(&mediaPacket)
                    : unprotectRtpLocked(&mediaPacket);
                if (!unprotected) {
                    continue;
                }
            }

            const VideoRecvIngressGate ingressGate =
                recvIngressPipeline.evaluateGate(m_mediaSocket, from);

            VideoRecvDatagram datagram = datagramPipeline.classifyDatagram(
                mediaPacket.data(),
                mediaPacket.size(),
                ingressGate.acceptSender,
                ingressGate.acceptRtcpFromPeerHost,
                m_receiver);
            const VideoRecvIngressAction ingressAction =
                recvIngressPipeline.resolveEntryAction(datagram.kind);
            if (ingressAction == VideoRecvIngressAction::Rtcp) {
                uint32_t localSsrc = 0U;
                {
                    QMutexLocker locker(&m_mutex);
                    localSsrc = m_sender.ssrc();
                }

                VideoRtcpFeedbackDispatchPlan dispatchPlan;
                if (recvRtcpPipeline.parseDispatchPlan(
                        mediaPacket.data(),
                        mediaPacket.size(),
                        localSsrc,
                        m_rtcpFeedbackPipeline,
                        m_rtcpFeedbackDispatchPipeline,
                        dispatchPlan)) {
                    QMutexLocker locker(&m_mutex);
                    (void)applyRtcpDispatchPlanLocked(dispatchPlan);
                }
            } else if (ingressAction == VideoRecvIngressAction::Rtp) {
                const uint32_t packetSsrc = datagram.rtpPacket.header.ssrc;
                if (expectedRemoteSsrc != 0U && packetSsrc != expectedRemoteSsrc) {
                    continue;
                }
                VideoRecvPeerRuntime* worker = ensureRecvPeerWorker(packetSsrc);
                if (worker == nullptr) {
                    continue;
                }
                if (!worker->enqueue(std::move(datagram.rtpPacket))) {
                    QMutexLocker locker(&m_mutex);
                    setErrorLocked("recv worker enqueue failed");
                }
            }
        }
    }

    flushPendingNackRequests();
    flushPendingKeyFrameRequests();
    stopAllRecvPeerWorkers();
}

}  // namespace av::session
