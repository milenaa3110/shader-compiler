#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Interpolated varyings (VS out -> FS in) are f32-only due to rasterizer constraints.
// Vertex inputs and Fragment outputs allow full 64-bit doubles, handled via a
// separate double buffer region to prevent alignment padding overhead.
// Inside each region, scalars are packed sequentially in declaration order.

// Invoked per vertex by the soft-rasterizer to process geometry data.
//   flat_in    - vs_input_floats elements (32-bit vertex attributes)
//   flat_in_d  - vs_input_doubles elements (64-bit vertex attributes)
//   flat_out   - vs_total_floats elements: [0..3] = gl_Position, [4..] = varyings
void vs_invoke(int vid, int iid, float* flat_in, double* flat_in_d, float* flat_out);

// Invoked per fragment by the soft-rasterizer to calculate pixel shading.
//   fragcoord  - 4 elements layout: window-space (x, y, z, 1/w)
//   varyings   - vs_varying_floats elements (perspective-correct interpolated, f32-only)
//   flat_out   - fs_output_floats elements (e.g. RGBA color output)
//   flat_out_d - fs_output_doubles elements (64-bit fragment outputs)
void fs_invoke(float* fragcoord, float* varyings, float* flat_out, double* flat_out_d);

// Pipeline layout metrics emitted as global LLVM i32 constants
extern int vs_total_floats;    // Size of gl_Position(4) + all f32 varyings
extern int vs_varying_floats;  // Size of f32 varyings only (interpolated region)
extern int vs_input_floats;    // 32-bit vertex attribute scalar slots
extern int vs_input_doubles;   // 64-bit vertex attribute scalar slots
extern int fs_output_floats;   // 32-bit fragment shader output scalar slots
extern int fs_output_doubles;  // 64-bit fragment shader output scalar slots

#ifdef __cplusplus
}
#endif