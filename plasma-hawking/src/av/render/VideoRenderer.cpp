#include "VideoRenderer.h"

#include "NV12Shader.h"

#include <QOpenGLFramebufferObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QSurfaceFormat>
#include <QDebug>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#endif

extern "C" {
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
}

namespace av::render {
namespace {

std::string describeAvError(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return buffer;
}

#ifdef _WIN32

#ifndef WGL_ACCESS_READ_ONLY_NV
#define WGL_ACCESS_READ_ONLY_NV 0x0000
#endif
#ifndef WGL_ACCESS_READ_WRITE_NV
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#endif

using WglDxOpenDeviceNvProc = HANDLE(WINAPI*)(void*);
using WglDxCloseDeviceNvProc = BOOL(WINAPI*)(HANDLE);
using WglDxRegisterObjectNvProc = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using WglDxUnregisterObjectNvProc = BOOL(WINAPI*)(HANDLE, HANDLE);
using WglDxLockObjectsNvProc = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
using WglDxUnlockObjectsNvProc = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);

template <typename T>
void releaseCom(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

bool resolveD3d11Context(const av::codec::DecodedVideoFrame& frame,
                         ID3D11Device*& deviceOut,
                         ID3D11DeviceContext*& deviceContextOut) {
    deviceOut = nullptr;
    deviceContextOut = nullptr;
    if (!frame.hardwareAvFrame || !frame.hardwareAvFrame->hw_frames_ctx) {
        return false;
    }

    AVHWFramesContext* framesContext =
        reinterpret_cast<AVHWFramesContext*>(frame.hardwareAvFrame->hw_frames_ctx->data);
    if (framesContext == nullptr ||
        framesContext->device_ref == nullptr ||
        framesContext->device_ref->data == nullptr) {
        return false;
    }

    AVHWDeviceContext* deviceContext =
        reinterpret_cast<AVHWDeviceContext*>(framesContext->device_ref->data);
    if (deviceContext->type != AV_HWDEVICE_TYPE_D3D11VA || deviceContext->hwctx == nullptr) {
        return false;
    }

    AVD3D11VADeviceContext* d3d11DeviceContext =
        reinterpret_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);
    if (d3d11DeviceContext->device == nullptr ||
        d3d11DeviceContext->device_context == nullptr) {
        return false;
    }

    d3d11DeviceContext->device->AddRef();
    d3d11DeviceContext->device_context->AddRef();
    deviceOut = d3d11DeviceContext->device;
    deviceContextOut = d3d11DeviceContext->device_context;
    return true;
}

#endif

bool canRetainFrameReference(const AVFrame& frame) {
    for (int index = 0; index < AV_NUM_DATA_POINTERS; ++index) {
        if (frame.buf[index] != nullptr) {
            return true;
        }
    }
    for (int index = 0; index < frame.nb_extended_buf; ++index) {
        if (frame.extended_buf != nullptr && frame.extended_buf[index] != nullptr) {
            return true;
        }
    }
    return false;
}

av::codec::DecodedVideoFrame::SharedAvFrame adoptOwnedFrame(av::AVFramePtr&& frame) {
    AVFrame* rawFrame = frame.release();
    if (rawFrame == nullptr) {
        return {};
    }
    return av::codec::DecodedVideoFrame::SharedAvFrame(
        rawFrame,
        [](AVFrame* ownedFrame) {
            av_frame_free(&ownedFrame);
        });
}

void clearHardwareShareFields(av::codec::DecodedVideoFrame& outFrame) {
    outFrame.hardwareAvFrame.reset();
    outFrame.hardwareFrameKind = av::codec::DecodedVideoFrame::HardwareFrameKind::None;
    outFrame.hardwareTextureHandle = nullptr;
    outFrame.hardwareSubresourceIndex = 0U;
}

void assignFrameMetadata(const AVFrame& frame, av::codec::DecodedVideoFrame& outFrame) {
    outFrame.width = frame.width;
    outFrame.height = frame.height;
    outFrame.pts = frame.best_effort_timestamp != AV_NOPTS_VALUE ? frame.best_effort_timestamp : frame.pts;
    outFrame.avFrame.reset();
    outFrame.pixelFormat = static_cast<AVPixelFormat>(frame.format);
    clearHardwareShareFields(outFrame);
}

bool moveNv12Frame(av::AVFramePtr&& frame, av::codec::DecodedVideoFrame& outFrame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }
    if (!canRetainFrameReference(*frame)) {
        return false;
    }

    assignFrameMetadata(*frame, outFrame);
    outFrame.yPlane.clear();
    outFrame.uvPlane.clear();
    outFrame.uPlane.clear();
    outFrame.vPlane.clear();
    outFrame.pixelFormat = AV_PIX_FMT_NV12;
    outFrame.avFrame = adoptOwnedFrame(std::move(frame));
    return static_cast<bool>(outFrame.avFrame);
}

