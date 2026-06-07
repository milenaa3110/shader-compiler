// pipeline_runtime.cpp — software vertex→fragment rasterizer.
// Uses the flat-array trampoline ABI emitted by irgen.
// Compile with -fopenmp to parallelize across CPU cores.
//
// Architecture: two-pass tile-based deferred renderer.
//
//   Pass 1 (parallel over triangles):  for each triangle compute its
//          screen-space TriSetup (vertices, edge constants, bbox), cull
//          back-facing / off-screen / w<=0 / degenerate cases, and bin
//          the survivor into every screen tile its bbox overlaps.
//
//   Pass 2 (parallel over tiles):      each tile is owned by exactly one
//          thread, walks its own bin list and rasterises every triangle
//          into its private slice of zbuf/rgb_out. No cross-tile sync,
//          no per-triangle barrier — scales to millions of triangles.
//
// For comparison, the previous design parallelised rows-within-a-triangle
// with an implicit `omp for` barrier per triangle (~T barriers/frame).
// On meshes with T in the millions (e.g. test/assets/teddy-bear) that
// barrier traffic dominated; tile-based eliminates it entirely.

#include "pipeline_runtime.h"
#include "pipeline_abi.h"
#include "tex_inline.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#ifdef _OPENMP
#include <omp.h>
#endif

// ── Software texture sampler ──────────────────────────────────────────────────
// `g_tex` storage lives here. The bilinear-sampling functions
// (`__tex_lookup`, `__tex2d_sample`) are *not* defined in this TU — they are
// supplied by `tex_inline.cpp`, compiled to LLVM bitcode and llvm-link'd
// into each shader module before `opt -O3` runs. Both functions are marked
// `always_inline` there, so opt expands them into the shader's hot loop and
// no cross-TU function call survives. The host-facing `bind_texture` API
// stays scalar code in this TU.
TexSlot g_tex[8] = {};

