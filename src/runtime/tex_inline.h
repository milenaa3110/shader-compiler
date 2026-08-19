// tex_inline.h — Shared declarations for the inline sampler/image module.
// Links directly into compiled IR shader modules on the RISC-V target backend.

#pragma once

// A sampled texture slot. `depth` carries the third dimension so one slot can
// hold a plain 2D image (depth 1), a cube map (6 faces), a 2D array (N layers),
// or a 3D volume (N slices). Slice s occupies data[s*w*h*4 .. (s+1)*w*h*4).
struct TexSlot {
    const float* data;  // flat RGBA, w*h*depth*4 floats
    int          w;     // width in texels
    int          h;     // height in texels
    int          depth; // faces / layers / volume slices (1 for a plain 2D texture)
};

// A read/write storage-image slot (image2D / imageBuffer).
struct ImgSlot {
    float* data;  // flat RGBA, w*h*4 floats (imageBuffer: h == 1)
    int    w;
    int    h;
};

// Sampled-texture and storage-image slot tables, defined in pipeline_runtime.cpp
// and resolved at link time.
extern TexSlot g_tex[8];
extern ImgSlot g_img[8];
