// rv_host_volume.cpp — RISC-V animation host for the volumetric sampler3D demo.
//
// Differs from rv_host_fragment.cpp only in that it has something to bind: it
// bakes a tileable RGBA volume once and hands it to bind_texture_layered(),
// which is what puts volume_fs's texture(sampler3D, vec3) on the __tex3d_sample
// path. Everything after that (uTime per frame, render, ffmpeg pipe) matches the
// generic host.
//
// The volume is baked, not procedural-in-shader, on purpose: it makes the frame
// cost a trilinear 3D fetch per march step rather than an FBM evaluation, which
// is the thing worth measuring here.
//
// Build (see CMakeLists.txt): the shader object uses the reduced NON-inlining
// opt pipeline — `opt -O3` miscompiles inlined non-2D sampler bodies on the
// RISC-V backend (same LLVM-18 issue documented for texlayer).

#include "../../src/runtime/pipeline_runtime.h"
#include "../../src/common/error_utils.h"
#include "../volume_data.h"

#include <vector>
#include <iostream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#ifdef _OPENMP
#include <omp.h>
#endif

// uTime uniform (defined in the FS LLVM module)
extern "C" float uTime;

#ifndef ANIM_NAME
#define ANIM_NAME "volume"
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
static constexpr int N = voldata::kN;

int main() {
    report_vector_config();
    constexpr int W = WIDTH, H = HEIGHT;
    mkdir("result", 0755);

    // ── Bake the volume (shared with the Vulkan host) ────────────────────────
    auto tv0 = std::chrono::high_resolution_clock::now();
    std::vector<float> vol = voldata::make(N);
    auto tv1 = std::chrono::high_resolution_clock::now();
    std::cout << "[" ANIM_NAME "] baked " << N << "^3 volume in "
              << std::chrono::duration<double, std::milli>(tv1 - tv0).count()
              << " ms\n";

    bind_texture_layered(0, vol.data(), N, N, N);   // uVol (binding 0)

    // ── Render ───────────────────────────────────────────────────────────────
    std::vector<unsigned char> img(W * H * 3);
    PipelineDesc desc{W, H, /*vert_count=*/6,
                      /*vbuf=*/nullptr, /*indices=*/nullptr, /*index_count=*/0};
    double total_ms = 0.0;

    char ff_cmd[512];
    std::snprintf(ff_cmd, sizeof(ff_cmd),
        "ffmpeg -y -f rawvideo -pixel_format rgb24 -video_size %dx%d "
        "-framerate %d -i pipe:0 "
        "-c:v libx264 -pix_fmt yuv420p -crf 20 result/" ANIM_NAME "_rv.mp4 2>/dev/null",
        W, H, FPS);
    FILE* ffpipe = nullptr;

    for (int frame = 0; frame < NFRAMES; ++frame) {
        uTime = frame / (float)FPS;

        auto t0 = std::chrono::high_resolution_clock::now();
        render_pipeline(desc, img.data());
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total_ms += ms;

        if (!ffpipe) {
            ffpipe = popen(ff_cmd, "w");
            if (!ffpipe) { logError("Cannot open ffmpeg pipe"); return 1; }
        }
        std::fwrite(img.data(), 1, W * H * 3, ffpipe);
        std::cout << "[" ANIM_NAME "] frame " << frame
                  << " t=" << uTime << "  " << ms << " ms\n";
    }

    if (ffpipe) pclose(ffpipe);

    double avg = total_ms / NFRAMES;
    std::cout << "[" ANIM_NAME "] RISC-V avg: " << avg << " ms/frame  ("
              << (1000.0 / avg) << " fps simulated)\n";
    std::cout << "[" ANIM_NAME "] MP4: result/" ANIM_NAME "_rv.mp4\n";
    return 0;
}
