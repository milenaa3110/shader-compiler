// rv_host_volume_cs.cpp — RISC-V host for the compute variant of the volumetric
// demo. Where rv_host_volume.cpp drives volume_fs through render_pipeline (VS →
// raster → FS), this one dispatches volume_cs directly: one cs_dispatch_row per
// scanline, OpenMP across rows, and the result read back out of the storage
// image the shader wrote through imageStore.
//
// The volume itself comes from the shared bake in test/volume_data.h, so this
// and the fragment path sample identical data.
//
// Build (see CMakeLists.txt): reduced NON-inlining opt pipeline, same as the
// other layered-sampler shaders — `opt -O3` miscompiles inlined non-2D sampler
// bodies on the RISC-V backend (LLVM-18).

// pipeline_runtime.cpp is deliberately NOT linked here: it defines
// render_pipeline, which references vs_invoke/fs_invoke and the stage layout
// globals that a pure compute module does not emit. The only things this host
// needs from it are the sampler/image slot tables that tex_inline.cpp resolves
// against, so it defines those directly instead of dragging in the rasterizer.
#include "../../src/runtime/tex_inline.h"
#include "../../src/common/error_utils.h"
#include "../volume_data.h"

TexSlot g_tex[8] = {};
ImgSlot g_img[8] = {};

#include <vector>
#include <iostream>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#ifdef _OPENMP
#include <omp.h>
#endif

// Uniforms and the dispatch trampoline, both emitted into the shader module.
extern "C" float uTime;
extern "C" uint32_t uW;
extern "C" uint32_t uH;
extern "C" void cs_dispatch_row(uint32_t y, uint32_t width);

#ifndef ANIM_NAME
#define ANIM_NAME "volume_cs"
#endif
#ifndef NFRAMES
#define NFRAMES 90
#endif
#ifndef WIDTH
#define WIDTH 256
#endif
#ifndef HEIGHT
#define HEIGHT 256
#endif
#ifndef FPS
#define FPS 30
#endif
#ifndef BENCH_ONLY
#define BENCH_ONLY 0
#endif

static constexpr int N = voldata::kN;

int main() {
    constexpr int W = WIDTH, H = HEIGHT;
    mkdir("result", 0755);

    auto tv0 = std::chrono::high_resolution_clock::now();
    std::vector<float> vol = voldata::make(N);
    auto tv1 = std::chrono::high_resolution_clock::now();
    std::cout << "[" ANIM_NAME "] baked " << N << "^3 volume in "
              << std::chrono::duration<double, std::milli>(tv1 - tv0).count()
              << " ms\n";

    // Storage image the shader writes through imageStore, RGBA float.
    std::vector<float> out((size_t)W * H * 4, 0.f);
    g_tex[0] = {vol.data(), N, N, N};   // uVol (binding 0)
    g_img[1] = {out.data(), W, H};      // uOut (binding 1)
    uW = (uint32_t)W;
    uH = (uint32_t)H;

    std::vector<unsigned char> rgb((size_t)W * H * 3);
    double total_ms = 0.0;

#if !BENCH_ONLY
    char ff_cmd[512];
    std::snprintf(ff_cmd, sizeof(ff_cmd),
        "ffmpeg -y -f rawvideo -pixel_format rgb24 -video_size %dx%d "
        "-framerate %d -i pipe:0 "
        "-c:v libx264 -pix_fmt yuv420p -crf 20 result/" ANIM_NAME "_rv.mp4 2>/dev/null",
        W, H, FPS);
    FILE* ffpipe = nullptr;
#endif

    for (int frame = 0; frame < NFRAMES; ++frame) {
        uTime = frame / (float)FPS;

        auto t0 = std::chrono::high_resolution_clock::now();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (int y = 0; y < H; ++y)
            cs_dispatch_row((uint32_t)y, (uint32_t)W);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total_ms += ms;

#if !BENCH_ONLY
        for (int i = 0; i < W * H; ++i) {
            const float* p = &out[(size_t)i * 4];
            for (int c = 0; c < 3; ++c) {
                float v = p[c] < 0.f ? 0.f : (p[c] > 1.f ? 1.f : p[c]);
                rgb[(size_t)i * 3 + c] = (unsigned char)(v * 255.f + 0.5f);
            }
        }
        if (!ffpipe) {
            ffpipe = popen(ff_cmd, "w");
            if (!ffpipe) { logError("Cannot open ffmpeg pipe"); return 1; }
        }
        std::fwrite(rgb.data(), 1, rgb.size(), ffpipe);
#endif
        std::cout << "[" ANIM_NAME "] frame " << frame
                  << " t=" << uTime << "  " << ms << " ms\n";
    }

#if !BENCH_ONLY
    if (ffpipe) pclose(ffpipe);
#endif

    double avg = total_ms / NFRAMES;
    std::cout << "[" ANIM_NAME "] RISC-V avg: " << avg << " ms/frame  ("
              << (1000.0 / avg) << " fps simulated)\n";
#if !BENCH_ONLY
    std::cout << "[" ANIM_NAME "] MP4: result/" ANIM_NAME "_rv.mp4\n";
#endif
    return 0;
}
