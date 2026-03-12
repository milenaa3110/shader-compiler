// anim_host.cpp — multi-frame animation driver for RISC-V pipeline tests.
// Renders NFRAMES frames of the animated VS+FS shader by updating uTime,
// writes each frame as result/anim_NNN.ppm, then emits a timing summary.

#include "../../pipeline/pipeline_runtime.h"
#include "../../pipeline/pipeline_abi.h"

#include <vector>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

// uTime is a uniform global emitted by the shader for the anim_vs.src shader
extern "C" float uTime;

static void write_ppm(const char* path, int W, int H, const unsigned char* rgb) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { std::cerr << "Cannot open " << path << "\n"; return; }
    f << "P6\n" << W << " " << H << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb), W * H * 3);
}

int main() {
    static constexpr int W      = 512;
    static constexpr int H      = 512;
    static constexpr int NFRAMES = 16;   // 16 frames @ 1/30s steps

    mkdir("result", 0755);
    std::vector<unsigned char> img(W * H * 3);
    PipelineDesc desc{W, H, /*vert_count=*/6};

    double total_ms = 0.0;

    for (int frame = 0; frame < NFRAMES; ++frame) {
        uTime = frame * (1.0f / 30.0f);

        auto t0 = std::chrono::high_resolution_clock::now();
        render_pipeline(desc, img.data());
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total_ms += ms;

        char path[128];
        std::snprintf(path, sizeof(path), "result/anim_%03d.ppm", frame);
        write_ppm(path, W, H, img.data());
        std::cout << "Frame " << frame << ": " << ms << " ms  → " << path << "\n";
    }

    std::cout << "Total: " << total_ms << " ms for " << NFRAMES << " frames\n";
    std::cout << "Avg  : " << total_ms / NFRAMES << " ms/frame\n";

    // Encode frames to MP4 via ffmpeg
    const char* out_mp4 = "result/anim.mp4";
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -framerate 30 -i result/anim_%%03d.ppm "
        "-c:v libx264 -pix_fmt yuv420p -crf 18 %s 2>/dev/null",
        out_mp4);
    if (std::system(cmd) == 0)
        std::cout << "MP4 written to " << out_mp4 << "\n";
    else
        std::cerr << "ffmpeg encoding failed (is ffmpeg installed?)\n";

    return 0;
}