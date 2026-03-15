// rv_host_fragment.cpp — generic RISC-V animation benchmark host.
// Compile with -DANIM_NAME="mandelbrot" -DNFRAMES=60 etc.
// The shader object file (built from irgen_riscv) is linked in.
// Compile with -fopenmp to parallelize pixel rendering across CPU cores.
//
// Usage (from Makefile):
//   riscv64-linux-gnu-g++ -O3 -fopenmp -DANIM_NAME="mandelbrot" -DNFRAMES=60 \
//       -Ipipeline rv_host_fragment.cpp pipeline/pipeline_runtime.cpp \
//       mandelbrot_riscv.o -o mandelbrot.rv
//   OMP_NUM_THREADS=8 qemu-riscv64-static -L /usr/riscv64-linux-gnu ./mandelbrot.rv

#include "../../pipeline/pipeline_runtime.h"
#include "../../pipeline/pipeline_abi.h"

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
#define ANIM_NAME "anim"
#endif
#ifndef NFRAMES
#define NFRAMES 60
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
#ifndef VERT_COUNT
#define VERT_COUNT 6
#endif

int main() {
    constexpr int W = WIDTH, H = HEIGHT;
    mkdir("result", 0755);
    std::vector<unsigned char> img(W * H * 3);
    PipelineDesc desc{W, H, /*vert_count=*/VERT_COUNT};
    double total_ms = 0.0;

    // ffmpeg pipe opened lazily on the first frame — if the process is killed
    // before any frame is rendered, no empty MP4 file is left on disk.
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
            if (!ffpipe) { std::cerr << "Cannot open ffmpeg pipe\n"; return 1; }
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
