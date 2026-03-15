// pipeline_runtime.cpp — software vertex→fragment rasterizer
// Uses the flat-array trampoline ABI emitted by irgen.
// Compile with -fopenmp to parallelize pixel coverage across CPU cores.

#include "pipeline_runtime.h"
#include "pipeline_abi.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#ifdef _OPENMP
#include <omp.h>
#endif

// ── Software texture sampler ──────────────────────────────────────────────────
static struct TexSlot { const float* data = nullptr; int w = 0, h = 0; } g_tex[8];

void bind_texture(int slot, const float* data, int width, int height) {
    if (slot < 0 || slot > 7) return;
    g_tex[slot] = {data, width, height};
}

extern "C" void __tex_lookup(int slot, float u, float v, float* out) {
    const auto& t = g_tex[slot < 0 || slot > 7 ? 0 : slot];
    if (!t.data) { out[0] = out[1] = out[2] = out[3] = 0.f; return; }

    u -= std::floor(u);
    v -= std::floor(v);

    float fx = u * (t.w - 1);
    float fy = v * (t.h - 1);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = std::min(x0 + 1, t.w - 1);
    int y1 = std::min(y0 + 1, t.h - 1);
    float tx = fx - x0, ty = fy - y0;

    // Precompute bilinear weights once, then apply to all 4 channels
    float w00 = (1.f - tx) * (1.f - ty);
    float w10 = tx         * (1.f - ty);
    float w01 = (1.f - tx) * ty;
    float w11 = tx         * ty;
    const float* p00 = t.data + (y0 * t.w + x0) * 4;
    const float* p10 = t.data + (y0 * t.w + x1) * 4;
    const float* p01 = t.data + (y1 * t.w + x0) * 4;
    const float* p11 = t.data + (y1 * t.w + x1) * 4;
    out[0] = p00[0]*w00 + p10[0]*w10 + p01[0]*w01 + p11[0]*w11;
    out[1] = p00[1]*w00 + p10[1]*w10 + p01[1]*w01 + p11[1]*w11;
    out[2] = p00[2]*w00 + p10[2]*w10 + p01[2]*w01 + p11[2]*w11;
    out[3] = p00[3]*w00 + p10[3]*w10 + p01[3]*w01 + p11[3]*w11;
}

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

// Upper bounds for per-shader varying and output counts.
// Covers all shaders we generate (2–8 varyings, 4 outputs typical).
static constexpr int MAX_VARYINGS = 16;
static constexpr int MAX_FS_OUT   = 8;