bool moveYuv420pFrame(av::AVFramePtr&& frame, av::codec::DecodedVideoFrame& outFrame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }
    if (!canRetainFrameReference(*frame)) {
        return false;
    }

    assignFrameMetadata(*frame, outFrame);
    outFrame.yPlane.clear();
    outFrame.uvPlane.clear();
    outFrame.uPlane.clear();
    outFrame.vPlane.clear();
    outFrame.pixelFormat = static_cast<AVPixelFormat>(frame->format);
    outFrame.avFrame = adoptOwnedFrame(std::move(frame));
    return static_cast<bool>(outFrame.avFrame);
}

bool copyNv12Frame(const AVFrame& frame, av::codec::DecodedVideoFrame& outFrame) {
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }

    assignFrameMetadata(frame, outFrame);
    outFrame.avFrame.reset();
    outFrame.pixelFormat = AV_PIX_FMT_NV12;
    outFrame.yPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
    outFrame.uvPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height / 2));
    outFrame.uPlane.clear();
    outFrame.vPlane.clear();

    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* src = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        std::copy(src, src + frame.width, outFrame.yPlane.begin() + static_cast<std::ptrdiff_t>(y * frame.width));
    }

    for (int y = 0; y < frame.height / 2; ++y) {
        const uint8_t* src = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
        std::copy(src, src + frame.width, outFrame.uvPlane.begin() + static_cast<std::ptrdiff_t>(y * frame.width));
    }
    return true;
}

bool copyYuv420pFrame(const AVFrame& frame, av::codec::DecodedVideoFrame& outFrame) {
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }

    assignFrameMetadata(frame, outFrame);
    outFrame.avFrame.reset();
    outFrame.pixelFormat = static_cast<AVPixelFormat>(frame.format);
    outFrame.yPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
    outFrame.uvPlane.clear();
    outFrame.uPlane.resize(static_cast<std::size_t>(frame.width / 2) * static_cast<std::size_t>(frame.height / 2));
    outFrame.vPlane.resize(static_cast<std::size_t>(frame.width / 2) * static_cast<std::size_t>(frame.height / 2));

    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* src = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        std::copy(src, src + frame.width, outFrame.yPlane.begin() + static_cast<std::ptrdiff_t>(y * frame.width));
    }

    for (int y = 0; y < frame.height / 2; ++y) {
        const uint8_t* srcU = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
        const uint8_t* srcV = frame.data[2] + static_cast<std::ptrdiff_t>(y) * frame.linesize[2];
        std::copy(srcU,
                  srcU + (frame.width / 2),
                  outFrame.uPlane.begin() + static_cast<std::ptrdiff_t>(y * (frame.width / 2)));
        std::copy(srcV,
                  srcV + (frame.width / 2),
                  outFrame.vPlane.begin() + static_cast<std::ptrdiff_t>(y * (frame.width / 2)));
    }
    return true;
}

