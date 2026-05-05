#include "WindowsD3D11CameraCapture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <QString>
#endif

namespace av::capture {
namespace {

void setError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

#ifdef _WIN32
std::string hresultString(HRESULT hr) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(hr));
    return buffer;
}

template <typename T>
void releaseComObject(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

std::wstring toWideDeviceFilter(const std::string& value) {
    return QString::fromStdString(value).trimmed().toStdWString();
}

std::wstring allocatedString(IMFActivate* activate, const GUID& key) {
    if (activate == nullptr) {
        return {};
    }
    WCHAR* value = nullptr;
    UINT32 length = 0;
    if (FAILED(activate->GetAllocatedString(key, &value, &length)) || value == nullptr) {
        return {};
    }
    std::wstring result(value, value + length);
    CoTaskMemFree(value);
    return result;
}

bool containsCaseInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }
    std::wstring normalizedHaystack = haystack;
    std::wstring normalizedNeedle = needle;
    std::transform(normalizedHaystack.begin(),
                   normalizedHaystack.end(),
                   normalizedHaystack.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    std::transform(normalizedNeedle.begin(),
                   normalizedNeedle.end(),
                   normalizedNeedle.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return normalizedHaystack.find(normalizedNeedle) != std::wstring::npos;
}

#endif

}  // namespace

