// rv_host_compute.cpp — Conway's Game of Life CPU benchmark (RISC-V + OpenMP).
//
// Dispatches the compiled life_cs.src shader via cs_invoke() per invocation,
// parallelized with OpenMP.  The shader reads/writes the src/dst globals set
// below before each generation.
//
// Compare with spirv_vulkan_life_host: same shader logic, but CPU pays zero
// submission cost and dispatches with OpenMP instead of Vulkan compute.
//
// Compile (invoked by run_benchmark_compute.sh):
//   riscv64-linux-gnu-g++ -std=c++20 -O3 -static -fopenmp \
//       -DGRID=256 -DNGENERATIONS=1000 \
//       test/rv_host/rv_host_compute.cpp build/riscv/life_cs_rv.o -o life.rv
// Run:
//   OMP_NUM_THREADS=$(nproc) qemu-riscv64-static -L /usr/riscv64-linux-gnu ./life.rv

#include <vector>
#include <iostream>
#include <fstream>
#include <chrono>
#include <random>
#include <cstring>
#include <sys/stat.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#ifndef GRID
#define GRID 256
#endif
#ifndef NGENERATIONS
#define NGENERATIONS 1000
#endif
#ifndef SNAP_EVERY
#define SNAP_EVERY 0   // 0 = no animation; N = save a frame every N generations
#endif

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
// Row dispatcher emitted by emit_trampolines into life_cs_rv.o.
// Loops over X internally so llc can inline cs_main and vectorise with RVV.
extern "C" void cs_dispatch_row(uint32_t y, uint32_t width);
// ─────────────────────────────────────────────────────────────────────────────

static void writePPM(const char* path, int W, int H, const uint32_t* cells) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << W << " " << H << "\n255\n";
    for (int i = 0; i < W * H; i++) {
        uint8_t v = cells[i] ? 255 : 0;
        f.write(reinterpret_cast<const char*>(&v), 1);
        f.write(reinterpret_cast<const char*>(&v), 1);
        f.write(reinterpret_cast<const char*>(&v), 1);
    }
}

int main() {
    constexpr int W = GRID, H = GRID, N = NGENERATIONS;
    std::cout << "Game of Life: " << W << "x" << H
              << " grid, " << N << " generations\n";
#ifdef _OPENMP
    std::cout << "OpenMP threads: " << omp_get_max_threads() << "\n";
#endif

    std::vector<uint32_t> curBuf(W * H), nxtBuf(W * H);

    // Seed with ~30% alive (same seed as GPU host)
    std::mt19937 rng(42);
    for (int i = 0; i < W * H; i++)
        curBuf[i] = (rng() % 10 < 3) ? 1u : 0u;

    mkdir("result", 0755);
    if (SNAP_EVERY > 0)
        std::cout << "[life-cpu] Animation mode: saving frame every "
                  << SNAP_EVERY << " generations\n";

    // Set push constants once (grid size doesn't change)
    width  = (uint32_t)W;
    height = (uint32_t)H;

    auto t0 = std::chrono::high_resolution_clock::now();
    int frameIdx = 0;

    for (int gen = 0; gen < N; gen++) {
        src = (void*)curBuf.data();
        dst = (void*)nxtBuf.data();

        #pragma omp parallel for schedule(static)
        for (int y = 0; y < H; y++)
            cs_dispatch_row((uint32_t)y, (uint32_t)W);

        std::swap(curBuf, nxtBuf);

        if constexpr (SNAP_EVERY > 0) if (gen % SNAP_EVERY == 0) {
            char path[256];
            std::snprintf(path, sizeof(path), "result/life_cpu_%04d.ppm", frameIdx++);
            writePPM(path, W, H, curBuf.data());
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double per_gen  = total_ms / N;
    double mpx_ms   = (double)W * H / 1e6 / per_gen;

    std::cout << "[life-cpu] " << N << " generations in " << total_ms << " ms\n";
    std::cout << "[life-cpu] avg: " << per_gen << " ms/gen"
              << "  (" << 1000.0 / per_gen << " gen/s)"
              << "  " << mpx_ms << " Mpx/ms\n";

    // Save final state (only in non-animation mode)
    if (SNAP_EVERY == 0) {
        writePPM("result/life_cpu.ppm", W, H, curBuf.data());
        std::cout << "[life-cpu] Final state: result/life_cpu.ppm\n";
    }

    // Encode MP4 if animation frames were saved, then delete PPMs
    if (SNAP_EVERY > 0 && frameIdx > 1) {
        char cmd_str[512];
        std::snprintf(cmd_str, sizeof(cmd_str),
            "ffmpeg -y -framerate 30 -i result/life_cpu_%%04d.ppm "
            "-c:v libx264 -pix_fmt yuv420p -vf scale=%d:%d -crf 18 "
            "result/life_cpu.mp4 2>/dev/null", W * 4, H * 4);
        if (std::system(cmd_str) == 0) {
            std::cout << "[life-cpu] Animation: result/life_cpu.mp4\n";
            for (int i = 0; i < frameIdx; i++) {
                char path[256];
                std::snprintf(path, sizeof(path), "result/life_cpu_%04d.ppm", i);
                std::remove(path);
            }
        }
    }
    return 0;
}