class VideoRendererBackend final : public QQuickFramebufferObject::Renderer, protected QOpenGLFunctions_4_5_Core {
public:
    VideoRendererBackend() = default;
    ~VideoRendererBackend() override {
#ifdef _WIN32
        releaseHardwareInterop();
#endif
        if (!m_initialized) {
            return;
        }
        m_shader.cleanup(this);
        glDeleteTextures(3, m_textures.data());
        glDeleteBuffers(static_cast<GLsizei>(m_pbos.size()), m_pbos.data());
        glDeleteBuffers(1, &m_vbo);
        glDeleteBuffers(1, &m_ebo);
        glDeleteVertexArrays(1, &m_vao);
    }

    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, format);
    }

    void synchronize(QQuickFramebufferObject* item) override {
        auto* rendererItem = qobject_cast<VideoRenderer*>(item);
        if (rendererItem == nullptr) {
            return;
        }

        if (VideoFrameStore* store = rendererItem->frameStore()) {
            uint64_t revision = 0;
            VideoFrameStore::FramePtr nextFrame = store->snapshotFrame(&revision);
            if (revision != m_revision) {
                if (nextFrame && !m_loggedFirstSynchronizedFrame) {
                    qInfo().noquote() << "[video-renderer] synchronized frame"
                                      << "revision=" << revision
                                      << "size=" << nextFrame->width << "x" << nextFrame->height;
                    m_loggedFirstSynchronizedFrame = true;
                }
                m_frame = std::move(nextFrame);
                m_revision = revision;
            }
        }
    }

    void render() override {
        if (!m_initialized) {
            initializeOpenGLFunctions();
            initializeGeometry();
            if (!m_loggedContextInfo) {
                if (const auto* context = QOpenGLContext::currentContext()) {
                    const QSurfaceFormat format = context->format();
                    qInfo().noquote() << "[video-renderer] context"
                                      << "major=" << format.majorVersion()
                                      << "minor=" << format.minorVersion()
                                      << "profile=" << static_cast<int>(format.profile())
                                      << "api=" << static_cast<int>(context->openGLModuleType());
                }
                m_loggedContextInfo = true;
            }
            m_initialized = m_shader.initialize(this);
            if (!m_initialized && !m_loggedInitFailure) {
                qWarning().noquote() << "[video-renderer] shader initialization failed";
                m_loggedInitFailure = true;
            }
        }

        glViewport(0, 0, framebufferObject()->width(), framebufferObject()->height());
        glClearColor(0.05F, 0.07F, 0.11F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (!m_initialized || !m_frame) {
            return;
        }

        logHardwareTextureShareCandidate(*m_frame);
        const bool usingHardwareInterop = tryPrepareHardwareInterop(*m_frame);
        const av::codec::DecodedVideoFrame* uploadFrame = nullptr;
        NV12Shader::InputFormat inputFormat = NV12Shader::InputFormat::Nv12;
        if (!usingHardwareInterop) {
#ifdef _WIN32
            releaseHardwareInteropTextures();
#endif
            if (!prepareFrameForUpload(*m_frame, uploadFrame) || uploadFrame == nullptr) {
                return;
            }
            uploadFrameTextures(*uploadFrame);
            inputFormat =
                (uploadFrame->hasAvFrameYuv420pPlanes() || uploadFrame->hasCpuYuv420pPlanes())
                    ? NV12Shader::InputFormat::Yuv420p
                    : NV12Shader::InputFormat::Nv12;
        } else {
            inputFormat = NV12Shader::InputFormat::Rgba;
        }

        m_shader.bind(this);
        m_shader.setInputFormat(this, inputFormat);
        glBindVertexArray(m_vao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_textures[0]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D,
                      inputFormat == NV12Shader::InputFormat::Rgba ? 0 : m_textures[1]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D,
                      inputFormat == NV12Shader::InputFormat::Yuv420p ? m_textures[2] : 0);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
        m_shader.release(this);
#ifdef _WIN32
        if (usingHardwareInterop) {
            unlockHardwareInteropObjects();
        }
#endif
        if (!m_loggedFirstDraw) {
            qInfo().noquote() << "[video-renderer] drew first frame"
                              << "size=" << m_frame->width << "x" << m_frame->height
                              << "revision=" << m_revision;
            m_loggedFirstDraw = true;
        }
    }

private:
#ifdef _WIN32
    bool resolveDxInteropFunctions() {
        if (m_wglDxOpenDeviceNv != nullptr &&
            m_wglDxCloseDeviceNv != nullptr &&
            m_wglDxRegisterObjectNv != nullptr &&
            m_wglDxUnregisterObjectNv != nullptr &&
            m_wglDxLockObjectsNv != nullptr &&
            m_wglDxUnlockObjectsNv != nullptr) {
            return true;
        }

        const auto loadWglProc = [](const char* name) -> PROC {
            PROC raw = wglGetProcAddress(name);
            if (raw == nullptr ||
                raw == reinterpret_cast<PROC>(1) ||
                raw == reinterpret_cast<PROC>(2) ||
                raw == reinterpret_cast<PROC>(3) ||
                raw == reinterpret_cast<PROC>(-1)) {
                return nullptr;
            }
            return raw;
        };

        m_wglDxOpenDeviceNv = reinterpret_cast<WglDxOpenDeviceNvProc>(loadWglProc("wglDXOpenDeviceNV"));
        m_wglDxCloseDeviceNv = reinterpret_cast<WglDxCloseDeviceNvProc>(loadWglProc("wglDXCloseDeviceNV"));
        m_wglDxRegisterObjectNv =
            reinterpret_cast<WglDxRegisterObjectNvProc>(loadWglProc("wglDXRegisterObjectNV"));
        m_wglDxUnregisterObjectNv =
            reinterpret_cast<WglDxUnregisterObjectNvProc>(loadWglProc("wglDXUnregisterObjectNV"));
        m_wglDxLockObjectsNv =
            reinterpret_cast<WglDxLockObjectsNvProc>(loadWglProc("wglDXLockObjectsNV"));
        m_wglDxUnlockObjectsNv =
            reinterpret_cast<WglDxUnlockObjectsNvProc>(loadWglProc("wglDXUnlockObjectsNV"));

        return m_wglDxOpenDeviceNv != nullptr &&
               m_wglDxCloseDeviceNv != nullptr &&
               m_wglDxRegisterObjectNv != nullptr &&
               m_wglDxUnregisterObjectNv != nullptr &&
               m_wglDxLockObjectsNv != nullptr &&
               m_wglDxUnlockObjectsNv != nullptr;
    }

    void logHardwareInteropFailure(const QString& reason) {
        if (m_loggedHardwareInteropFailure) {
            return;
        }
        qWarning().noquote() << "[video-renderer] hardware interop fallback:" << reason;
        m_loggedHardwareInteropFailure = true;
    }

    void releaseHardwareInteropTextures() {
        unlockHardwareInteropObjects();

        if (m_dxInteropObject != nullptr &&
            m_dxInteropDeviceHandle != nullptr &&
            m_wglDxUnregisterObjectNv != nullptr) {
            m_wglDxUnregisterObjectNv(m_dxInteropDeviceHandle, m_dxInteropObject);
        }
        m_dxInteropObject = nullptr;

        releaseCom(m_dxVideoOutputView);
        releaseCom(m_dxInteropRgbaTexture);
        releaseCom(m_dxVideoProcessor);
        releaseCom(m_dxVideoProcessorEnumerator);
        m_dxInteropWidth = 0;
        m_dxInteropHeight = 0;
    }

    void releaseHardwareInterop() {
        releaseHardwareInteropTextures();

        if (m_dxInteropDeviceHandle != nullptr &&
            m_wglDxCloseDeviceNv != nullptr) {
            m_wglDxCloseDeviceNv(m_dxInteropDeviceHandle);
        }
        m_dxInteropDeviceHandle = nullptr;

        releaseCom(m_dxVideoContext);
        releaseCom(m_dxVideoDevice);
        releaseCom(m_dxContext);
        releaseCom(m_dxDevice);
    }

    bool ensureHardwareInteropContext(const av::codec::DecodedVideoFrame& frame) {
        ID3D11Device* nextDevice = nullptr;
        ID3D11DeviceContext* nextContext = nullptr;
        if (!resolveD3d11Context(frame, nextDevice, nextContext)) {
            return false;
        }

        if (nextDevice == m_dxDevice && nextContext == m_dxContext) {
            releaseCom(nextContext);
            releaseCom(nextDevice);
            return true;
        }

        releaseHardwareInterop();
        m_dxDevice = nextDevice;
        m_dxContext = nextContext;

        if (FAILED(m_dxDevice->QueryInterface(
                __uuidof(ID3D11VideoDevice),
                reinterpret_cast<void**>(&m_dxVideoDevice)))) {
            logHardwareInteropFailure(QStringLiteral("ID3D11VideoDevice unavailable"));
            releaseHardwareInterop();
            return false;
        }

        if (FAILED(m_dxContext->QueryInterface(
                __uuidof(ID3D11VideoContext),
                reinterpret_cast<void**>(&m_dxVideoContext)))) {
            logHardwareInteropFailure(QStringLiteral("ID3D11VideoContext unavailable"));
            releaseHardwareInterop();
            return false;
        }

        m_dxInteropDeviceHandle = m_wglDxOpenDeviceNv(m_dxDevice);
        if (m_dxInteropDeviceHandle == nullptr) {
            logHardwareInteropFailure(QStringLiteral("wglDXOpenDeviceNV failed"));
            releaseHardwareInterop();
            return false;
        }
        return true;
    }

    bool ensureHardwareInteropTextures(int width, int height) {
        if (width <= 0 || height <= 0) {
            return false;
        }
        if (m_dxInteropObject != nullptr &&
            m_dxInteropRgbaTexture != nullptr &&
            m_dxVideoOutputView != nullptr &&
            m_dxVideoProcessor != nullptr &&
            m_dxVideoProcessorEnumerator != nullptr &&
            m_dxInteropWidth == width &&
            m_dxInteropHeight == height) {
            return true;
        }

        releaseHardwareInteropTextures();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = static_cast<UINT>(width);
        contentDesc.InputHeight = static_cast<UINT>(height);
        contentDesc.OutputWidth = static_cast<UINT>(width);
        contentDesc.OutputHeight = static_cast<UINT>(height);
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(m_dxVideoDevice->CreateVideoProcessorEnumerator(
                &contentDesc,
                &m_dxVideoProcessorEnumerator))) {
            logHardwareInteropFailure(QStringLiteral("CreateVideoProcessorEnumerator failed"));
            releaseHardwareInteropTextures();
            return false;
        }

        if (FAILED(m_dxVideoDevice->CreateVideoProcessor(
                m_dxVideoProcessorEnumerator,
                0,
                &m_dxVideoProcessor))) {
            logHardwareInteropFailure(QStringLiteral("CreateVideoProcessor failed"));
            releaseHardwareInteropTextures();
            return false;
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = static_cast<UINT>(width);
        textureDesc.Height = static_cast<UINT>(height);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (FAILED(m_dxDevice->CreateTexture2D(&textureDesc, nullptr, &m_dxInteropRgbaTexture))) {
            logHardwareInteropFailure(QStringLiteral("CreateTexture2D(RGBA shared) failed"));
            releaseHardwareInteropTextures();
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
        outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputViewDesc.Texture2D.MipSlice = 0;
        if (FAILED(m_dxVideoDevice->CreateVideoProcessorOutputView(
                m_dxInteropRgbaTexture,
                m_dxVideoProcessorEnumerator,
                &outputViewDesc,
                &m_dxVideoOutputView))) {
            logHardwareInteropFailure(QStringLiteral("CreateVideoProcessorOutputView failed"));
            releaseHardwareInteropTextures();
            return false;
        }

        glBindTexture(GL_TEXTURE_2D, m_textures[0]);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA8,
                     width,
                     height,
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     nullptr);

        m_dxInteropObject = m_wglDxRegisterObjectNv(
            m_dxInteropDeviceHandle,
            m_dxInteropRgbaTexture,
            m_textures[0],
            GL_TEXTURE_2D,
            WGL_ACCESS_READ_ONLY_NV);
        if (m_dxInteropObject == nullptr) {
            logHardwareInteropFailure(QStringLiteral("wglDXRegisterObjectNV failed"));
            releaseHardwareInteropTextures();
            return false;
        }

        m_dxInteropWidth = width;
        m_dxInteropHeight = height;
        return true;
    }

    bool blitHardwareFrameToInteropTexture(const av::codec::DecodedVideoFrame& frame) {
        if (frame.hardwareTextureHandle == nullptr ||
            m_dxVideoDevice == nullptr ||
            m_dxVideoContext == nullptr ||
            m_dxVideoProcessor == nullptr ||
            m_dxVideoProcessorEnumerator == nullptr ||
            m_dxVideoOutputView == nullptr) {
            return false;
        }

        auto* sourceTexture = static_cast<ID3D11Texture2D*>(frame.hardwareTextureHandle);
        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceTexture->GetDesc(&sourceDesc);
        if (sourceDesc.Format != DXGI_FORMAT_NV12) {
            logHardwareInteropFailure(QStringLiteral("unsupported D3D11 decode format for interop"));
            return false;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
        inputViewDesc.FourCC = 0;
        inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDesc.Texture2D.MipSlice = 0;
        inputViewDesc.Texture2D.ArraySlice = frame.hardwareSubresourceIndex;

        ID3D11VideoProcessorInputView* inputView = nullptr;
        if (FAILED(m_dxVideoDevice->CreateVideoProcessorInputView(
                sourceTexture,
                m_dxVideoProcessorEnumerator,
                &inputViewDesc,
                &inputView)) ||
            inputView == nullptr) {
            logHardwareInteropFailure(QStringLiteral("CreateVideoProcessorInputView failed"));
            return false;
        }

        RECT sourceRect{0, 0, frame.width, frame.height};
        RECT targetRect{0, 0, frame.width, frame.height};
        m_dxVideoContext->VideoProcessorSetStreamFrameFormat(
            m_dxVideoProcessor,
            0,
            D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_dxVideoContext->VideoProcessorSetStreamSourceRect(
            m_dxVideoProcessor,
            0,
            TRUE,
            &sourceRect);
        m_dxVideoContext->VideoProcessorSetStreamDestRect(
            m_dxVideoProcessor,
            0,
            TRUE,
            &targetRect);
        m_dxVideoContext->VideoProcessorSetOutputTargetRect(
            m_dxVideoProcessor,
            TRUE,
            &targetRect);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.pInputSurface = inputView;
        const HRESULT blitResult = m_dxVideoContext->VideoProcessorBlt(
            m_dxVideoProcessor,
            m_dxVideoOutputView,
            0,
            1,
            &stream);
        releaseCom(inputView);
        if (FAILED(blitResult)) {
            logHardwareInteropFailure(QStringLiteral("VideoProcessorBlt failed"));
            return false;
        }
        m_dxContext->Flush();
        return true;
    }

    bool lockHardwareInteropObjects() {
        if (m_dxInteropDeviceHandle == nullptr || m_dxInteropObject == nullptr) {
            return false;
        }
        HANDLE object = m_dxInteropObject;
        if (!m_wglDxLockObjectsNv(m_dxInteropDeviceHandle, 1, &object)) {
            return false;
        }
        m_dxInteropLocked = true;
        return true;
    }

    void unlockHardwareInteropObjects() {
        if (!m_dxInteropLocked ||
            m_dxInteropDeviceHandle == nullptr ||
            m_dxInteropObject == nullptr ||
            m_wglDxUnlockObjectsNv == nullptr) {
            return;
        }
        HANDLE object = m_dxInteropObject;
        m_wglDxUnlockObjectsNv(m_dxInteropDeviceHandle, 1, &object);
        m_dxInteropLocked = false;
    }

    bool tryPrepareHardwareInterop(const av::codec::DecodedVideoFrame& frame) {
        if (!frame.hasHardwareTextureShareCandidate() ||
            frame.hardwareFrameKind != av::codec::DecodedVideoFrame::HardwareFrameKind::D3d11Texture2D) {
            return false;
        }
        if (!resolveDxInteropFunctions()) {
            logHardwareInteropFailure(QStringLiteral("WGL_NV_DX_interop unavailable"));
            return false;
        }
        if (!ensureHardwareInteropContext(frame)) {
            return false;
        }
        if (!ensureHardwareInteropTextures(frame.width, frame.height)) {
            return false;
        }
        if (!blitHardwareFrameToInteropTexture(frame)) {
            releaseHardwareInteropTextures();
            return false;
        }
        if (!lockHardwareInteropObjects()) {
            logHardwareInteropFailure(QStringLiteral("wglDXLockObjectsNV failed"));
            releaseHardwareInteropTextures();
            return false;
        }

        if (!m_loggedHardwareInteropActivated) {
            qInfo().noquote() << "[video-renderer] hardware texture interop active"
                              << "size=" << frame.width << "x" << frame.height;
            m_loggedHardwareInteropActivated = true;
        }
        return true;
    }
#else
    bool tryPrepareHardwareInterop(const av::codec::DecodedVideoFrame&) {
        return false;
    }
#endif

    void logHardwareTextureShareCandidate(const av::codec::DecodedVideoFrame& frame) {
        if (m_loggedHardwareShareCandidate || !frame.hasHardwareTextureShareCandidate()) {
            return;
        }

        const char* frameKind = "unknown";
        if (frame.hardwareFrameKind ==
            av::codec::DecodedVideoFrame::HardwareFrameKind::D3d11Texture2D) {
            frameKind = "d3d11-texture2d";
        }

        qInfo().noquote() << "[video-renderer] detected hardware texture share candidate"
                          << "kind=" << frameKind
                          << "size=" << frame.width << "x" << frame.height
                          << "subresource=" << frame.hardwareSubresourceIndex;
        m_loggedHardwareShareCandidate = true;
    }

    bool prepareFrameForUpload(const av::codec::DecodedVideoFrame& source,
                               const av::codec::DecodedVideoFrame*& uploadFrame) {
        uploadFrame = nullptr;
        if (source.hasAvFramePlanes() || source.hasCpuPlanes()) {
            uploadFrame = &source;
            return true;
        }
        if (!source.hasHardwareTextureShareCandidate()) {
            return false;
        }
        if (!promoteHardwareFrameForUpload(source)) {
            return false;
        }
        uploadFrame = &m_hwTransferFrame;
        return true;
    }

    bool promoteHardwareFrameForUpload(const av::codec::DecodedVideoFrame& source) {
        if (!source.hardwareAvFrame) {
            return false;
        }

        av::AVFramePtr transferredFrame = av::makeFrame();
        if (!transferredFrame) {
            if (!m_loggedHardwareTransferFailure) {
                qWarning().noquote() << "[video-renderer] hardware fallback transfer frame alloc failed";
                m_loggedHardwareTransferFailure = true;
            }
            return false;
        }

        const int transferResult = av_hwframe_transfer_data(
            transferredFrame.get(),
            source.hardwareAvFrame.get(),
            0);
        if (transferResult < 0) {
            if (!m_loggedHardwareTransferFailure) {
                qWarning().noquote() << "[video-renderer] hardware fallback transfer failed"
                                     << "error=" << QString::fromStdString(describeAvError(transferResult));
                m_loggedHardwareTransferFailure = true;
            }
            return false;
        }

        (void)av_frame_copy_props(transferredFrame.get(), source.hardwareAvFrame.get());
        m_hwTransferFrame = av::codec::DecodedVideoFrame{};

        const AVPixelFormat transferredFormat = static_cast<AVPixelFormat>(transferredFrame->format);
        bool promoted = false;
        if (transferredFormat == AV_PIX_FMT_NV12) {
            promoted = moveNv12Frame(std::move(transferredFrame), m_hwTransferFrame);
            if (!promoted) {
                promoted = copyNv12Frame(*transferredFrame, m_hwTransferFrame);
            }
        } else if (transferredFormat == AV_PIX_FMT_YUV420P || transferredFormat == AV_PIX_FMT_YUVJ420P) {
            promoted = moveYuv420pFrame(std::move(transferredFrame), m_hwTransferFrame);
            if (!promoted) {
                promoted = copyYuv420pFrame(*transferredFrame, m_hwTransferFrame);
            }
        }

        if (promoted && !m_loggedHardwareTransferFallback) {
            qInfo().noquote() << "[video-renderer] hardware frame fallback transfer engaged"
                              << "size=" << m_hwTransferFrame.width << "x" << m_hwTransferFrame.height;
            m_loggedHardwareTransferFallback = true;
        }
        return promoted;
    }

    void initializeGeometry() {
        static constexpr std::array<float, 16> kVertices = {
            -1.0F, -1.0F, 0.0F, 0.0F,
             1.0F, -1.0F, 1.0F, 0.0F,
             1.0F,  1.0F, 1.0F, 1.0F,
            -1.0F,  1.0F, 0.0F, 1.0F,
        };
        static constexpr std::array<unsigned int, 6> kIndices = {0, 1, 2, 0, 2, 3};

        glGenVertexArrays(1, &m_vao);
        glGenBuffers(1, &m_vbo);
        glGenBuffers(1, &m_ebo);
        glBindVertexArray(m_vao);

        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(kVertices)), kVertices.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(kIndices)), kIndices.data(), GL_STATIC_DRAW);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glGenTextures(3, m_textures.data());
        for (GLuint texture : m_textures) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glGenBuffers(static_cast<GLsizei>(m_pbos.size()), m_pbos.data());

        glBindVertexArray(0);
    }

    void uploadFrameTextures(const av::codec::DecodedVideoFrame& frame) {
        const bool needsResize = m_textureWidth != frame.width || m_textureHeight != frame.height;
        const int pboSlot = static_cast<int>(m_pboUploadIndex++ % 2U);
        const GLuint yPbo = m_pbos[static_cast<std::size_t>(pboSlot * 2)];
        const GLuint uvPbo = m_pbos[static_cast<std::size_t>(pboSlot * 2 + 1)];

        const auto uploadPlaneWithPbo = [&](GLuint texture,
                                            GLuint pbo,
                                            GLenum internalFormat,
                                            int width,
                                            int height,
                                            GLenum format,
                                            const std::vector<uint8_t>& bytes) {
            if (width <= 0 || height <= 0 || bytes.empty()) {
                return;
            }

            glBindTexture(GL_TEXTURE_2D, texture);
            if (needsResize) {
                glTexImage2D(
                    GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);
            }

            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_UNPACK_BUFFER,
                         static_cast<GLsizeiptr>(bytes.size()),
                         nullptr,
                         GL_STREAM_DRAW);
            void* mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER,
                                            0,
                                            static_cast<GLsizeiptr>(bytes.size()),
                                            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (mapped != nullptr) {
                std::memcpy(mapped, bytes.data(), bytes.size());
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                glTexSubImage2D(
                    GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, nullptr);
            } else {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                glTexSubImage2D(
                    GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, bytes.data());
                return;
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        };

        const auto uploadPlaneDirect = [&](GLuint texture,
                                           GLenum internalFormat,
                                           int width,
                                           int height,
                                           GLenum format,
                                           const uint8_t* data,
                                           int strideBytes,
                                           int bytesPerPixel) {
            if (width <= 0 || height <= 0 || data == nullptr || strideBytes <= 0 || bytesPerPixel <= 0) {
                return;
            }

            const int minStrideBytes = width * bytesPerPixel;
            if (strideBytes < minStrideBytes) {
                return;
            }

            glBindTexture(GL_TEXTURE_2D, texture);
            if (needsResize) {
                glTexImage2D(
                    GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);
            }

            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, strideBytes / bytesPerPixel);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, data);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        };

        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (frame.hasAvFramePlanes()) {
            uploadPlaneDirect(m_textures[0],
                              GL_R8,
                              frame.width,
                              frame.height,
                              GL_RED,
                              frame.yData(),
                              frame.yStride(),
                              1);
        } else {
            uploadPlaneWithPbo(m_textures[0],
                               yPbo,
                               GL_R8,
                               frame.width,
                               frame.height,
                               GL_RED,
                               frame.yPlane);
        }

        if (frame.hasAvFrameYuv420pPlanes() || frame.hasCpuYuv420pPlanes()) {
            glActiveTexture(GL_TEXTURE1);
            if (frame.hasAvFrameYuv420pPlanes()) {
                uploadPlaneDirect(m_textures[1],
                                  GL_R8,
                                  frame.width / 2,
                                  frame.height / 2,
                                  GL_RED,
                                  frame.uData(),
                                  frame.uStride(),
                                  1);
            } else {
                uploadPlaneWithPbo(m_textures[1],
                                   uvPbo,
                                   GL_R8,
                                   frame.width / 2,
                                   frame.height / 2,
                                   GL_RED,
                                   frame.uPlane);
            }

            glActiveTexture(GL_TEXTURE2);
            if (frame.hasAvFrameYuv420pPlanes()) {
                uploadPlaneDirect(m_textures[2],
                                  GL_R8,
                                  frame.width / 2,
                                  frame.height / 2,
                                  GL_RED,
                                  frame.vData(),
                                  frame.vStride(),
                                  1);
            } else {
                uploadPlaneWithPbo(m_textures[2],
                                   uvPbo,
                                   GL_R8,
                                   frame.width / 2,
                                   frame.height / 2,
                                   GL_RED,
                                   frame.vPlane);
            }
        } else {
            glActiveTexture(GL_TEXTURE1);
            if (frame.hasAvFrameNv12Planes()) {
                uploadPlaneDirect(m_textures[1],
                                  GL_RG8,
                                  frame.width / 2,
                                  frame.height / 2,
                                  GL_RG,
                                  frame.uvData(),
                                  frame.uvStride(),
                                  2);
            } else {
                uploadPlaneWithPbo(m_textures[1],
                                   uvPbo,
                                   GL_RG8,
                                   frame.width / 2,
                                   frame.height / 2,
                                   GL_RG,
                                   frame.uvPlane);
            }
        }

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (needsResize) {
            m_textureWidth = frame.width;
            m_textureHeight = frame.height;
        }
    }

    bool m_initialized{false};
    bool m_loggedContextInfo{false};
    bool m_loggedInitFailure{false};
    bool m_loggedFirstSynchronizedFrame{false};
    bool m_loggedFirstDraw{false};
    bool m_loggedHardwareShareCandidate{false};
    bool m_loggedHardwareTransferFallback{false};
    bool m_loggedHardwareTransferFailure{false};
