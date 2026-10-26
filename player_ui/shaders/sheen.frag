#version 440
// 唱片光泽:固定方向的对角高光 (两道,呈 X 形),只作用在纹路区域;不随唱片旋转,模拟真实灯光反射
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 tint;
};

void main() {
    vec2 d = qt_TexCoord0 - vec2(0.5);
    float r = length(d);
    float region = smoothstep(0.18, 0.21, r) * (1.0 - smoothstep(0.47, 0.49, r));
    float ang = atan(d.y, d.x);
    float s = pow(abs(cos(ang - 0.75)), 14.0) * 0.16 + pow(abs(cos(ang + 0.85)), 30.0) * 0.07;
    vec3 c = mix(vec3(1.0), tint.rgb, 0.35) * s * region;
    float a = (s * region) * qt_Opacity;
    fragColor = vec4(c * qt_Opacity, a);
}
