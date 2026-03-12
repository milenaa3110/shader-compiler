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