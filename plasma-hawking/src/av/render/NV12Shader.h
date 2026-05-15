#pragma once

#include <QOpenGLExtraFunctions>

namespace av::render {

class NV12Shader {
public:
    enum class InputFormat {
        Nv12 = 0,
        Yuv420p = 1,
        Rgba = 2,
    };

    NV12Shader() = default;
    ~NV12Shader();

    bool initialize(QOpenGLExtraFunctions* gl);
    void cleanup(QOpenGLExtraFunctions* gl);
    void bind(QOpenGLExtraFunctions* gl) const;
    void setInputFormat(QOpenGLExtraFunctions* gl, InputFormat format) const;
    void release(QOpenGLExtraFunctions* gl) const;
    GLuint program() const;

private:
    GLuint compileShader(QOpenGLExtraFunctions* gl, GLenum type, const char* source);

    GLuint m_program{0};
    GLint m_inputFormatLocation{-1};
};

}  // namespace av::render
