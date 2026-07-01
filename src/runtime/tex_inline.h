// tex_inline.h — Shared declarations for the inline bilinear sampler module.
// Links directly into compiled IR shader modules on the RISC-V target backend.

#pragma once

// Memory layout descriptor for texture buffer allocation metadata
struct TexSlot {
    const float* data;  // Raw pointer to flat floating-point RGBA components
    int          w;     // Physical allocation width in pixels
    int          h;     // Physical allocation height in pixels
};

// Texture slot registers defined in pipeline_runtime.cpp and resolved at JIT link time
extern TexSlot g_tex[8];