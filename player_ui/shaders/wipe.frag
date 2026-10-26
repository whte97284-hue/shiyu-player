#version 440
// 歌词逐行填充:已唱部分全亮,未唱部分压暗,交界处柔和过渡;多行歌词按行依次填充
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float progress;  // 0..1,整句的演唱进度
    float lines;     // 行数
    float soft;      // 过渡带宽度 (uv)
    float dim;       // 未唱部分的亮度
};
layout(binding = 1) uniform sampler2D source;

void main() {
    vec4 c = texture(source, qt_TexCoord0);
    float row = min(floor(qt_TexCoord0.y * lines), lines - 1.0);
    float local = progress * lines - row;
    float lit = 1.0 - smoothstep(local - soft, local + soft, qt_TexCoord0.x);
    fragColor = c * (mix(dim, 1.0, lit) * qt_Opacity);
}
