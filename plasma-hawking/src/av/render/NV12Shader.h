#pragma once

#include <QOpenGLFunctions_4_5_Core>

namespace av::render {

class NV12Shader {
public:
    NV12Shader() = default;
    ~NV12Shader();

    bool initialize(QOpenGLFunctions_4_5_Core* gl);
    void bind(QOpenGLFunctions_4_5_Core* gl) const;
    void release(QOpenGLFunctions_4_5_Core* gl) const;
    GLuint program() const;

private:
    GLuint compileShader(QOpenGLFunctions_4_5_Core* gl, GLenum type, const char* source);

    GLuint m_program{0};
};

}  // namespace av::render
