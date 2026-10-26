#version 440
// 上下边缘渐隐:滚动列表在可视区边缘柔和消失,而不是被硬切
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float fadeTop;     // 顶部渐隐带 (uv)
    float fadeBottom;  // 底部渐隐带 (uv)
};
layout(binding = 1) uniform sampler2D source;

void main() {
    float y = qt_TexCoord0.y;
    float a = smoothstep(0.0, fadeTop, y) * (1.0 - smoothstep(1.0 - fadeBottom, 1.0, y));
    fragColor = texture(source, qt_TexCoord0) * (a * qt_Opacity);
}
