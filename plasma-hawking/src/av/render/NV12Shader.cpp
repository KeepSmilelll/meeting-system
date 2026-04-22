#include "NV12Shader.h"

#include <QDebug>

namespace av::render {
namespace {

constexpr const char* kVertexShaderSource = R"(
#version 460 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

constexpr const char* kFragmentShaderSource = R"(
#version 460 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D texY;
uniform sampler2D texUV;
uniform sampler2D texU;
uniform sampler2D texV;
uniform int u_inputFormat;
void main() {
    if (u_inputFormat == 2) {
        fragColor = texture(texY, vTexCoord);
        return;
    }

    float y = texture(texY, vTexCoord).r;
    float u = 0.0;
    float v = 0.0;
    if (u_inputFormat == 1) {
        u = texture(texU, vTexCoord).r - 0.5;
        v = texture(texV, vTexCoord).r - 0.5;
    } else {
        vec2 uv = texture(texUV, vTexCoord).rg - vec2(0.5, 0.5);
        u = uv.x;
        v = uv.y;
    }
    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;
    fragColor = vec4(r, g, b, 1.0);
}
)";

}  // namespace

NV12Shader::~NV12Shader() = default;

bool NV12Shader::initialize(QOpenGLFunctions_4_5_Core* gl) {
    if (gl == nullptr) {
        return false;
    }
    if (m_program != 0) {
        return true;
    }

    const GLuint vertexShader = compileShader(gl, GL_VERTEX_SHADER, kVertexShaderSource);
    const GLuint fragmentShader = compileShader(gl, GL_FRAGMENT_SHADER, kFragmentShaderSource);
    if (vertexShader == 0 || fragmentShader == 0) {
        if (vertexShader != 0) {
            gl->glDeleteShader(vertexShader);
        }
        if (fragmentShader != 0) {
            gl->glDeleteShader(fragmentShader);
        }
        return false;
    }

    m_program = gl->glCreateProgram();
    gl->glAttachShader(m_program, vertexShader);
    gl->glAttachShader(m_program, fragmentShader);
    gl->glLinkProgram(m_program);

    GLint linked = GL_FALSE;
    gl->glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    gl->glDeleteShader(vertexShader);
    gl->glDeleteShader(fragmentShader);
    if (linked != GL_TRUE) {
        char infoLog[1024]{};
        gl->glGetProgramInfoLog(m_program, sizeof(infoLog), nullptr, infoLog);
        qWarning().noquote() << "[nv12-shader] link failed:" << infoLog;
        gl->glDeleteProgram(m_program);
        m_program = 0;
        return false;
    }

    gl->glUseProgram(m_program);
    gl->glUniform1i(gl->glGetUniformLocation(m_program, "texY"), 0);
    gl->glUniform1i(gl->glGetUniformLocation(m_program, "texUV"), 1);
    gl->glUniform1i(gl->glGetUniformLocation(m_program, "texU"), 1);
    gl->glUniform1i(gl->glGetUniformLocation(m_program, "texV"), 2);
    m_inputFormatLocation = gl->glGetUniformLocation(m_program, "u_inputFormat");
    if (m_inputFormatLocation >= 0) {
        gl->glUniform1i(m_inputFormatLocation, static_cast<GLint>(InputFormat::Nv12));
    }
    gl->glUseProgram(0);
    return true;
}

void NV12Shader::cleanup(QOpenGLFunctions_4_5_Core* gl) {
    if (gl != nullptr && m_program != 0) {
        gl->glDeleteProgram(m_program);
        m_program = 0;
        m_inputFormatLocation = -1;
    }
}

void NV12Shader::bind(QOpenGLFunctions_4_5_Core* gl) const {
    if (gl != nullptr && m_program != 0) {
        gl->glUseProgram(m_program);
    }
}

void NV12Shader::setInputFormat(QOpenGLFunctions_4_5_Core* gl, InputFormat format) const {
    if (gl != nullptr && m_program != 0 && m_inputFormatLocation >= 0) {
        gl->glUniform1i(m_inputFormatLocation, static_cast<GLint>(format));
    }
}

void NV12Shader::release(QOpenGLFunctions_4_5_Core* gl) const {
    if (gl != nullptr) {
        gl->glUseProgram(0);
    }
}

GLuint NV12Shader::program() const {
    return m_program;
}

GLuint NV12Shader::compileShader(QOpenGLFunctions_4_5_Core* gl, GLenum type, const char* source) {
    const GLuint shader = gl->glCreateShader(type);
    gl->glShaderSource(shader, 1, &source, nullptr);
    gl->glCompileShader(shader);

    GLint compiled = GL_FALSE;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char infoLog[1024]{};
        gl->glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        qWarning().noquote() << "[nv12-shader] compile failed:" << infoLog;
        gl->glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace av::render
