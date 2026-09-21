// vec_math.h — Vector transcendentals for the packet path.
//
// RVV has instructions for sqrt (vfsqrt.v) and for the rounding family, so
// llvm.sqrt/floor.vNf32 lower to single instructions. It has nothing for sin or
// cos, and there is no vector libm on this target, so llc scalarizes
// llvm.sin.vNf32 into W separate sinf calls plus the pack/unpack around them.
// Measured on galaxy at W=4: fs_packet issued 16 sinf + 12 expf + 4 logf to
// shade four pixels, which is exactly what the scalar fs_main issues for the
// same four — the packet path saved no calls at all and paid the shuffles.
//
// These replacements evaluate the polynomial with plain vector arithmetic, so
// all W lanes cost one pass and no libm call is made.
//
// Both are marked always_inline and the module is llvm-link'd into the shader
// IR before opt -O3, the same path tex_inline.bc takes.

#pragma once

#include "../common/packet_width.h"

// Lane count is fixed at build time and must match the emitter's kW; CMake
// passes the same -DSHADER_PACKET_WIDTH to both.
typedef float vfloat __attribute__((ext_vector_type(SHADER_PACKET_WIDTH)));

extern "C" {

// sin/cos over the reduced range, accurate to roughly 1 ulp near zero and
// degrading as |x| grows (see the range note in vec_math.cpp).
vfloat __vsinf(vfloat x);
vfloat __vcosf(vfloat x);

// exp/log. exp saturates rather than producing garbage outside roughly
// [-87, 88]; log returns 0 for x <= 0 instead of -inf/NaN, which matches how
// the shaders use it (always on a positive radius) and keeps the packet path
// free of traps.
vfloat __vexpf(vfloat x);
vfloat __vlogf(vfloat x);

}  // extern "C"