#ifdef _WIN32
    bool m_loggedHardwareInteropActivated{false};
    bool m_loggedHardwareInteropFailure{false};
    bool m_dxInteropLocked{false};
    WglDxOpenDeviceNvProc m_wglDxOpenDeviceNv{nullptr};
    WglDxCloseDeviceNvProc m_wglDxCloseDeviceNv{nullptr};
    WglDxRegisterObjectNvProc m_wglDxRegisterObjectNv{nullptr};
    WglDxUnregisterObjectNvProc m_wglDxUnregisterObjectNv{nullptr};
    WglDxLockObjectsNvProc m_wglDxLockObjectsNv{nullptr};
    WglDxUnlockObjectsNvProc m_wglDxUnlockObjectsNv{nullptr};
    HANDLE m_dxInteropDeviceHandle{nullptr};
    HANDLE m_dxInteropObject{nullptr};
    ID3D11Device* m_dxDevice{nullptr};
    ID3D11DeviceContext* m_dxContext{nullptr};
    ID3D11VideoDevice* m_dxVideoDevice{nullptr};
    ID3D11VideoContext* m_dxVideoContext{nullptr};
    ID3D11VideoProcessorEnumerator* m_dxVideoProcessorEnumerator{nullptr};
    ID3D11VideoProcessor* m_dxVideoProcessor{nullptr};
    ID3D11Texture2D* m_dxInteropRgbaTexture{nullptr};
    ID3D11VideoProcessorOutputView* m_dxVideoOutputView{nullptr};
    int m_dxInteropWidth{0};
    int m_dxInteropHeight{0};
