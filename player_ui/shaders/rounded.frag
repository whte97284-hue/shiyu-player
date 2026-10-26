#version 440
// 圆角遮罩:带 1px 抗锯齿的圆角矩形 SDF,用于封面;corners 依次为左上 / 右上 / 右下 / 左下是否圆角 (拼图封面只圆外侧那个角)
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float radius;
    vec4 corners;
};
layout(binding = 1) uniform sampler2D source;

void main() {
    vec2 p = qt_TexCoord0 * size;
    vec2 h = size * 0.5;
    float r = radius * (p.x < h.x ? (p.y < h.y ? corners.x : corners.w) : (p.y < h.y ? corners.y : corners.z));
    vec2 q = abs(p - h) - (h - vec2(r));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float a = clamp(0.5 - d, 0.0, 1.0);
    fragColor = texture(source, qt_TexCoord0) * (a * qt_Opacity);
}
