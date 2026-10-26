#version 440
// 胶片颗粒:打散大面积模糊渐变的色带 (banding),让背景更有质感
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float strength;
};

float hash(vec2 p) {
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return fract((p.x + p.y) * p.x);
}

void main() {
    float n = hash(floor(qt_TexCoord0 * size));
    float a = strength * qt_Opacity;
    // 预乘 alpha:亮/暗颗粒随机分布
    fragColor = vec4(vec3(n) * a, a);
}
