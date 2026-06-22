// tex_inline.cpp — bilinear sampler emitted as LLVM bitcode and linked into
// shader IR before opt -O3. Both functions are marked `always_inline` so
// `opt` substitutes their bodies into every fragment-shader call site —
// eliminating the per-fragment cross-TU function-call overhead that the
// scalar pipeline_runtime.cpp version was paying.
//
// This file is NOT compiled into pipeline_runtime.o. It's compiled with
// clang-18 to a standalone .bc file (see CMakeLists.txt), then llvm-link'd
// into each shader module after irgen.

#include "tex_inline.h"

#include <cmath>

extern "C" __attribute__((always_inline))
void __tex_lookup(int slot, float u, float v, float* out) {
    const TexSlot& t = g_tex[slot < 0 || slot > 7 ? 0 : slot];
    if (!t.data) {
        out[0] = out[1] = out[2] = out[3] = 0.f;
        return;
    }

    u -= std::floor(u);
    v -= std::floor(v);

    float fx = u * (t.w - 1);
    float fy = v * (t.h - 1);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1; if (x1 > t.w - 1) x1 = t.w - 1;
    int y1 = y0 + 1; if (y1 > t.h - 1) y1 = t.h - 1;
    float tx = fx - x0, ty = fy - y0;

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

extern "C" __attribute__((always_inline))
void __tex2d_sample(void* /*sampler*/, float u, float v, float* out) {
    // Single-texture demos always sample from slot 0; the sampler pointer
    // is ignored. Always-inline causes the call below to inline as well,
    // so the entire sample becomes ~30 instructions of straight-line math
    // at every shader call site.
    __tex_lookup(0, u, v, out);
}