void render_pipeline(const PipelineDesc& desc, unsigned char* rgb_out) {
    const int W = desc.width;
    const int H = desc.height;
    const int NV = desc.vert_count;

    const int vtot = vs_total_floats;
    const int nvar = vs_varying_floats;
    (void)fs_output_floats; // MAX_FS_OUT used instead

    // 1. Run vertex shader for every vertex
    std::vector<float> vbuf(vtot * NV);
    for (int v = 0; v < NV; ++v)
        vs_invoke(v, 0, vbuf.data() + v * vtot);

    std::memset(rgb_out, 0, W * H * 3);
    std::vector<float> zbuf(W * H, 1.f);

    // 2. Spawn thread pool once for the entire frame.
    //    Each thread iterates all triangles; omp for distributes rows within each.
    //    Scratch buffers are stack-allocated per thread — no heap traffic in the hot path.
    #pragma omp parallel
    {
        // Per-thread scratch — reused across all triangles and all rows.
        float t_interp[MAX_VARYINGS];
        float t_fsout[MAX_FS_OUT];
        float t_fragcoord[4];

        for (int tri = 0; tri + 2 < NV; tri += 3) {
            const float* v0 = vbuf.data() + tri       * vtot;
            const float* v1 = vbuf.data() + (tri + 1) * vtot;
            const float* v2 = vbuf.data() + (tri + 2) * vtot;

            float w0 = v0[3], w1 = v1[3], w2 = v2[3];
            if (w0 == 0.f || w1 == 0.f || w2 == 0.f) continue;

            // Reciprocal w — replace all /w divisions below with *iw multiplies
            float iw0 = 1.f/w0, iw1 = 1.f/w1, iw2 = 1.f/w2;

            float nx0 = v0[0]*iw0, ny0 = v0[1]*iw0, nz0 = v0[2]*iw0;
            float nx1 = v1[0]*iw1, ny1 = v1[1]*iw1, nz1 = v1[2]*iw1;
            float nx2 = v2[0]*iw2, ny2 = v2[1]*iw2, nz2 = v2[2]*iw2;

            float sx0 = ( nx0 + 1.f) * 0.5f * W;
            float sy0 = (-ny0 + 1.f) * 0.5f * H;
            float sx1 = ( nx1 + 1.f) * 0.5f * W;
            float sy1 = (-ny1 + 1.f) * 0.5f * H;
            float sx2 = ( nx2 + 1.f) * 0.5f * W;
            float sy2 = (-ny2 + 1.f) * 0.5f * H;

            int minX = std::max(0,   (int)std::floor(std::min({sx0,sx1,sx2})));
            int maxX = std::min(W-1, (int)std::ceil (std::max({sx0,sx1,sx2})));
            int minY = std::max(0,   (int)std::floor(std::min({sy0,sy1,sy2})));
            int maxY = std::min(H-1, (int)std::ceil (std::max({sy0,sy1,sy2})));

            float triArea = edgeFunc(sx0, sy0, sx1, sy1, sx2, sy2);
            if (std::fabs(triArea) < 1e-6f) continue;
            float invArea = 1.f / triArea;
            bool  positive = triArea > 0.f;

            // Per-triangle: scale z and varyings by 1/w so the pixel loop only multiplies.
            // Stack arrays — each thread computes its own copy from the shared read-only vertex data.
            float nzw0 = nz0 * iw0, nzw1 = nz1 * iw1, nzw2 = nz2 * iw2;
            float v0w[MAX_VARYINGS], v1w[MAX_VARYINGS], v2w[MAX_VARYINGS];
            for (int k = 0; k < nvar; ++k) {
                v0w[k] = v0[4+k] * iw0;
                v1w[k] = v1[4+k] * iw1;
                v2w[k] = v2[4+k] * iw2;
            }

            // Incremental edge step per +1 pixel in X
            const float step_e0 = sy2 - sy1;
            const float step_e1 = sy0 - sy2;
            const float step_e2 = sy1 - sy0;

            // Incremental edge step per +1 pixel in Y — avoids calling edgeFunc per row
            const float ystep_e0 = -(sx2 - sx1);
            const float ystep_e1 = -(sx0 - sx2);
            const float ystep_e2 = -(sx1 - sx0);
            const float e0_base = edgeFunc(sx1, sy1, sx2, sy2, minX + 0.5f, minY + 0.5f);
            const float e1_base = edgeFunc(sx2, sy2, sx0, sy0, minX + 0.5f, minY + 0.5f);
            const float e2_base = edgeFunc(sx0, sy0, sx1, sy1, minX + 0.5f, minY + 0.5f);

            // Distribute rows across threads. Implicit barrier after the loop ensures
            // triangle T is fully written to zbuf/rgb_out before triangle T+1 starts.
            #pragma omp for schedule(dynamic, 1)
            for (int py = minY; py <= maxY; ++py) {
                float pcy = py + 0.5f;
                float dy  = (float)(py - minY);
                float e0  = e0_base + dy * ystep_e0;
                float e1  = e1_base + dy * ystep_e1;
                float e2  = e2_base + dy * ystep_e2;

                for (int px = minX; px <= maxX;
                     ++px, e0 += step_e0, e1 += step_e1, e2 += step_e2) {

                    if (positive ? (e0 < 0.f || e1 < 0.f || e2 < 0.f)
                                 : (e0 > 0.f || e1 > 0.f || e2 > 0.f)) continue;

                    float b0 = e0 * invArea;
                    float b1 = e1 * invArea;
                    float b2 = 1.f - b0 - b1;

                    float iz     = b0*iw0  + b1*iw1  + b2*iw2;
                    float inv_iz = 1.f / iz;
                    float z      = (b0*nzw0 + b1*nzw1 + b2*nzw2) * inv_iz;
                    z = z * 0.5f + 0.5f;

                    int pidx = py * W + px;
                    if (z >= zbuf[pidx]) continue;
                    zbuf[pidx] = z;  // NOTE: not atomic, minor z-fight artifact ok

                    for (int k = 0; k < nvar; ++k)
                        t_interp[k] = (b0*v0w[k] + b1*v1w[k] + b2*v2w[k]) * inv_iz;

                    t_fragcoord[0] = px + 0.5f;
                    t_fragcoord[1] = (float)H - pcy;
                    t_fragcoord[2] = z;
                    t_fragcoord[3] = iz;

                    fs_invoke(t_fragcoord, t_interp, t_fsout);

                    unsigned char* px_ptr = rgb_out + pidx * 3;
                    px_ptr[0] = (unsigned char)(clamp01(t_fsout[0]) * 255.f + 0.5f);
                    px_ptr[1] = (unsigned char)(clamp01(t_fsout[1]) * 255.f + 0.5f);
                    px_ptr[2] = (unsigned char)(clamp01(t_fsout[2]) * 255.f + 0.5f);
                }
            }
            // implicit omp barrier — triangle T fully complete before T+1
        }
    }
}
