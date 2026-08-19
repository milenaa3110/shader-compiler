// rv_host_texlayer.cpp — exercises the layered-texture AND storage-image runtime
// end-to-end on the RISC-V RENDER path by binding through the public APIs that no
// other host called: bind_texture_layered() (cube / 3D / array), bind_texture()
// (plain 2D), and bind_image() (read/write image2D). It renders texlayer_fs over a
// full-screen quad, then checks both the pixel color and the image round-trip.
//
// Each sampler is bound SOLID (same value on every face/slice/layer) so the
// rendered color is independent of coordinate precision; selection correctness is
// covered by the sampler_numeric gate. Expected sum, per channel:
//   R = cube(0.3) + tex(0.2) = 0.5 → 128    G = vol(0.25) → 64    B = arr(0.75) → 191
// (framebuffer byte = clamp01(v)*255 + 0.5). The shader also does
// imageStore(uImg,(3,4), imageLoad(uImg,(1,2)) + 0.5), so texel(3,4) == (1,2)+0.5.
// Renders in scalar mode.
//
// Build (see CMakeLists.txt): the shader object is built at a REDUCED opt pipeline
// (no inliner) because `opt -O3` miscompiles inlined multi-sampler code on the
// RISC-V backend — see the CMake comment. Returns 0 when both checks pass.

#include "../../src/runtime/pipeline_runtime.h"   // bind_texture(_layered), bind_image, render_pipeline, PipelineDesc
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

int main() {
    constexpr int W = 32, H = 32;

    // Solid layered inputs — 1x1 per face/slice/layer.
    float cube[6 * 4] = {0}; for (int f = 0; f < 6; ++f) cube[f * 4 + 0] = 0.3f;  // R on every face
    float vol[4 * 4]  = {0}; for (int s = 0; s < 4; ++s) vol[s * 4 + 1]  = 0.25f; // G on every slice
    float arr[3 * 4]  = {0}; for (int l = 0; l < 3; ++l) arr[l * 4 + 2]  = 0.75f; // B on every layer
    float tex[4]      = {0.2f, 0.f, 0.f, 0.f};                                    // R on the 2D texel

    // Read/write storage image (8x8 RGBA); seed the source texel (1,2).
    constexpr int IW = 8, IH = 8;
    std::vector<float> imgTex(IW * IH * 4, 0.f);
    float* t12 = &imgTex[(2 * IW + 1) * 4];
    t12[0] = 1.f; t12[1] = 2.f; t12[2] = 3.f; t12[3] = 4.f;

    // The wiring that was previously dead: bind through the public APIs.
    bind_texture_layered(0, cube, 1, 1, 6);   // uCube (binding 0)
    bind_texture_layered(1, vol,  1, 1, 4);   // uVol  (binding 1)
    bind_texture_layered(2, arr,  1, 1, 3);   // uArr  (binding 2)
    bind_texture(3, tex, 1, 1);               // uTex  (binding 3)
    bind_image(4, imgTex.data(), IW, IH);     // uImg  (binding 4)

    std::vector<unsigned char> img(W * H * 3, 0);
    PipelineDesc desc{W, H, /*vert_count=*/6, /*vbuf=*/nullptr,
                      /*indices=*/nullptr, /*index_count=*/0};   // scene_vs synthesizes the quad
    render_pipeline(desc, img.data());   // scalar mode: SHADER_PACKET unset

    int bad = 0;
    auto chkb = [&](const char* w, int got, int want) {
        if (std::abs(got - want) > 1) {   // ±1 for the round-to-nearest byte conversion
            std::printf("    MISMATCH %-8s got %d want %d\n", w, got, want); bad = 1;
        }
    };
    auto chkf = [&](const char* w, float got, float want) {
        if (std::fabs(got - want) > 1e-3f) {
            std::printf("    MISMATCH %-8s got %.4f want %.4f\n", w, got, want); bad = 1;
        }
    };

    // The whole quad is one solid color; check the center pixel.
    const unsigned char* p = &img[((H / 2) * W + (W / 2)) * 3];
    int r = p[0], g = p[1], b = p[2];
    chkb("pix.R", r, 128); chkb("pix.G", g, 64); chkb("pix.B", b, 191);

    // Storage-image round-trip: texel(3,4) == texel(1,2) + 0.5.
    const float* t34 = &imgTex[(4 * IW + 3) * 4];
    chkf("img.r", t34[0], 1.5f); chkf("img.g", t34[1], 2.5f);
    chkf("img.b", t34[2], 3.5f); chkf("img.a", t34[3], 4.5f);

    if (bad) { std::printf("[texlayer-rv] FAIL (pixel R=%d G=%d B=%d)\n", r, g, b); return 1; }
    std::printf("[texlayer-rv] PASS: bind_texture_layered (cube/3D/array) + bind_texture + "
                "bind_image round-trip (pixel R=%d G=%d B=%d)\n", r, g, b);
    return 0;
}
