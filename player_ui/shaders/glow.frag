#version 440
// 椭圆柔光 (预乘 alpha),用于播放岛下方随低音呼吸的光晕
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 color;
    float intensity;
};

void main() {
    vec2 d = (qt_TexCoord0 - vec2(0.5)) * 2.0;
    float f = exp(-dot(d, d) * 3.2);
    float a = f * intensity * qt_Opacity;
    fragColor = vec4(color.rgb * a, a);
}
