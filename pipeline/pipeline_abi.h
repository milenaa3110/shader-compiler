#pragma once
// pipeline_abi.h — stable C ABI between the software rasterizer and a compiled
// VS+FS pipeline module.  The functions and globals below are emitted by irgen
// into every stage-entry module.

#ifdef __cplusplus
extern "C" {
#endif

// Called once per vertex by the rasterizer.
//   vid        — gl_VertexID
//   iid        — gl_InstanceID  (usually 0)
//   flat_out   — output array of vs_total_floats floats
//                  [0..3]  = gl_Position (xyzw clip-space)
//                  [4..]   = varyings in declaration order
void vs_invoke(int vid, int iid, float* flat_out);

// Called once per fragment by the rasterizer.
//   fragcoord  — 4 floats: window-space (x, y, z, 1/w)
//   varyings   — vs_varying_floats floats (perspective-correct interpolated)
//   flat_out   — fs_output_floats floats (e.g. RGBA FragColor)
void fs_invoke(float* fragcoord, float* varyings, float* flat_out);

// Layout constants (emitted as LLVM global i32 constants)
extern int vs_total_floats;    // gl_Position(4) + all out-vars
extern int vs_varying_floats;  // all out-vars only (interpolated by rasterizer)
extern int fs_output_floats;   // floats in FS_Output (e.g. 4 for vec4 FragColor)

#ifdef __cplusplus
}
#endif