void bind_texture(int slot, const float* data, int width, int height) {
    if (slot < 0 || slot > 7) return;
    g_tex[slot] = {data, width, height};
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

    const int vtot   = vs_total_floats;
    const int nvar   = vs_varying_floats;
    const int ninput = vs_input_floats;
    (void)fs_output_floats; // MAX_FS_OUT used instead

    const bool indexed     = (desc.indices != nullptr);
    const int  triCount    = indexed ? (desc.index_count / 3) : (NV / 3);
    // Sub-range start: rasterise indices[first_index .. first_index+index_count].
    const int  firstIndex  = indexed ? desc.first_index : 0;

    // 1. Run vertex shader for every vertex.
    // Parallel: each iteration writes a disjoint slice of vsout
    // (`vsout.data() + v * vtot`) and reads only its own input slice plus
    // read-only uniforms. Big win on dense meshes (Jeep/teddy ~100K verts).
    std::vector<float> vsout(vtot * NV);
    #pragma omp parallel for schedule(static)
    for (int v = 0; v < NV; ++v) {
        // Per-vertex attribute pointer (NULL when shader has no inputs).
        float* vin = (ninput > 0 && desc.vbuf)
                       ? const_cast<float*>(desc.vbuf + v * ninput)
                       : nullptr;
        vs_invoke(v, 0, vin, vsout.data() + v * vtot);
    }

    // Static framebuffer-scratch zbuffer that survives across multiple
    // `render_pipeline` calls within the same frame. Cleared only when the
    // host signals start-of-frame via `desc.clear`. Shared across OpenMP
    // workers — must NOT be thread_local. Single-threaded host call site
    // (one render_pipeline call returns before the next begins), so re-
    // sizing on the host thread is safe.
    static std::vector<float> zbuf;
    if (desc.clear) {
        std::memset(rgb_out, 0, W * H * 3);
        zbuf.assign(W * H, 1.f);
    } else if ((int)zbuf.size() != W * H) {
        // Shouldn't happen in normal use, but guard against stale state.
        zbuf.assign(W * H, 1.f);
    }

    // ── Tile-based rasterizer ─────────────────────────────────────────────────
    //
    // Pass 1: per-triangle setup + bin into screen tiles (parallel-over-tris,
    //         per-thread storage so no synchronization is needed).
    // Pass 2: per-tile rasterize (parallel-over-tiles, each tile owns its
    //         pixels so no z-buffer race and no per-triangle barrier).
    //
    // For closed meshes the binning is conservative (bbox-only); a triangle
    // may end up in a tile it doesn't actually cover, but the inner edge
    // test will still reject those pixels — correct, just slightly redundant.

    constexpr int TILE = 32;
    const int TILES_X = (W + TILE - 1) / TILE;
    const int TILES_Y = (H + TILE - 1) / TILE;
    const int NUM_TILES = TILES_X * TILES_Y;

    int max_threads = 1;
#ifdef _OPENMP
    max_threads = omp_get_max_threads();
#endif

    struct TriSetup {
        float sx0, sy0, sx1, sy1, sx2, sy2;
        float iw0, iw1, iw2;
        float nzw0, nzw1, nzw2;
        float invArea;
        int16_t minX, maxX, minY, maxY;
        int32_t i0, i1, i2;  // vsout offsets for varying lookup
    };

    // Per-thread setup storage + per-thread per-tile bins. Each bin entry is
    // a local-to-thread index into that thread's `setups` vector — no global
    // index, no cross-thread sharing during the bin pass.
    //
    // Static across calls: clear() retains capacity, so per-frame heap traffic
    // is just whatever growth a larger mesh demands. For multi-range draws
    // (per-material) this saves NUM_TILES * max_threads vector reallocations
    // per range — significant for high-tri meshes with many materials.
    // Single-threaded host call site, so the static storage is race-free.
    static std::vector<std::vector<TriSetup>>             threadSetups;
    static std::vector<std::vector<std::vector<int32_t>>> threadBins;
    threadSetups.resize(max_threads);
    threadBins.resize(max_threads);
    for (int tid = 0; tid < max_threads; ++tid) {
        threadSetups[tid].clear();
        threadBins[tid].resize(NUM_TILES);
        for (auto& bin : threadBins[tid]) bin.clear();
    }

    // ── Pass 1: setup + bin ──────────────────────────────────────────────────
    #pragma omp parallel
    {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        auto& mySetups = threadSetups[tid];
        auto& myBins   = threadBins[tid];
        // Rough upfront reservation to keep push_back's amortisation cheap.
        mySetups.reserve(triCount / std::max(1, max_threads) / 2 + 64);

        #pragma omp for schedule(static)
        for (int tri = 0; tri < triCount; ++tri) {
            int32_t i0, i1, i2;
            if (indexed) {
                int base = firstIndex + tri * 3;
                i0 = (int32_t)desc.indices[base + 0];
                i1 = (int32_t)desc.indices[base + 1];
                i2 = (int32_t)desc.indices[base + 2];
            } else {
                i0 = tri * 3 + 0;
                i1 = tri * 3 + 1;
                i2 = tri * 3 + 2;
            }
            const float* v0 = vsout.data() + (size_t)i0 * vtot;
            const float* v1 = vsout.data() + (size_t)i1 * vtot;
            const float* v2 = vsout.data() + (size_t)i2 * vtot;

            float w0 = v0[3], w1 = v1[3], w2 = v2[3];
            if (w0 <= 0.f || w1 <= 0.f || w2 <= 0.f) continue;

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

            float ssMinX = std::min({sx0, sx1, sx2});
            float ssMaxX = std::max({sx0, sx1, sx2});
            float ssMinY = std::min({sy0, sy1, sy2});
            float ssMaxY = std::max({sy0, sy1, sy2});
            if (ssMaxX < 0.f || ssMinX >= (float)W ||
                ssMaxY < 0.f || ssMinY >= (float)H) continue;

            float triArea = edgeFunc(sx0, sy0, sx1, sy1, sx2, sy2);
            // Back-face cull + degenerate. Object-space CCW front faces have
            // POSITIVE screen-space edge-function area (CCW NDC → CW screen
            // due to Y-flip; with `edgeFunc(A,B,C) = (Cx-Ax)(By-Ay) -
            // (Cy-Ay)(Bx-Ax)` that flip makes positive the front side).
            // Reject negative (back) and near-zero (degenerate) areas.
            if (triArea < 1e-6f) continue;

            int minX = std::max(0,   (int)std::floor(ssMinX));
            int maxX = std::min(W-1, (int)std::ceil (ssMaxX));
            int minY = std::max(0,   (int)std::floor(ssMinY));
            int maxY = std::min(H-1, (int)std::ceil (ssMaxY));

            TriSetup s;
            s.sx0 = sx0; s.sy0 = sy0;
            s.sx1 = sx1; s.sy1 = sy1;
            s.sx2 = sx2; s.sy2 = sy2;
            s.iw0 = iw0; s.iw1 = iw1; s.iw2 = iw2;
            s.nzw0 = nz0 * iw0; s.nzw1 = nz1 * iw1; s.nzw2 = nz2 * iw2;
            s.invArea = 1.f / triArea;
            s.minX = (int16_t)minX; s.maxX = (int16_t)maxX;
            s.minY = (int16_t)minY; s.maxY = (int16_t)maxY;
            s.i0 = i0; s.i1 = i1; s.i2 = i2;

            int32_t sidx = (int32_t)mySetups.size();
            mySetups.push_back(s);

            int t_minX = minX / TILE;
            int t_maxX = maxX / TILE;
            int t_minY = minY / TILE;
            int t_maxY = maxY / TILE;
            if (t_maxX > TILES_X - 1) t_maxX = TILES_X - 1;
            if (t_maxY > TILES_Y - 1) t_maxY = TILES_Y - 1;

            for (int ty = t_minY; ty <= t_maxY; ++ty) {
                int row = ty * TILES_X;
                for (int tx = t_minX; tx <= t_maxX; ++tx)
                    myBins[row + tx].push_back(sidx);
            }
        }
    }

    // ── Pass 2: per-tile rasterize ───────────────────────────────────────────
    #pragma omp parallel
    {
        float t_interp[MAX_VARYINGS];
        float t_fsout[MAX_FS_OUT];
        float t_fragcoord[4];

        #pragma omp for schedule(dynamic, 1)
        for (int t = 0; t < NUM_TILES; ++t) {
            int tx_lo = (t % TILES_X) * TILE;
            int ty_lo = (t / TILES_X) * TILE;
            int tx_hi = std::min(tx_lo + TILE, W);
            int ty_hi = std::min(ty_lo + TILE, H);

            // Visit every thread's contribution to this tile. Triangle order
            // within a tile doesn't matter for correctness — the z-test
            // (`z >= zbuf[pidx]` reject) preserves the nearest sample
            // regardless of iteration order.
            for (int tid = 0; tid < max_threads; ++tid) {
                const auto& mySetups = threadSetups[tid];
                const auto& bin      = threadBins[tid][t];

                for (int32_t sidx : bin) {
                    const TriSetup& s = mySetups[sidx];

                    // Intersect bbox with this tile.
                    int minX = std::max((int)s.minX, tx_lo);
                    int maxX = std::min((int)s.maxX, tx_hi - 1);
                    int minY = std::max((int)s.minY, ty_lo);
                    int maxY = std::min((int)s.maxY, ty_hi - 1);
                    if (minX > maxX || minY > maxY) continue;

                    // Re-fetch the per-vertex varyings from vsout (kept out
                    // of TriSetup to keep that struct compact — varyings are
                    // small and the fetch is cache-friendly within the tile).
                    const float* v0 = vsout.data() + (size_t)s.i0 * vtot;
                    const float* v1 = vsout.data() + (size_t)s.i1 * vtot;
                    const float* v2 = vsout.data() + (size_t)s.i2 * vtot;

                    float v0w[MAX_VARYINGS], v1w[MAX_VARYINGS], v2w[MAX_VARYINGS];
                    for (int k = 0; k < nvar; ++k) {
                        v0w[k] = v0[4+k] * s.iw0;
                        v1w[k] = v1[4+k] * s.iw1;
                        v2w[k] = v2[4+k] * s.iw2;
                    }

                    const float step_e0 = s.sy2 - s.sy1;
                    const float step_e1 = s.sy0 - s.sy2;
                    const float step_e2 = s.sy1 - s.sy0;
                    const float ystep_e0 = -(s.sx2 - s.sx1);
                    const float ystep_e1 = -(s.sx0 - s.sx2);
                    const float ystep_e2 = -(s.sx1 - s.sx0);
                    const float e0_base = edgeFunc(s.sx1, s.sy1, s.sx2, s.sy2,
                                                    minX + 0.5f, minY + 0.5f);
                    const float e1_base = edgeFunc(s.sx2, s.sy2, s.sx0, s.sy0,
                                                    minX + 0.5f, minY + 0.5f);
                    const float e2_base = edgeFunc(s.sx0, s.sy0, s.sx1, s.sy1,
                                                    minX + 0.5f, minY + 0.5f);

                    for (int py = minY; py <= maxY; ++py) {
                        float pcy = py + 0.5f;
                        float dy  = (float)(py - minY);
                        float e0  = e0_base + dy * ystep_e0;
                        float e1  = e1_base + dy * ystep_e1;
                        float e2  = e2_base + dy * ystep_e2;

                        for (int px = minX; px <= maxX;
                             ++px, e0 += step_e0, e1 += step_e1, e2 += step_e2) {

                            // After the back-face cull above only positive-
                            // area (front-facing in screen space) triangles
                            // remain — a pixel is inside when all three edge
                            // values are >= 0.
                            if (e0 < 0.f || e1 < 0.f || e2 < 0.f) continue;

                            float b0 = e0 * s.invArea;
                            float b1 = e1 * s.invArea;
                            float b2 = 1.f - b0 - b1;

                            float iz     = b0*s.iw0  + b1*s.iw1  + b2*s.iw2;
                            float inv_iz = 1.f / iz;
                            float z      = (b0*s.nzw0 + b1*s.nzw1 + b2*s.nzw2) * inv_iz;
                            z = z * 0.5f + 0.5f;

                            int pidx = py * W + px;
                            if (z >= zbuf[pidx]) continue;
                            zbuf[pidx] = z;  // tile owns this pixel — no race

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
                }
            }
        }
    }
}
