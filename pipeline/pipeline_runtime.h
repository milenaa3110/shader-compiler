#pragma once
#include <cstdint>

struct PipelineDesc {
    int width;        // framebuffer width  in pixels
    int height;       // framebuffer height in pixels
    int vert_count;   // total vertices (every 3 form one triangle)
};

// Render vert_count vertices through the VS→rasterize→FS pipeline.
// rgb_out must point to width*height*3 bytes (packed R,G,B, top-to-bottom).
void render_pipeline(const PipelineDesc& desc, unsigned char* rgb_out);

// ── Texture API (CPU software bilinear sampler) ───────────────────────────────
// Bind a RGBA float texture to slot 0..7.
// data must remain valid for the duration of rendering.
void bind_texture(int slot, const float* data, int width, int height);

// Called from shader IR (lowered from texture(sampler2D, vec2) builtins).
// Returns bilinear-interpolated RGBA into out[4].
extern "C" void __tex_lookup(int slot, float u, float v, float* out);