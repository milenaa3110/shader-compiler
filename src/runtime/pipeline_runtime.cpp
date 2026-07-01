#pragma once
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

// Global texture slots accessed by always_inline shader routines
TexSlot g_tex[8] = {};

void bind_texture(int slot, const float* data, int width, int height) {
    if (slot >= 0 && slot <= 7) {
        g_tex[slot] = {data, width, height};
    }
}

static inline float edgeFunc(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static inline float clamp01(float v) {
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

static constexpr int MAX_VARYINGS = 16;
static constexpr int MAX_FS_OUT   = 8;
static constexpr int PACKET_W     = 4;

extern "C" __attribute__((weak)) void fs_packet(const float* vary_soa, const float* frag_soa, float* out_soa, int* live_out);
extern "C" __attribute__((weak)) void vs_packet(const float* in_soa, int vid_base, int iid, float* out_soa);

void render_pipeline(const PipelineDesc& desc, unsigned char* rgb_out) {
    const int W = desc.width;
    const int H = desc.height;
    const int NV = desc.vert_count;

    const int vtot     = vs_total_floats;
    const int nvar     = vs_varying_floats;
    const int ninput   = vs_input_floats;
    const int ninput_d = vs_input_doubles;

    const bool indexed   = (desc.indices != nullptr);
    const int triCount   = indexed ? (desc.index_count / 3) : (NV / 3);
    const int firstIndex = indexed ? desc.first_index : 0;

    // Vertex Shader Execution Pass
    std::vector<float> vsout(vtot * NV);
    const bool vsPacketMode = (vs_packet != nullptr) && (std::getenv("SHADER_PACKET") != nullptr);

    if (vsPacketMode) {
        #pragma omp parallel for schedule(static)
        for (int v0 = 0; v0 < NV; v0 += PACKET_W) {
            const int n = std::min(PACKET_W, NV - v0);
            std::vector<float> in_soa((size_t)ninput * PACKET_W);
            std::vector<float> out_soa((size_t)vtot * PACKET_W);
            for (int l = 0; l < PACKET_W; ++l) {
                int v = (l < n) ? v0 + l : v0;
                const float* vin = (ninput > 0 && desc.vbuf) ? desc.vbuf + (size_t)v * ninput : nullptr;
                for (int k = 0; k < ninput; ++k) in_soa[(size_t)k * PACKET_W + l] = vin ? vin[k] : 0.f;
            }
            vs_packet(in_soa.data(), v0, 0, out_soa.data());
            for (int l = 0; l < n; ++l) {
                float* dst = vsout.data() + (size_t)(v0 + l) * vtot;
                for (int c = 0; c < vtot; ++c) dst[c] = out_soa[(size_t)c * PACKET_W + l];
            }
        }
    } else {
        #pragma omp parallel for schedule(static)
        for (int v = 0; v < NV; ++v) {
            float* vin = (ninput > 0 && desc.vbuf) ? const_cast<float*>(desc.vbuf + v * ninput) : nullptr;
            double* vin_d = (ninput_d > 0 && desc.vbuf_d) ? const_cast<double*>(desc.vbuf_d + v * ninput_d) : nullptr;
            vs_invoke(v, 0, vin, vin_d, vsout.data() + v * vtot);
        }
    }

    // Persistent Scratch Depth Buffer setup
    static std::vector<float> zbuf;
    if (desc.clear) {
        std::memset(rgb_out, 0, W * H * 3);
        zbuf.assign(W * H, 1.f);
    } else if ((int)zbuf.size() != W * H) {
        zbuf.assign(W * H, 1.f);
    }

    // Tile-Based Binning Configuration
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
        int32_t i0, i1, i2;
    };

    static std::vector<std::vector<TriSetup>>             threadSetups;
    static std::vector<std::vector<std::vector<int32_t>>> threadBins;
    threadSetups.resize(max_threads);
    threadBins.resize(max_threads);
    for (int tid = 0; tid < max_threads; ++tid) {
        threadSetups[tid].clear();
        threadBins[tid].resize(NUM_TILES);
        for (auto& bin : threadBins[tid]) bin.clear();
    }

    // Triangle Setup & Conservative Binning 
    #pragma omp parallel
    {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        auto& mySetups = threadSetups[tid];
        auto& myBins   = threadBins[tid];
        mySetups.reserve(triCount / std::max(1, max_threads) / 2 + 64);

        #pragma omp_for schedule(static)
        for (int tri = 0; tri < triCount; ++tri) {
            int32_t i0, i1, i2;
            if (indexed) {
                int base = firstIndex + tri * 3;
                i0 = (int32_t)desc.indices[base + 0];
                i1 = (int32_t)desc.indices[base + 1];
                i2 = (int32_t)desc.indices[base + 2];
            } else {
                i0 = tri * 3 + 0; i1 = tri * 3 + 1; i2 = tri * 3 + 2;
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
            if (ssMaxX < 0.f || ssMinX >= (float)W || ssMaxY < 0.f || ssMinY >= (float)H) continue;

            float triArea = edgeFunc(sx0, sy0, sx1, sy1, sx2, sy2);
            if (triArea < 1e-6f) continue; // Back-face and degenerate cull

            int minX = std::max(0,   (int)std::floor(ssMinX));
            int maxX = std::min(W-1, (int)std::ceil (ssMaxX));
            int minY = std::max(0,   (int)std::floor(ssMinY));
            int maxY = std::min(H-1, (int)std::ceil (ssMaxY));

            TriSetup s;
            s.sx0 = sx0; s.sy0 = sy0; s.sx1 = sx1; s.sy1 = sy1; s.sx2 = sx2; s.sy2 = sy2;
            s.iw0 = iw0; s.iw1 = iw1; s.iw2 = iw2;
            s.nzw0 = nz0 * iw0; s.nzw1 = nz1 * iw1; s.nzw2 = nz2 * iw2;
            s.invArea = 1.f / triArea;
            s.minX = (int16_t)minX; s.maxX = (int16_t)maxX; s.minY = (int16_t)minY; s.maxY = (int16_t)maxY;
            s.i0 = i0; s.i1 = i1; s.i2 = i2;

            int32_t sidx = (int32_t)mySetups.size();
            mySetups.push_back(s);

            int t_minX = minX / TILE; int t_maxX = maxX / TILE;
            int t_minY = minY / TILE; int t_maxY = maxY / TILE;
            if (t_maxX > TILES_X - 1) t_maxX = TILES_X - 1;
            if (t_maxY > TILES_Y - 1) t_maxY = TILES_Y - 1;

            for (int ty = t_minY; ty <= t_maxY; ++ty) {
                int row = ty * TILES_X;
                for (int tx = t_minX; tx <= t_maxX; ++tx)
                    myBins[row + tx].push_back(sidx);
            }
        }
    }

    // Parallel Per-Tile Rasterization & Fragment Execution 
    const bool packetMode = (fs_packet != nullptr) && (std::getenv("SHADER_PACKET") != nullptr);

    #pragma omp parallel
    {
        float t_interp[MAX_VARYINGS];
        float t_fsout[MAX_FS_OUT];
        double t_fsout_d[MAX_FS_OUT];
        float t_fragcoord[4];

        int   b_pidx[PACKET_W];
        float b_vary[MAX_VARYINGS * PACKET_W];
        float b_frag[4 * PACKET_W];
        int   bn = 0;

        auto writePixel = [&](int pidx, float r, float g, float b) {
            unsigned char* p = rgb_out + pidx * 3;
            p[0] = (unsigned char)(clamp01(r) * 255.f + 0.5f);
            p[1] = (unsigned char)(clamp01(g) * 255.f + 0.5f);
            p[2] = (unsigned char)(clamp01(b) * 255.f + 0.5f);
        };

        auto flush = [&]() {
            if (bn == 0) return;
            float osoa[MAX_FS_OUT * PACKET_W];
            int   live[PACKET_W];
            for (int l = bn; l < PACKET_W; ++l) {
                b_pidx[l] = b_pidx[0];
                for (int k = 0; k < nvar; ++k) b_vary[k * PACKET_W + l] = b_vary[k * PACKET_W];
                for (int c = 0; c < 4; ++c)    b_frag[c * PACKET_W + l] = b_frag[c * PACKET_W];
            }
            fs_packet(b_vary, b_frag, osoa, live);
            for (int l = 0; l < bn; ++l) {
                if (!live[l]) continue;
                writePixel(b_pidx[l], osoa[0 * PACKET_W + l], osoa[1 * PACKET_W + l], osoa[2 * PACKET_W + l]);
            }
            bn = 0;
        };

        #pragma omp for schedule(dynamic, 1)
        for (int t = 0; t < NUM_TILES; ++t) {
            int tx_lo = (t % TILES_X) * TILE;
            int ty_lo = (t / TILES_X) * TILE;
            int tx_hi = std::min(tx_lo + TILE, W);
            int ty_hi = std::min(ty_lo + TILE, H);

            for (int tid = 0; tid < max_threads; ++tid) {
                const auto& mySetups = threadSetups[tid];
                const auto& bin      = threadBins[tid][t];

                for (int32_t sidx : bin) {
                    const TriSetup& s = mySetups[sidx];

                    int minX = std::max((int)s.minX, tx_lo); int maxX = std::min((int)s.maxX, tx_hi - 1);
                    int minY = std::max((int)s.minY, ty_lo); int maxY = std::min((int)s.maxY, ty_hi - 1);
                    if (minX > maxX || minY > maxY) continue;

                    const float* v0 = vsout.data() + (size_t)s.i0 * vtot;
                    const float* v1 = vsout.data() + (size_t)s.i1 * vtot;
                    const float* v2 = vsout.data() + (size_t)s.i2 * vtot;

                    float v0w[MAX_VARYINGS], v1w[MAX_VARYINGS], v2w[MAX_VARYINGS];
                    for (int k = 0; k < nvar; ++k) {
                        v0w[k] = v0[4+k] * s.iw0; v1w[k] = v1[4+k] * s.iw1; v2w[k] = v2[4+k] * s.iw2;
                    }

                    const float step_e0 = s.sy2 - s.sy1;   const float step_e1 = s.sy0 - s.sy2;   const float step_e2 = s.sy1 - s.sy0;
                    const float ystep_e0 = -(s.sx2 - s.sx1); const float ystep_e1 = -(s.sx0 - s.sx2); const float ystep_e2 = -(s.sx1 - s.sx0);
                    
                    const float e0_base = edgeFunc(s.sx1, s.sy1, s.sx2, s.sy2, minX + 0.5f, minY + 0.5f);
                    const float e1_base = edgeFunc(s.sx2, s.sy2, s.sx0, s.sy0, minX + 0.5f, minY + 0.5f);
                    const float e2_base = edgeFunc(s.sx0, s.sy0, s.sx1, s.sy1, minX + 0.5f, minY + 0.5f);

                    for (int py = minY; py <= maxY; ++py) {
                        float pcy = py + 0.5f; float dy = (float)(py - minY);
                        float e0 = e0_base + dy * ystep_e0;
                        float e1 = e1_base + dy * ystep_e1;
                        float e2 = e2_base + dy * ystep_e2;

                        for (int px = minX; px <= maxX; ++px, e0 += step_e0, e1 += step_e1, e2 += step_e2) {
                            if (e0 < 0.f || e1 < 0.f || e2 < 0.f) continue;

                            float b0 = e0 * s.invArea; float b1 = e1 * s.invArea; float b2 = 1.f - b0 - b1;
                            float iz = b0*s.iw0 + b1*s.iw1 + b2*s.iw2;
                            float inv_iz = 1.f / iz;
                            float z = (b0*s.nzw0 + b1*s.nzw1 + b2*s.nzw2) * inv_iz;
                            z = z * 0.5f + 0.5f;

                            int pidx = py * W + px;
                            if (z >= zbuf[pidx]) continue;
                            zbuf[pidx] = z;

                            if (packetMode) {
                                b_pidx[bn] = pidx;
                                for (int k = 0; k < nvar; ++k) b_vary[k * PACKET_W + bn] = (b0*v0w[k] + b1*v1w[k] + b2*v2w[k]) * inv_iz;
                                b_frag[0 * PACKET_W + bn] = px + 0.5f;
                                b_frag[1 * PACKET_W + bn] = (float)H - pcy;
                                b_frag[2 * PACKET_W + bn] = z;
                                b_frag[3 * PACKET_W + bn] = iz;
                                if (++bn == PACKET_W) flush();
                            } else {
                                for (int k = 0; k < nvar; ++k) t_interp[k] = (b0*v0w[k] + b1*v1w[k] + b2*v2w[k]) * inv_iz;
                                t_fragcoord[0] = px + 0.5f; t_fragcoord[1] = (float)H - pcy; t_fragcoord[2] = z; t_fragcoord[3] = iz;
                                fs_invoke(t_fragcoord, t_interp, t_fsout, t_fsout_d);
                                writePixel(pidx, t_fsout[0], t_fsout[1], t_fsout[2]);
                            }
                        }
                    }
                }
            }
            if (packetMode) flush();
        }
    }
}