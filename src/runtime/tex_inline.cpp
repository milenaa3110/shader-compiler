// tex_inline.cpp — Inline sampler/image runtime compiled to LLVM bitcode and
// linked into shader IR before optimization. Every entry point is
// always_inline so the math is embedded directly at the call site.
//
// Each intrinsic takes an `int slot` (the sampler/image binding) so distinct
// textures/images read distinct data. Sampled textures live in g_tex (with a
// `depth` for cube faces / array layers / 3D slices); storage images live in the
// mutable g_img table.

#include "tex_inline.h"

#include <cmath>

// Bilinear fetch within slice `slice` of a sampled slot. Slices are contiguous
// w*h*4-float planes (cube face / array layer / volume slice).
static inline void tex_slice(int slot, int slice, float u, float v, float* out) {
    const TexSlot& t = g_tex[(unsigned)slot > 7u ? 0 : slot];
    if (!t.data) { out[0] = out[1] = out[2] = out[3] = 0.f; return; }
    if (slice < 0) slice = 0;
    if (slice > t.depth - 1) slice = t.depth - 1;

    u -= std::floor(u);
    v -= std::floor(v);
    float fx = u * (t.w - 1), fy = v * (t.h - 1);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1; if (x1 > t.w - 1) x1 = t.w - 1;
    int y1 = y0 + 1; if (y1 > t.h - 1) y1 = t.h - 1;
    float tx = fx - x0, ty = fy - y0;
    float w00 = (1.f - tx) * (1.f - ty), w10 = tx * (1.f - ty);
    float w01 = (1.f - tx) * ty,         w11 = tx * ty;

    const float* base = t.data + (long)slice * t.w * t.h * 4;
    const float* p00 = base + (y0 * t.w + x0) * 4;
    const float* p10 = base + (y0 * t.w + x1) * 4;
    const float* p01 = base + (y1 * t.w + x0) * 4;
    const float* p11 = base + (y1 * t.w + x1) * 4;
    for (int c = 0; c < 4; ++c)
        out[c] = p00[c]*w00 + p10[c]*w10 + p01[c]*w01 + p11[c]*w11;
}

// Legacy 2D lookup (kept for any external caller): slice 0 of the slot.
extern "C" __attribute__((always_inline))
void __tex_lookup(int slot, float u, float v, float* out) {
    tex_slice(slot, 0, u, v, out);
}

extern "C" __attribute__((always_inline))
void __tex2d_sample(void* /*sampler*/, int slot, float u, float v, float* out) {
    tex_slice(slot, 0, u, v, out);
}

extern "C" __attribute__((always_inline))
void __tex2d_sample_lod(void* /*sampler*/, int slot, float u, float v,
                        float /*lod*/, float* out) {
    // Base level: no mip pyramid is bound (a per-level TexSlot is a follow-up).
    tex_slice(slot, 0, u, v, out);
}

extern "C" __attribute__((always_inline))
void __texcube_sample(void* /*sampler*/, int slot, float x, float y, float z,
                      float* out) {
    // GL cube-face order: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z. Pick the major axis,
    // project onto that face's (s,t), then sample the face's slice.
    float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    int face; float s, t, ma;
    if (ax >= ay && ax >= az) {
        ma = ax; if (x > 0.f) { face = 0; s = -z; t = -y; } else { face = 1; s = z;  t = -y; }
    } else if (ay >= az) {
        ma = ay; if (y > 0.f) { face = 2; s =  x; t =  z; } else { face = 3; s = x;  t = -z; }
    } else {
        ma = az; if (z > 0.f) { face = 4; s =  x; t = -y; } else { face = 5; s = -x; t = -y; }
    }
    if (ma == 0.f) ma = 1e-8f;
    tex_slice(slot, face, 0.5f * (s / ma) + 0.5f, 0.5f * (t / ma) + 0.5f, out);
}

extern "C" __attribute__((always_inline))
void __tex3d_sample(void* /*sampler*/, int slot, float u, float v, float w,
                    float* out) {
    // Trilinear: bilinear on the two bracketing depth slices, lerp by w.
    const TexSlot& t = g_tex[(unsigned)slot > 7u ? 0 : slot];
    int D = t.depth < 1 ? 1 : t.depth;
    float fw = (w - std::floor(w)) * (D - 1);
    int z0 = (int)fw; int z1 = z0 + 1; if (z1 > D - 1) z1 = D - 1;
    float tz = fw - z0;
    float a[4], b[4];
    tex_slice(slot, z0, u, v, a);
    tex_slice(slot, z1, u, v, b);
    for (int c = 0; c < 4; ++c) out[c] = a[c] * (1.f - tz) + b[c] * tz;
}

extern "C" __attribute__((always_inline))
void __tex2darray_sample(void* /*sampler*/, int slot, float u, float v,
                         float layer, float* out) {
    // Nearest layer, bilinear within it.
    tex_slice(slot, (int)(layer + 0.5f), u, v, out);
}

// Explicit-LOD variants for the vec3 samplers. LOD clamps to the base level for
// now (no mip pyramid is bound); they exist so textureLod dispatches correctly by
// type instead of mis-sampling a cube/3D/array as 2D.
extern "C" __attribute__((always_inline))
void __texcube_sample_lod(void* s, int slot, float x, float y, float z,
                          float /*lod*/, float* out) {
    __texcube_sample(s, slot, x, y, z, out);
}
extern "C" __attribute__((always_inline))
void __tex3d_sample_lod(void* s, int slot, float u, float v, float w,
                        float /*lod*/, float* out) {
    __tex3d_sample(s, slot, u, v, w, out);
}
extern "C" __attribute__((always_inline))
void __tex2darray_sample_lod(void* s, int slot, float u, float v, float layer,
                             float /*lod*/, float* out) {
    __tex2darray_sample(s, slot, u, v, layer, out);
}

extern "C" __attribute__((always_inline))
void __image2d_load(void* /*image*/, int slot, int x, int y, float* out) {
    const ImgSlot& im = g_img[(unsigned)slot > 7u ? 0 : slot];
    if (!im.data || x < 0 || y < 0 || x >= im.w || y >= im.h) {
        out[0] = out[1] = out[2] = out[3] = 0.f; return;
    }
    const float* p = im.data + (y * im.w + x) * 4;
    for (int c = 0; c < 4; ++c) out[c] = p[c];
}

extern "C" __attribute__((always_inline))
void __image2d_store(void* /*image*/, int slot, int x, int y, float* val) {
    ImgSlot& im = g_img[(unsigned)slot > 7u ? 0 : slot];
    if (!im.data || x < 0 || y < 0 || x >= im.w || y >= im.h) return;
    float* p = im.data + (y * im.w + x) * 4;
    for (int c = 0; c < 4; ++c) p[c] = val[c];
}

extern "C" __attribute__((always_inline))
void __imagebuffer_load(void* /*image*/, int slot, int i, float* out) {
    const ImgSlot& im = g_img[(unsigned)slot > 7u ? 0 : slot];
    if (!im.data || i < 0 || i >= im.w) {
        out[0] = out[1] = out[2] = out[3] = 0.f; return;
    }
    const float* p = im.data + i * 4;
    for (int c = 0; c < 4; ++c) out[c] = p[c];
}

extern "C" __attribute__((always_inline))
void __imagebuffer_store(void* /*image*/, int slot, int i, float* val) {
    ImgSlot& im = g_img[(unsigned)slot > 7u ? 0 : slot];
    if (!im.data || i < 0 || i >= im.w) return;
    float* p = im.data + i * 4;
    for (int c = 0; c < 4; ++c) p[c] = val[c];
}
