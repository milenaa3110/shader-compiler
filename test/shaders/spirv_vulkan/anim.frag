#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 FragColor;

void main() {
    float u = vUV.x;
    float v = vUV.y;

    float p = 0.0;
    p += sin(u * 6.2832);
    p += sin(v * 6.2832);
    p += sin((u + v) * 4.0);
    p += sin(sqrt(u * u + v * v + 0.01) * 8.0);
    p = (p + 4.0) * 0.125;

    float r = 0.5 + 0.5 * cos(p * 6.2832 + 0.0)  * vColor.r;
    float g = 0.5 + 0.5 * cos(p * 6.2832 + 2.09) * vColor.g;
    float b = 0.5 + 0.5 * cos(p * 6.2832 + 4.19) * vColor.b;

    FragColor = vec4(r, g, b, 1.0);
}