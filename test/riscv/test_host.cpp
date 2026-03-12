#include <vector>
#include <cmath>
#include <chrono>
#include <iostream>
#include <fstream>
#include <algorithm>
struct Vec3Uniform {
    float x, y, z;
    float _pad;
};

struct Light {
    Vec3Uniform position;   // -> <3 x float> (16B)
    Vec3Uniform color;      // -> <3 x float> (16B)
    float intensity;        // -> float       (4B)
};

struct Material {
    Vec3Uniform color;      // -> <3 x float> (16B)
    float roughness;        // -> float       (4B)
    float metallic;         // -> float       (4B)
};


extern "C" {
    void shade_wrapper(float u, float v, float *out);
}

int main() {
    std::cout << "Testing shader with static linking...\n";

    static constexpr int W = 2048;
    static constexpr int H = 2048;

    std::vector<unsigned char> img(W * H * 3);
    float rgba[4];

    auto start = std::chrono::high_resolution_clock::now();

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float u = (x + 0.5f) / W;
            float v = (y + 0.5f) / H;

            shade_wrapper(u, v, rgba);

            int i = (y * W + x) * 3;

            img[i + 0] = static_cast<unsigned char>(255 * std::clamp(rgba[0], 0.0f, 1.0f));
            img[i + 1] = static_cast<unsigned char>(255 * std::clamp(rgba[1], 0.0f, 1.0f));
            img[i + 2] = static_cast<unsigned char>(255 * std::clamp(rgba[2], 0.0f, 1.0f));
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "Render time: " << ms << " ms\n";

    const char* outpath = "result/shader_out.ppm";
    std::ofstream f(outpath, std::ios::binary);
    if (!f) {
        std::cerr << "Cannot open " << outpath << " for writing\n";
        return 1;
    }
    f << "P6\n" << W << " " << H << "\n255\n";
    f.write(reinterpret_cast<char*>(img.data()), img.size());
    std::cout << "Image written to " << outpath << "\n";
    return 0;
}
