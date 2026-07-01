#pragma once
#include <cstdint>

struct PipelineDesc {
    int width;           // Framebuffer width in pixels
    int height;          // Framebuffer height in pixels
    int vert_count;      // VS invocations to dispatch. If indices is NULL, gl_VertexID 
                         // loops 0..vert_count-1. Otherwise, this is the unique vertex count.

    const float* vbuf;       // Vertex attributes buffer (vert_count * vs_input_floats), or NULL if synthesized
    const uint32_t* indices; // Optional index buffer, or NULL for non-indexed draws
    int             index_count;

    const double* vbuf_d = nullptr; // Optional 64-bit vertex attributes (vert_count * vs_input_doubles)

    int  first_index = 0; // Sub-range index offset for multi-material batches
    bool clear       = true;  // Clear the framebuffer and depth buffer on entry
};

// Dispatches the pipeline through VS -> rasterize -> FS stages. 
// rgb_out must point to width * height * 3 bytes (packed RGB).
void render_pipeline(const PipelineDesc& desc, unsigned char* rgb_out);

// Binds an RGBA float texture to slot 0..7. 
// Underlying data must remain valid for the duration of rendering.
void bind_texture(int slot, const float* data, int width, int height);

// Note: __tex_lookup and __tex2d_sample are emitted as always_inline LLVM 
// bitcode in tex_inline.cpp and are not exposed as public C symbols.