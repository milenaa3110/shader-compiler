#include <vector>
    #include <cmath>
    #include <chrono>
    #include <iostream>
    #include <fstream>
    #include <algorithm>

    extern "C" void shade_wrapper(float u, float v, float *out);

    // Mock uniforms - u pravom sistemu bi se ovi slali preko UBO
    float g_time = 0.0f;
    float g_cameraPos[3] = {0.0f, 0.0f, -5.0f};
    float g_lightPos[3] = {2.0f, 2.0f, -2.0f};

    void render_frame(int W, int H, std::vector<unsigned char>& img, float time) {
        g_time = time;
        float rgba[4];

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
    }

    void benchmark(int W, int H, int frames) {
        std::vector<unsigned char> img(W * H * 3);

        auto start = std::chrono::high_resolution_clock::now();

        for (int f = 0; f < frames; ++f) {
            render_frame(W, H, img, f * 0.016f); // ~60fps
        }

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        std::cout << "Rendered " << frames << " frames in " << ms << " ms\n";
        std::cout << "Average: " << (ms / frames) << " ms/frame\n";
        std::cout << "FPS: " << (1000.0 * frames / ms) << "\n";
    }

    int main() {
        static constexpr int W = 1024;
        static constexpr int H = 1024;

        std::vector<unsigned char> img(W * H * 3);

        // Single frame render
        render_frame(W, H, img, 0.0f);

        std::ofstream f("result/advanced_out.ppm", std::ios::binary);
        f << "P6\n" << W << " " << H << "\n255\n";
        f.write(reinterpret_cast<char*>(img.data()), img.size());
        std::cout << "Saved result/advanced_out.ppm\n";

        // Benchmark
        benchmark(512, 512, 100);

        return 0;
    }