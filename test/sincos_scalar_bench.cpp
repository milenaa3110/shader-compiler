#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" void fs_invoke(float*, float*, float*, double*);
extern "C" float uTime;

int main(int argc, char** argv) {
    int n = argc > 1 ? std::atoi(argv[1]) : 100000;
    int reps = argc > 2 ? std::atoi(argv[2]) : 20;
    std::vector<float> out(4 * n);
    volatile double sink = 0;
    for (int warm = 0; warm < 3; ++warm)
        for (int i = 0; i < n; ++i) {
            float frag[4] = {float(i % 1024), float((i * 17) % 1024), 0, 1};
            float varying[2] = {float(i % 1000) / 1000.0f, float((i * 13) % 1000) / 1000.0f};
            double dout[1] = {};
            fs_invoke(frag, varying, out.data() + 4 * i, dout);
        }
    auto begin = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r) {
        uTime = 1.7f + 0.01f * r;
        for (int i = 0; i < n; ++i) {
            float frag[4] = {float(i % 1024), float((i * 17) % 1024), 0, 1};
            float varying[2] = {float(i % 1000) / 1000.0f, float((i * 13) % 1000) / 1000.0f};
            double dout[1] = {};
            fs_invoke(frag, varying, out.data() + 4 * i, dout);
        }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / reps;
    for (float x : out) sink += x;
    std::printf("%.9f %.9f\n", ms, double(sink));
}
