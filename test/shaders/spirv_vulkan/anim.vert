#version 450

layout(push_constant) uniform Push {
    float uTime;
} push;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    float vid = float(gl_VertexIndex);

    // Full-screen quad (2 triangles, 6 verts)
    float px = 0.0, py = 0.0;
    if (vid == 0.0) { px = -1.0; py = -1.0; }
    if (vid == 1.0) { px =  1.0; py = -1.0; }
    if (vid == 2.0) { px =  1.0; py =  1.0; }
    if (vid == 3.0) { px = -1.0; py = -1.0; }
    if (vid == 4.0) { px =  1.0; py =  1.0; }
    if (vid == 5.0) { px = -1.0; py =  1.0; }

    float uo = push.uTime * 0.2;
    vUV    = vec2((px + 1.0) * 0.5 + uo,
                  (py + 1.0) * 0.5 + push.uTime * 0.15);
    vColor = vec4(0.5 + 0.5 * sin(push.uTime + vid),
                  0.5 + 0.5 * cos(push.uTime),
                  0.5, 1.0);

    gl_Position = vec4(px, -py, 0.0, 1.0);  // flip Y for Vulkan NDC
}