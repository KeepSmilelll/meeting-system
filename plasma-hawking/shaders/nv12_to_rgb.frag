#version 460 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D texY;
uniform sampler2D texUV;
uniform sampler2D texU;
uniform sampler2D texV;
uniform int u_inputFormat;
void main() {
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
