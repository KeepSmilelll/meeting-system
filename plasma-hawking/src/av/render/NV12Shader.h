#pragma once

#include <QOpenGLFunctions_4_5_Core>

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

    bool initialize(QOpenGLFunctions_4_5_Core* gl);
    void cleanup(QOpenGLFunctions_4_5_Core* gl);
    void bind(QOpenGLFunctions_4_5_Core* gl) const;
    void setInputFormat(QOpenGLFunctions_4_5_Core* gl, InputFormat format) const;
    void release(QOpenGLFunctions_4_5_Core* gl) const;
    GLuint program() const;

private:
    GLuint compileShader(QOpenGLFunctions_4_5_Core* gl, GLenum type, const char* source);

    GLuint m_program{0};
    GLint m_inputFormatLocation{-1};
};

}  // namespace av::render
