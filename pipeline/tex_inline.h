// tex_inline.h — shared declarations for the bilinear sampler module that
// gets bitcode-linked into compiled shaders on the RISC-V side.
//
// The CPU runtime (pipeline_runtime.cpp) owns the storage of `g_tex`; the
// inline sampler (tex_inline.cpp) reads it. Both translation units include
// this header to agree on layout.
#pragma once

struct TexSlot {
    const float* data;
    int          w;
    int          h;
};

// Defined in pipeline_runtime.cpp. Eight slots, accessed by `bind_texture`
// at runtime; the inlined `__tex_lookup` reads from slot 0 by default.
extern TexSlot g_tex[8];