#endif
    uint64_t m_revision{0};
    VideoFrameStore::FramePtr m_frame;
    av::codec::DecodedVideoFrame m_hwTransferFrame;
    NV12Shader m_shader;
    GLuint m_vao{0};
    GLuint m_vbo{0};
    GLuint m_ebo{0};
    std::array<GLuint, 3> m_textures{0, 0, 0};
    std::array<GLuint, 4> m_pbos{0, 0, 0, 0};
    uint64_t m_pboUploadIndex{0U};
    int m_textureWidth{0};
    int m_textureHeight{0};
};

}  // namespace

VideoRenderer::VideoRenderer(QQuickItem* parent)
    : QQuickFramebufferObject(parent) {
    qInfo().noquote() << "[video-renderer] item constructed";
}

QQuickFramebufferObject::Renderer* VideoRenderer::createRenderer() const {
    qInfo().noquote() << "[video-renderer] backend created";
    return new VideoRendererBackend();
}

QObject* VideoRenderer::frameSource() const {
    return m_frameSource;
}

void VideoRenderer::setFrameSource(QObject* source) {
    if (m_frameSource == source) {
        return;
    }

    if (m_frameChangedConnection) {
        disconnect(m_frameChangedConnection);
        m_frameChangedConnection = {};
    }

    m_frameSource = source;
    if (!m_loggedFrameSourceChange) {
        qInfo().noquote() << "[video-renderer] frame source changed"
                          << "source=" << (m_frameSource != nullptr ? m_frameSource->metaObject()->className() : "null")
                          << "is_store=" << (qobject_cast<VideoFrameStore*>(m_frameSource) != nullptr);
        m_loggedFrameSourceChange = true;
    }
    if (auto* nextStore = qobject_cast<VideoFrameStore*>(m_frameSource)) {
        if (!m_loggedFrameSourceAssigned) {
            uint64_t revision = 0;
            const VideoFrameStore::FramePtr frame = nextStore->snapshotFrame(&revision);
            qInfo().noquote() << "[video-renderer] frame source assigned"
                              << "revision=" << revision
                              << "has_frame=" << (frame != nullptr);
            m_loggedFrameSourceAssigned = true;
        }
        m_frameChangedConnection =
            connect(nextStore, &VideoFrameStore::frameChanged, this, [this, nextStore]() {
                if (!m_loggedFrameChangeObserved) {
                    uint64_t revision = 0;
                    const VideoFrameStore::FramePtr frame = nextStore->snapshotFrame(&revision);
                    qInfo().noquote() << "[video-renderer] frame change observed"
                                      << "revision=" << revision
                                      << "has_frame=" << (frame != nullptr);
                    m_loggedFrameChangeObserved = true;
                }
                update();
            }, Qt::QueuedConnection);
    }
    emit frameSourceChanged();
    update();
}

VideoFrameStore* VideoRenderer::frameStore() const {
    return qobject_cast<VideoFrameStore*>(m_frameSource);
}

}  // namespace av::render
