// pipeline_runtime.cpp — software vertex→fragment rasterizer
// Uses the flat-array trampoline ABI emitted by irgen.

#include "pipeline_runtime.h"
#include "pipeline_abi.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

// ── helpers ───────────────────────────────────────────────────────────────────

static inline float edgeFunc(float ax, float ay,
                              float bx, float by,
                              float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static inline float clamp01(float v) {
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

// ── rasterizer ────────────────────────────────────────────────────────────────

void render_pipeline(const PipelineDesc& desc, unsigned char* rgb_out) {
    const int W = desc.width;
    const int H = desc.height;
    const int NV = desc.vert_count;

    const int vtot = vs_total_floats;    // e.g. 10 = 4(pos) + 6(varyings)
    const int nvar = vs_varying_floats;  // e.g. 6
    const int nfso = fs_output_floats;   // e.g. 4

    // 1. Run vertex shader for every vertex
    std::vector<float> vbuf(vtot * NV);
    for (int v = 0; v < NV; ++v)
        vs_invoke(v, 0, vbuf.data() + v * vtot);

    // Zero framebuffer
    std::memset(rgb_out, 0, W * H * 3);

    // Depth buffer
    std::vector<float> zbuf(W * H, 1.f);

    // Interpolated varyings + FS output scratch buffers
    std::vector<float> interp(nvar);
    std::vector<float> fsout(nfso);
    float fragcoord[4];

    // 2. Iterate triangles
    for (int tri = 0; tri + 2 < NV; tri += 3) {
        const float* v0 = vbuf.data() + tri       * vtot;
        const float* v1 = vbuf.data() + (tri + 1) * vtot;
        const float* v2 = vbuf.data() + (tri + 2) * vtot;

        // Clip-space positions (x,y,z,w) are first 4 floats
        float w0 = v0[3], w1 = v1[3], w2 = v2[3];
        if (w0 == 0.f || w1 == 0.f || w2 == 0.f) continue;

        // NDC
        float nx0 = v0[0]/w0, ny0 = v0[1]/w0, nz0 = v0[2]/w0;
        float nx1 = v1[0]/w1, ny1 = v1[1]/w1, nz1 = v1[2]/w1;
        float nx2 = v2[0]/w2, ny2 = v2[1]/w2, nz2 = v2[2]/w2;

        // Screen-space (Y flipped: NDC +1 = top → pixel row 0)
        float sx0 = ( nx0 + 1.f) * 0.5f * W;
        float sy0 = (-ny0 + 1.f) * 0.5f * H;
        float sx1 = ( nx1 + 1.f) * 0.5f * W;
        float sy1 = (-ny1 + 1.f) * 0.5f * H;
        float sx2 = ( nx2 + 1.f) * 0.5f * W;
        float sy2 = (-ny2 + 1.f) * 0.5f * H;

        // Bounding box (clamped to framebuffer)
        int minX = std::max(0,   (int)std::floor(std::min({sx0,sx1,sx2})));
        int maxX = std::min(W-1, (int)std::ceil (std::max({sx0,sx1,sx2})));
        int minY = std::max(0,   (int)std::floor(std::min({sy0,sy1,sy2})));
        int maxY = std::min(H-1, (int)std::ceil (std::max({sy0,sy1,sy2})));

        float triArea = edgeFunc(sx0, sy0, sx1, sy1, sx2, sy2);
        if (std::fabs(triArea) < 1e-6f) continue;

        for (int py = minY; py <= maxY; ++py) {
            for (int px = minX; px <= maxX; ++px) {
                float pcx = px + 0.5f, pcy = py + 0.5f;

                float e0 = edgeFunc(sx1, sy1, sx2, sy2, pcx, pcy);
                float e1 = edgeFunc(sx2, sy2, sx0, sy0, pcx, pcy);
                float e2 = edgeFunc(sx0, sy0, sx1, sy1, pcx, pcy);

                // Back-face culling: accept only if area > 0 (CCW winding)
                if (triArea > 0.f) {
                    if (e0 < 0.f || e1 < 0.f || e2 < 0.f) continue;
                } else {
                    if (e0 > 0.f || e1 > 0.f || e2 > 0.f) continue;
                }

                // Barycentric weights
                float b0 = e0 / triArea;
                float b1 = e1 / triArea;
                float b2 = e2 / triArea;

                // Perspective-correct depth
                float iz = b0 / w0 + b1 / w1 + b2 / w2;  // 1/w interp
                float z  = b0 * nz0 / w0 + b1 * nz1 / w1 + b2 * nz2 / w2;
                z /= iz;
                z = z * 0.5f + 0.5f;  // map [-1,1] → [0,1]

                int pidx = py * W + px;
                if (z >= zbuf[pidx]) continue;
                zbuf[pidx] = z;

                // Perspective-correct varying interpolation
                for (int k = 0; k < nvar; ++k) {
                    float fv = b0 * v0[4+k] / w0
                             + b1 * v1[4+k] / w1
                             + b2 * v2[4+k] / w2;
                    interp[k] = fv / iz;
                }

                // Fragment coordinate (window-space)
                fragcoord[0] = pcx;
                fragcoord[1] = (float)H - pcy;   // flip Y to match OpenGL convention
                fragcoord[2] = z;
                fragcoord[3] = iz;

                // Run fragment shader
                fs_invoke(fragcoord, interp.data(), fsout.data());

                // Write RGB
                unsigned char* px_ptr = rgb_out + (py * W + px) * 3;
                px_ptr[0] = (unsigned char)(clamp01(fsout[0]) * 255.f + 0.5f);
                px_ptr[1] = (unsigned char)(clamp01(fsout[1]) * 255.f + 0.5f);
                px_ptr[2] = (unsigned char)(clamp01(fsout[2]) * 255.f + 0.5f);
            }
        }
    }
}