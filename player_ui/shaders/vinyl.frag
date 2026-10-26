#version 440
// 黑胶唱片:纹路 + 曲间环 + 边缘高光 + 封面标签 + 中心孔 (光泽由 sheen.frag 单独绘制,不随唱片旋转)
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float pixel;     // 1 像素对应的 uv 半径,用于抗锯齿
    vec4 tint;       // 唱片底色的微弱染色 (强调色)
};
layout(binding = 1) uniform sampler2D label;

void main() {
    vec2 d = qt_TexCoord0 - vec2(0.5);
    float r = length(d);
    float edge = clamp((0.5 - r) / pixel, 0.0, 1.0);
    if (edge <= 0.0) { fragColor = vec4(0.0); return; }

    vec3 col;
    const float labelR = 0.175;
    if (r < labelR) {
        // 标签:把封面映射到中心圆
        vec2 luv = d / (labelR * 2.0) + 0.5;
        col = texture(label, luv).rgb;
        float ring = smoothstep(labelR - 0.006, labelR - 0.002, r);
        col = mix(col, col * 0.55, ring);
        float hole = 1.0 - smoothstep(0.012, 0.012 + pixel, r);
        col = mix(col, vec3(0.03), hole);
    } else {
        // 纹路:高频细纹 + 少量曲目间隔 (更暗的宽环)
        float grooves = 0.5 + 0.5 * sin(r * 1400.0);
        float band = smoothstep(0.004, 0.0, abs(fract(r * 9.0 + 0.3) - 0.5) - 0.48);
        col = vec3(0.055, 0.055, 0.062) + tint.rgb * 0.05;
        col += vec3(0.012) * grooves;
        col *= 1.0 - 0.35 * band;
        // 外缘倒角高光
        col += vec3(0.10) * smoothstep(0.485, 0.497, r) * (1.0 - smoothstep(0.497, 0.5, r));
        // 标签外的平滑压边
        col = mix(vec3(0.035), col, smoothstep(labelR, labelR + 0.02, r));
    }
    float a = edge * qt_Opacity;
    fragColor = vec4(col * a, a);
}
