#version 440
// 毛玻璃:对已降采样的背景纹理做 9 点模糊并压暗,叠加圆角遮罩
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float radius;
    vec2 texel;
    vec4 tint;
};
layout(binding = 1) uniform sampler2D source;

void main() {
    vec2 uv = qt_TexCoord0;
    vec4 c = texture(source, uv) * 0.2;
    c += texture(source, uv + vec2( texel.x, 0.0)) * 0.1;
    c += texture(source, uv + vec2(-texel.x, 0.0)) * 0.1;
    c += texture(source, uv + vec2(0.0,  texel.y)) * 0.1;
    c += texture(source, uv + vec2(0.0, -texel.y)) * 0.1;
    c += texture(source, uv + texel) * 0.1;
    c += texture(source, uv - texel) * 0.1;
    c += texture(source, uv + vec2(texel.x, -texel.y)) * 0.1;
    c += texture(source, uv + vec2(-texel.x, texel.y)) * 0.1;
    // 与半透明深色叠加 (tint 为预乘颜色)
    vec3 rgb = c.rgb * (1.0 - tint.a) + tint.rgb;

    vec2 p = uv * size;
    vec2 h = size * 0.5;
    vec2 q = abs(p - h) - (h - vec2(radius));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    float a = clamp(0.5 - d, 0.0, 1.0) * qt_Opacity;
    fragColor = vec4(rgb * a, a);
}
