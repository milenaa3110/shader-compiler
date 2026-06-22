#pragma once
#include <cstdint>

struct PipelineDesc {
    int width;          // framebuffer width  in pixels
    int height;         // framebuffer height in pixels
    int vert_count;     // VS invocations to dispatch.
                        //   - if `indices` is NULL: gl_VertexID iterates 0..vert_count-1
                        //     (every 3 form one triangle).
                        //   - if `indices` is non-NULL: this is the *unique* vertex count
                        //     and `index_count` is the number of indices.

    // Optional: per-vertex input attributes (matches `vs_input_floats` floats per vertex).
    // Pass NULL if the shader synthesizes geometry from gl_VertexID (e.g. terrain).
    const float* vbuf;       // vert_count * vs_input_floats floats
    const uint32_t* indices; // index_count uint32_t indices into vbuf, or NULL for non-indexed
    int            index_count;

    // Sub-range support for multi-material draws. The rasteriser reads
    // indices starting at `indices + first_index` and processes
    // `index_count` indices total (i.e. `index_count` is the *range* count,
    // not the whole-mesh count).
    int  first_index = 0;
    // When false the framebuffer + zbuffer are NOT reset on entry, so the
    // host can stack multiple `render_pipeline` calls per frame (one per
    // material range). Set true on the first call of a frame, false after.
    bool clear       = true;
};

// Render vert_count vertices through the VS→rasterize→FS pipeline.
// rgb_out must point to width*height*3 bytes (packed R,G,B, top-to-bottom).
void render_pipeline(const PipelineDesc& desc, unsigned char* rgb_out);

// ── Texture API (CPU software bilinear sampler) ───────────────────────────────
// Bind a RGBA float texture to slot 0..7.
// data must remain valid for the duration of rendering.
void bind_texture(int slot, const float* data, int width, int height);

// `__tex_lookup` and `__tex2d_sample` are emitted as `always_inline` LLVM
// bitcode in `tex_inline.cpp` and linked into each shader module before
// `opt -O3`. They are not exposed as C symbols in pipeline_runtime.o.