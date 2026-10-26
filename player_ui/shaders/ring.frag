#version 440
// 进度环:细轨道 + 圆头弧线,像素级抗锯齿 (预乘 alpha)
// 角度从正上方顺时针计;start / span 均为圈数 (0..1)
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 color;       // 弧线颜色
    vec4 trackColor;  // 轨道颜色 (含透明度)
    float side;       // 控件边长 (像素)
    float arcWidth;   // 弧线粗细 (像素)
    float trackWidth; // 轨道粗细 (像素)
    float start;
    float span;
};

const float TAU = 6.28318530718;

void main() {
    vec2 p = (qt_TexCoord0 - vec2(0.5)) * side;
    float r = side * 0.5 - arcWidth;            // 环的中线半径
    float radial = abs(length(p) - r);

    float track = clamp(trackWidth * 0.5 - radial + 0.5, 0.0, 1.0) * trackColor.a;

    float arc = 0.0;
    if (span > 0.0005) {
        float ang = atan(p.x, -p.y) / TAU;      // 0 在正上方,顺时针增大
        float rel = fract(ang - start);
        float d;
        if (span >= 0.9995 || rel <= span) {
            d = radial;
        } else {
            // 圆头:到两个端点的距离
            float a0 = start * TAU, a1 = (start + span) * TAU;
            vec2 e0 = vec2(sin(a0), -cos(a0)) * r;
            vec2 e1 = vec2(sin(a1), -cos(a1)) * r;
            d = min(length(p - e0), length(p - e1));
        }
        arc = clamp(arcWidth * 0.5 - d + 0.5, 0.0, 1.0) * color.a;
    }

    vec3 rgb = trackColor.rgb * track * (1.0 - arc) + color.rgb * arc;
    float a = track * (1.0 - arc) + arc;
    fragColor = vec4(rgb, a) * qt_Opacity;
}