struct WindowsD3D11CameraCapture::Impl {
#ifdef _WIN32
    bool initialize(av::codec::VideoEncoder& encoder,
                    const std::string& preferredDeviceName,
                    int targetWidth,
                    int targetHeight,
                    int frameRate,
                    std::string* error) {
        shutdown();
        m_targetWidth = std::max(2, targetWidth & ~1);
        m_targetHeight = std::max(2, targetHeight & ~1);
        m_frameRate = std::max(1, frameRate);

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            m_comInitialized = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            setError(error, "COM initialize failed: " + hresultString(hr));
            return false;
        }

        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            setError(error, "Media Foundation startup failed: " + hresultString(hr));
            shutdown();
            return false;
        }
        m_mfStarted = true;

        m_device = static_cast<ID3D11Device*>(encoder.d3d11Device());
        if (m_device == nullptr || encoder.hardwareFramesContext() == nullptr) {
            setError(error, "hardware camera capture requires D3D11 encoder device");
            shutdown();
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

        UINT resetToken = 0;
        hr = MFCreateDXGIDeviceManager(&resetToken, &m_dxgiManager);
        if (FAILED(hr) || m_dxgiManager == nullptr) {
            setError(error, "MF DXGI device manager create failed: " + hresultString(hr));
            shutdown();
            return false;
        }
        hr = m_dxgiManager->ResetDevice(m_device, resetToken);
        if (FAILED(hr)) {
            setError(error, "MF DXGI device manager reset failed: " + hresultString(hr));
            shutdown();
            return false;
        }

        IMFActivate* selectedActivate = nullptr;
        if (!selectCameraActivate(preferredDeviceName, &selectedActivate, error)) {
            shutdown();
            return false;
        }

        hr = selectedActivate->ActivateObject(__uuidof(IMFMediaSource), reinterpret_cast<void**>(&m_source));
        releaseComObject(selectedActivate);
        if (FAILED(hr) || m_source == nullptr) {
            setError(error, "Media Foundation camera source activate failed: " + hresultString(hr));
            shutdown();
            return false;
        }

        IMFAttributes* readerAttributes = nullptr;
        hr = MFCreateAttributes(&readerAttributes, 4);
        if (FAILED(hr) || readerAttributes == nullptr) {
            setError(error, "Media Foundation source reader attributes failed: " + hresultString(hr));
            shutdown();
            return false;
        }
        readerAttributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_dxgiManager);
        readerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
        readerAttributes->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);

        hr = MFCreateSourceReaderFromMediaSource(m_source, readerAttributes, &m_reader);
        releaseComObject(readerAttributes);
        if (FAILED(hr) || m_reader == nullptr) {
            setError(error, "Media Foundation source reader create failed: " + hresultString(hr));
            shutdown();
            return false;
        }

        if (!configureMediaType(error)) {
            shutdown();
            return false;
        }
        m_initialized = true;
        return true;
    }

    bool capture(av::codec::VideoEncoder& encoder,
                 int64_t pts,
                 av::AVFramePtr& outFrame,
                 std::string* error) {
        outFrame.reset();
        if (!m_initialized || m_reader == nullptr) {
            setError(error, "hardware camera capture is not initialized");
            return false;
        }

        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = m_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                          0,
                                          &streamIndex,
                                          &flags,
                                          &timestamp,
                                          &sample);
        if (FAILED(hr)) {
            if (hr == MF_E_HW_MFT_FAILED_START_STREAMING) {
                setError(error, "hardware camera capture requires DXGI surface sample; CPU sample disabled");
            } else {
                setError(error, "Media Foundation camera ReadSample failed: " + hresultString(hr));
            }
            return false;
        }
        if ((flags & MF_SOURCE_READERF_ERROR) != 0U) {
            setError(error, "Media Foundation camera stream error");
            return false;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0U) {
            setError(error, "Media Foundation camera stream ended");
            return false;
        }
        if (sample == nullptr) {
            if (error != nullptr) {
                error->clear();
            }
            return false;
        }

        IMFMediaBuffer* buffer = nullptr;
        IMFDXGIBuffer* dxgiBuffer = nullptr;
        ID3D11Texture2D* sourceTexture = nullptr;
        hr = sample->GetBufferByIndex(0, &buffer);
        if (SUCCEEDED(hr) && buffer != nullptr) {
            hr = buffer->QueryInterface(__uuidof(IMFDXGIBuffer), reinterpret_cast<void**>(&dxgiBuffer));
        }
        if (SUCCEEDED(hr) && dxgiBuffer != nullptr) {
            hr = dxgiBuffer->GetResource(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&sourceTexture));
        }
        if (FAILED(hr) || sourceTexture == nullptr || dxgiBuffer == nullptr) {
            releaseComObject(sourceTexture);
            releaseComObject(dxgiBuffer);
            releaseComObject(buffer);
            releaseComObject(sample);
            setError(error, "hardware camera capture requires DXGI surface sample; CPU sample disabled");
            return false;
        }

        UINT sourceSubresource = 0;
        dxgiBuffer->GetSubresourceIndex(&sourceSubresource);
        outFrame = av::makeFrame();
        if (!outFrame) {
            releaseComObject(sourceTexture);
            releaseComObject(dxgiBuffer);
            releaseComObject(buffer);
            releaseComObject(sample);
            setError(error, "hardware camera frame allocation failed");
            return false;
        }
        if (av_hwframe_get_buffer(encoder.hardwareFramesContext(), outFrame.get(), 0) < 0) {
            outFrame.reset();
            releaseComObject(sourceTexture);
            releaseComObject(dxgiBuffer);
            releaseComObject(buffer);
            releaseComObject(sample);
            setError(error, "D3D11 hardware camera frame pool allocation failed");
            return false;
        }
        outFrame->pts = pts;

        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceTexture->GetDesc(&sourceDesc);
        const bool converted = ensureVideoProcessor(static_cast<int>(sourceDesc.Width),
                                                    static_cast<int>(sourceDesc.Height),
                                                    error) &&
                               blitToEncoderFrame(sourceTexture, sourceSubresource, *outFrame, error);
        releaseComObject(sourceTexture);
        releaseComObject(dxgiBuffer);
        releaseComObject(buffer);
        releaseComObject(sample);
        if (!converted) {
            outFrame.reset();
            return false;
        }
        if (static_cast<AVPixelFormat>(outFrame->format) != AV_PIX_FMT_D3D11 ||
            outFrame->hw_frames_ctx == nullptr) {
            outFrame.reset();
            setError(error, "hardware camera capture produced invalid D3D11 AVFrame");
            return false;
        }
        return true;
    }

    void shutdown() {
        releaseComObject(m_videoProcessor);
        releaseComObject(m_processorEnumerator);
        releaseComObject(m_reader);
        if (m_source != nullptr) {
            m_source->Shutdown();
        }
        releaseComObject(m_source);
        releaseComObject(m_dxgiManager);
        releaseComObject(m_videoContext);
        releaseComObject(m_videoDevice);
        releaseComObject(m_context);
        releaseComObject(m_device);
        m_initialized = false;
        m_sourceWidth = 0;
        m_sourceHeight = 0;
        m_sourceSubtype = GUID_NULL;
        m_backendName.clear();
        if (m_mfStarted) {
            MFShutdown();
            m_mfStarted = false;
        }
        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
    }

    bool selectCameraActivate(const std::string& preferredDeviceName,
                              IMFActivate** selectedActivate,
                              std::string* error) {
        if (selectedActivate == nullptr) {
            setError(error, "invalid camera activate output");
            return false;
        }
        *selectedActivate = nullptr;

        IMFAttributes* enumAttributes = nullptr;
        HRESULT hr = MFCreateAttributes(&enumAttributes, 1);
        if (FAILED(hr) || enumAttributes == nullptr) {
            setError(error, "Media Foundation camera enum attributes failed: " + hresultString(hr));
            return false;
        }
        enumAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate** activates = nullptr;
        UINT32 activateCount = 0;
        hr = MFEnumDeviceSources(enumAttributes, &activates, &activateCount);
        releaseComObject(enumAttributes);
        if (FAILED(hr) || activates == nullptr || activateCount == 0) {
            setError(error, "no Media Foundation video capture devices available");
            if (activates != nullptr) {
                CoTaskMemFree(activates);
            }
            return false;
        }

        const std::wstring preferred = toWideDeviceFilter(preferredDeviceName);
        IMFActivate* chosen = nullptr;
        for (UINT32 i = 0; i < activateCount; ++i) {
            IMFActivate* activate = activates[i];
            const std::wstring friendlyName =
                allocatedString(activate, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            const std::wstring symbolicLink =
                allocatedString(activate, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            if (chosen == nullptr && preferred.empty()) {
                chosen = activate;
                chosen->AddRef();
            } else if (!preferred.empty() &&
                       (containsCaseInsensitive(friendlyName, preferred) ||
                        containsCaseInsensitive(symbolicLink, preferred))) {
                chosen = activate;
                chosen->AddRef();
                break;
            }
        }

        for (UINT32 i = 0; i < activateCount; ++i) {
            releaseComObject(activates[i]);
        }
        CoTaskMemFree(activates);

        if (chosen == nullptr) {
            setError(error, "preferred Media Foundation camera not found");
            return false;
        }
        m_backendName = "mf-d3d11";
        *selectedActivate = chosen;
        return true;
    }

    bool configureMediaType(std::string* error) {
        const GUID preferredSubtypes[] = {
            MFVideoFormat_NV12,
            MFVideoFormat_YUY2,
            MFVideoFormat_RGB32,
        };
        for (const GUID& subtype : preferredSubtypes) {
            if (setReaderMediaType(subtype, m_targetWidth, m_targetHeight, m_frameRate)) {
                return readCurrentMediaType(error);
            }
        }

        IMFMediaType* nativeType = nullptr;
        for (DWORD i = 0;
             SUCCEEDED(m_reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nativeType));
             ++i) {
            GUID subtype = GUID_NULL;
            UINT32 width = 0;
            UINT32 height = 0;
            nativeType->GetGUID(MF_MT_SUBTYPE, &subtype);
            MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &width, &height);
            releaseComObject(nativeType);
            if ((subtype == MFVideoFormat_NV12 ||
                 subtype == MFVideoFormat_YUY2 ||
                 subtype == MFVideoFormat_RGB32) &&
                width > 0 && height > 0 &&
                setReaderMediaType(subtype,
                                   static_cast<int>(width),
                                   static_cast<int>(height),
                                   m_frameRate)) {
                return readCurrentMediaType(error);
            }
        }

        setError(error, "Media Foundation camera cannot configure GPU-compatible NV12/YUY2/RGB32 media type");
        return false;
    }

    bool setReaderMediaType(const GUID& subtype, int width, int height, int frameRate) {
        IMFMediaType* mediaType = nullptr;
        HRESULT hr = MFCreateMediaType(&mediaType);
        if (FAILED(hr) || mediaType == nullptr) {
            return false;
        }
        mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mediaType->SetGUID(MF_MT_SUBTYPE, subtype);
        mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(mediaType, MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
        MFSetAttributeRatio(mediaType, MF_MT_FRAME_RATE, static_cast<UINT32>(std::max(1, frameRate)), 1);
        MFSetAttributeRatio(mediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = m_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType);
        releaseComObject(mediaType);
        return SUCCEEDED(hr);
    }

    bool readCurrentMediaType(std::string* error) {
        IMFMediaType* currentType = nullptr;
        HRESULT hr = m_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType);
        if (FAILED(hr) || currentType == nullptr) {
            setError(error, "Media Foundation current camera media type unavailable: " + hresultString(hr));
            return false;
        }
        UINT32 width = 0;
        UINT32 height = 0;
        currentType->GetGUID(MF_MT_SUBTYPE, &m_sourceSubtype);
        MFGetAttributeSize(currentType, MF_MT_FRAME_SIZE, &width, &height);
        releaseComObject(currentType);
        if (width == 0 || height == 0) {
            setError(error, "Media Foundation camera media type has invalid size");
            return false;
        }
        m_sourceWidth = static_cast<int>(width);
        m_sourceHeight = static_cast<int>(height);
        return ensureVideoProcessor(m_sourceWidth, m_sourceHeight, error);
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
        contentDesc.InputWidth = static_cast<UINT>(m_sourceWidth);
        contentDesc.InputHeight = static_cast<UINT>(m_sourceHeight);
        contentDesc.OutputWidth = static_cast<UINT>(m_targetWidth);
        contentDesc.OutputHeight = static_cast<UINT>(m_targetHeight);
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &m_processorEnumerator);
        if (FAILED(hr) || m_processorEnumerator == nullptr) {
            setError(error, "CreateVideoProcessorEnumerator camera failed: " + hresultString(hr));
            return false;
        }
        hr = m_videoDevice->CreateVideoProcessor(m_processorEnumerator, 0, &m_videoProcessor);
        if (FAILED(hr) || m_videoProcessor == nullptr) {
            setError(error, "CreateVideoProcessor camera failed: " + hresultString(hr));
            return false;
        }
        return true;
    }

    bool blitToEncoderFrame(ID3D11Texture2D* sourceTexture,
                            UINT sourceSubresource,
                            AVFrame& outFrame,
                            std::string* error) {
        auto* destTexture = reinterpret_cast<ID3D11Texture2D*>(outFrame.data[0]);
        if (sourceTexture == nullptr || destTexture == nullptr || outFrame.hw_frames_ctx == nullptr) {
            setError(error, "invalid D3D11 hardware camera frame textures");
            return false;
        }

        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceTexture->GetDesc(&sourceDesc);
        const UINT mipLevels = std::max<UINT>(1, sourceDesc.MipLevels);
        const UINT sourceMip = sourceSubresource % mipLevels;
        const UINT sourceArraySlice = sourceSubresource / mipLevels;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = sourceMip;
        inputDesc.Texture2D.ArraySlice = sourceArraySlice;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        outputDesc.Texture2DArray.MipSlice = 0;
        outputDesc.Texture2DArray.FirstArraySlice =
            static_cast<UINT>(reinterpret_cast<std::uintptr_t>(outFrame.data[1]));
        outputDesc.Texture2DArray.ArraySize = 1;

        ID3D11VideoProcessorInputView* inputView = nullptr;
        ID3D11VideoProcessorOutputView* outputView = nullptr;
        HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(sourceTexture,
                                                                  m_processorEnumerator,
                                                                  &inputDesc,
                                                                  &inputView);
        if (FAILED(hr) || inputView == nullptr) {
            setError(error, "CreateVideoProcessorInputView camera failed: " + hresultString(hr));
            return false;
        }
        hr = m_videoDevice->CreateVideoProcessorOutputView(destTexture,
                                                           m_processorEnumerator,
                                                           &outputDesc,
                                                           &outputView);
        if (FAILED(hr) || outputView == nullptr) {
            releaseComObject(inputView);
            setError(error, "CreateVideoProcessorOutputView camera failed: " + hresultString(hr));
            return false;
        }

        RECT sourceRect{0, 0, static_cast<LONG>(sourceDesc.Width), static_cast<LONG>(sourceDesc.Height)};
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
            setError(error, "VideoProcessorBlt camera frame failed: " + hresultString(hr));
            return false;
        }
        return true;
    }

    bool m_comInitialized{false};
    bool m_mfStarted{false};
    bool m_initialized{false};
    ID3D11Device* m_device{nullptr};
    ID3D11DeviceContext* m_context{nullptr};
    ID3D11VideoDevice* m_videoDevice{nullptr};
    ID3D11VideoContext* m_videoContext{nullptr};
    IMFDXGIDeviceManager* m_dxgiManager{nullptr};
    IMFMediaSource* m_source{nullptr};
    IMFSourceReader* m_reader{nullptr};
    ID3D11VideoProcessorEnumerator* m_processorEnumerator{nullptr};
    ID3D11VideoProcessor* m_videoProcessor{nullptr};
    int m_sourceWidth{0};
    int m_sourceHeight{0};
    int m_targetWidth{0};
    int m_targetHeight{0};
    int m_frameRate{30};
    GUID m_sourceSubtype{GUID_NULL};
    std::string m_backendName;
