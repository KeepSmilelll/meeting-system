#include "VideoRenderer.h"

#include "NV12Shader.h"

#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_4_5_Core>

#include <array>
#include <utility>

namespace av::render {
namespace {

class VideoRendererBackend final : public QQuickFramebufferObject::Renderer, protected QOpenGLFunctions_4_5_Core {
public:
    VideoRendererBackend() = default;
    ~VideoRendererBackend() override {
        if (!m_initialized) {
            return;
        }
        glDeleteTextures(2, m_textures.data());
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
            av::codec::DecodedVideoFrame nextFrame;
            if (store->snapshot(nextFrame, &revision) && revision != m_revision) {
                m_frame = std::move(nextFrame);
                m_revision = revision;
            }
        }
    }

    void render() override {
        if (!m_initialized) {
            initializeOpenGLFunctions();
            initializeGeometry();
            m_initialized = m_shader.initialize(this);
        }

        glViewport(0, 0, framebufferObject()->width(), framebufferObject()->height());
        glClearColor(0.05F, 0.07F, 0.11F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (!m_initialized || m_frame.width <= 0 || m_frame.height <= 0 ||
            m_frame.yPlane.empty() || m_frame.uvPlane.empty()) {
            return;
        }

        uploadFrameTextures();

        m_shader.bind(this);
        glBindVertexArray(m_vao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_textures[0]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_textures[1]);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
        m_shader.release(this);
    }

private:
    void initializeGeometry() {
        static constexpr std::array<float, 16> kVertices = {
            -1.0F, -1.0F, 0.0F, 1.0F,
             1.0F, -1.0F, 1.0F, 1.0F,
             1.0F,  1.0F, 1.0F, 0.0F,
            -1.0F,  1.0F, 0.0F, 0.0F,
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

        glGenTextures(2, m_textures.data());
        for (GLuint texture : m_textures) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        glBindVertexArray(0);
    }

    void uploadFrameTextures() {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_textures[0]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_R8,
                     m_frame.width,
                     m_frame.height,
                     0,
                     GL_RED,
                     GL_UNSIGNED_BYTE,
                     m_frame.yPlane.data());

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_textures[1]);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RG8,
                     m_frame.width / 2,
                     m_frame.height / 2,
                     0,
                     GL_RG,
                     GL_UNSIGNED_BYTE,
                     m_frame.uvPlane.data());
    }

    bool m_initialized{false};
    uint64_t m_revision{0};
    av::codec::DecodedVideoFrame m_frame;
    NV12Shader m_shader;
    GLuint m_vao{0};
    GLuint m_vbo{0};
    GLuint m_ebo{0};
    std::array<GLuint, 2> m_textures{0, 0};
};

}  // namespace

VideoRenderer::VideoRenderer(QQuickItem* parent)
    : QQuickFramebufferObject(parent) {}

QQuickFramebufferObject::Renderer* VideoRenderer::createRenderer() const {
    return new VideoRendererBackend();
}

QObject* VideoRenderer::frameSource() const {
    return m_frameSource;
}

void VideoRenderer::setFrameSource(QObject* source) {
    if (m_frameSource == source) {
        return;
    }

    if (auto* currentStore = qobject_cast<VideoFrameStore*>(m_frameSource)) {
        disconnect(currentStore, &VideoFrameStore::frameChanged, this, &VideoRenderer::update);
    }

    m_frameSource = source;
    if (auto* nextStore = qobject_cast<VideoFrameStore*>(m_frameSource)) {
        connect(nextStore, &VideoFrameStore::frameChanged, this, &VideoRenderer::update, Qt::QueuedConnection);
    }
    emit frameSourceChanged();
    update();
}

VideoFrameStore* VideoRenderer::frameStore() const {
    return qobject_cast<VideoFrameStore*>(m_frameSource);
}

}  // namespace av::render
