// rv_host_compute_blur.cpp — CPU-side Gaussian blur benchmark (RISC-V + OpenMP).
// Dispatches the compiled blur_cs.src shader via cs_invoke() per invocation,
// parallelized with OpenMP.  Shows CPU thread-parallel vs GPU compute-shader.
//
// Compile for RISC-V:
//   riscv64-linux-gnu-g++ -std=c++20 -O3 -static -fopenmp \
//       -DWIDTH=512 -DHEIGHT=512 -DNRUNS=100 \
//       test/rv_host/rv_host_compute_blur.cpp build/riscv/blur_cs_rv.o -o blur.rv
//
// Run:
//   OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu ./blur.rv

#include <vector>
#include <fstream>
#include <iostream>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <sys/stat.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#ifndef WIDTH
#define WIDTH 512
#endif
#ifndef HEIGHT
#define HEIGHT 512
#endif
#ifndef NRUNS
#define NRUNS 100
#endif

static constexpr int W = WIDTH, H = HEIGHT;

// ── Shader ABI ────────────────────────────────────────────────────────────────
// src / dst are external declarations in the shader module; host provides the
// definitions so the linker can resolve them.
extern "C" {
    void* src;
    void* dst;
}
// width / height are defined in the shader module (zero-initialized globals);
// the host sets them once before the first dispatch.
extern "C" uint32_t width;
extern "C" uint32_t height;
// Row dispatcher emitted by emit_trampolines into blur_cs_rv.o.
// Loops over X internally so llc can inline cs_main and vectorise with RVV.
extern "C" void cs_dispatch_row(uint32_t y, uint32_t width);
// ─────────────────────────────────────────────────────────────────────────────

static void writePPM(const char* path, const std::vector<float>& rgba) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << W << " " << H << "\n255\n";
    for (int i = 0; i < W * H; i++) {
        auto c = [](float v) -> uint8_t {
            return (uint8_t)((v < 0.f ? 0.f : v > 1.f ? 1.f : v) * 255.f);
        };
        uint8_t rgb[3] = {c(rgba[i*4]), c(rgba[i*4+1]), c(rgba[i*4+2])};
        f.write((char*)rgb, 3);
    }
}

int main() {
    mkdir("result", 0755);

    // Generate same synthetic noise as GPU host
    std::vector<float> inBuf(W * H * 4), outBuf(W * H * 4);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float u = (float)x / W, v = (float)y / H;
            int i = (y * W + x) * 4;
            inBuf[i+0] = 0.5f + 0.5f * sinf(u*31.4f + v*25.1f) * cosf(v*18.8f - u*12.6f);
            inBuf[i+1] = 0.5f + 0.5f * sinf(u*17.3f + v*22.7f);
            inBuf[i+2] = 0.5f + 0.5f * cosf(u*28.2f - v*15.4f);
            inBuf[i+3] = 1.f;
        }
    }

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = omp_get_max_threads();
#endif

    std::cout << "[blur] CPU RISC-V+OpenMP: " << W << "x" << H
              << " Gaussian blur, " << NRUNS << " runs, " << nthreads << " threads\n";

    // Set push constants once
    width  = (uint32_t)W;
    height = (uint32_t)H;
    src    = (void*)inBuf.data();
    dst    = (void*)outBuf.data();

    // Warmup
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < H; y++)
        cs_dispatch_row((uint32_t)y, (uint32_t)W);

    double total_ms = 0.0;
    for (int run = 0; run < NRUNS; run++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        #pragma omp parallel for schedule(static)
        for (int y = 0; y < H; y++)
            cs_dispatch_row((uint32_t)y, (uint32_t)W);
        auto t1 = std::chrono::high_resolution_clock::now();
        total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    double avg = total_ms / NRUNS;
    std::cout << "[blur] CPU avg: " << avg << " ms/run  ("
              << (1000.0 / avg) << " runs/s)\n";
    std::cout << "[blur] Throughput: "
              << (W * H / avg / 1000.0) << " Mpixels/ms\n";

    writePPM("result/blur_cpu.ppm", outBuf);
    std::cout << "[blur] Output: result/blur_cpu.ppm\n";
    return 0;
}
