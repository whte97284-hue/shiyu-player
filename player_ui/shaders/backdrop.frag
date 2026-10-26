#version 440
// 沉浸页背景:封面极度模糊后铺满,略提饱和、整体压暗并带暗角,文字始终清晰
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 crop;     // 封面按「填满」裁切时的 uv 缩放
    float spread;  // 模糊半径 (uv)
    float dim;     // 整体亮度
};
layout(binding = 1) uniform sampler2D source;

void main() {
    vec2 uv = (qt_TexCoord0 - 0.5) * crop + 0.5;
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) {
            vec2 o = vec2(float(i), float(j)) / 3.0;
            float w = exp(-dot(o, o) * 2.0);
            acc += texture(source, clamp(uv + o * spread, 0.0, 1.0)).rgb * w;
            wsum += w;
        }
    }
    vec3 c = acc / wsum;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = max(mix(vec3(l), c, 1.3), 0.0);
    vec2 d = qt_TexCoord0 - 0.5;
    float v = 1.0 - dot(d, d) * 1.1;
    fragColor = vec4(c * dim * v, 1.0) * qt_Opacity;
}