#else
    bool initialize(av::codec::VideoEncoder&,
                    const std::string&,
                    int,
                    int,
                    int,
                    std::string* error) {
        setError(error, "hardware camera capture unsupported on this platform");
        return false;
    }

    bool capture(av::codec::VideoEncoder&, int64_t, av::AVFramePtr&, std::string* error) {
        setError(error, "hardware camera capture unsupported on this platform");
        return false;
    }

    void shutdown() {}
    bool m_initialized{false};
#endif
};

WindowsD3D11CameraCapture::WindowsD3D11CameraCapture()
    : m_impl(std::make_unique<Impl>()) {}

WindowsD3D11CameraCapture::~WindowsD3D11CameraCapture() {
    shutdown();
}

bool WindowsD3D11CameraCapture::initialize(av::codec::VideoEncoder& encoder,
                                           const std::string& preferredDeviceName,
                                           int targetWidth,
                                           int targetHeight,
                                           int frameRate,
                                           std::string* error) {
    return m_impl->initialize(encoder, preferredDeviceName, targetWidth, targetHeight, frameRate, error);
}

bool WindowsD3D11CameraCapture::capture(av::codec::VideoEncoder& encoder,
                                        int64_t pts,
                                        av::AVFramePtr& outFrame,
                                        std::string* error) {
    return m_impl->capture(encoder, pts, outFrame, error);
}

void WindowsD3D11CameraCapture::shutdown() {
    if (m_impl) {
        m_impl->shutdown();
    }
}

bool WindowsD3D11CameraCapture::isInitialized() const {
#ifdef _WIN32
    return m_impl && m_impl->m_initialized;
#else
    return false;
#endif
}

std::string WindowsD3D11CameraCapture::backendName() const {
#ifdef _WIN32
    return m_impl ? m_impl->m_backendName : std::string{};
#else
    return {};
#endif
}

}  // namespace av::capture
