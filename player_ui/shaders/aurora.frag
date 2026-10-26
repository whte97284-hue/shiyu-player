#version 440
// 极光背景:4 个封面主色色团沿李萨如轨迹缓慢漂移、相互融合;暗角 + 抖动噪点防色带
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float time;
    float energy;
    vec4 c0;
    vec4 c1;
    vec4 c2;
    vec4 c3;
    vec4 base;
};

float hash(vec2 p) {
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return fract((p.x + p.y) * p.x);
}

float blob(vec2 p, vec2 c, float r) {
    vec2 d = p - c;
    return exp(-dot(d, d) / (r * r));
}

void main() {
    vec2 uv = qt_TexCoord0;
    float aspect = size.x / max(size.y, 1.0);
    vec2 p = vec2(uv.x * aspect, uv.y);
    float t = time;

    vec2 p0 = vec2((0.22 + 0.16 * sin(t * 0.13)) * aspect, 0.28 + 0.20 * cos(t * 0.11));
    vec2 p1 = vec2((0.82 + 0.12 * cos(t * 0.09 + 1.0)) * aspect, 0.22 + 0.20 * sin(t * 0.12 + 1.3));
    vec2 p2 = vec2((0.58 + 0.22 * sin(t * 0.07 + 2.1)) * aspect, 0.82 + 0.14 * cos(t * 0.10 + 0.7));
    vec2 p3 = vec2((0.08 + 0.12 * cos(t * 0.08 + 3.0)) * aspect, 0.88 + 0.10 * sin(t * 0.09 + 2.0));

    float r = 0.62 + 0.04 * energy;
    float w0 = blob(p, p0, r * 1.05);
    float w1 = blob(p, p1, r * 0.95);
    float w2 = blob(p, p2, r * 1.10);
    float w3 = blob(p, p3, r * 0.85);
    float wsum = w0 + w1 + w2 + w3;
    vec3 mixc = (c0.rgb * w0 + c1.rgb * w1 + c2.rgb * w2 + c3.rgb * w3) / max(wsum, 1e-4);
    vec3 col = mix(base.rgb, mixc, clamp(wsum * 0.95, 0.0, 1.0));
    col *= 1.0 + 0.10 * energy;

    // 暗角:四周与底部更暗,中心与左上保留色彩
    vec2 q = uv - vec2(0.5, 0.42);
    col *= 1.0 - 0.45 * smoothstep(0.25, 0.95, length(q * vec2(1.0, 1.25)));
    col *= mix(1.0, 0.72, smoothstep(0.55, 1.0, uv.y));

    col += (hash(floor(uv * size) + fract(t)) - 0.5) * (1.6 / 255.0);
    fragColor = vec4(col, 1.0) * qt_Opacity;
}
