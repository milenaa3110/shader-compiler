// pipeline_host.cpp — test driver for the software pipeline rasterizer.
// Links against a compiled VS+FS pipeline module and pipeline_runtime.

#include "pipeline_runtime.h"
#include "pipeline_abi.h"

#include <vector>
#include <fstream>
#include <iostream>
#include <chrono>
#include <sys/stat.h>

static void write_ppm(const char* path, int W, int H, const unsigned char* rgb) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { std::cerr << "Cannot open " << path << "\n"; return; }
    f << "P6\n" << W << " " << H << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb), W * H * 3);
    std::cout << "Image written to " << path << "\n";
}

int main() {
    static constexpr int W = 1024;
    static constexpr int H = 1024;

    std::vector<unsigned char> img(W * H * 3);

    PipelineDesc desc{W, H, /*vert_count=*/3};

    auto t0 = std::chrono::high_resolution_clock::now();
    render_pipeline(desc, img.data());
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "Pipeline render time: " << ms << " ms\n";

    mkdir("result", 0755);
    write_ppm("result/pipeline_out.ppm", W, H, img.data());
    return 0;
}