// A/B benchmark for generated RISC-V fragment shaders, linked into one process.
// The off_ and on_ symbol prefixes isolate all shader state. No LTO is used.
// Timed variants reuse the same output buffers; validation uses separate copies.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>
#include <sched.h>
#include <time.h>

#ifndef SHADER_PACKET_WIDTH
#define SHADER_PACKET_WIDTH 4
#endif

extern "C" {
void off_fs_invoke(float*, float*, float*, double*);
void on_fs_invoke(float*, float*, float*, double*);
void off_fs_packet(const float*, const float*, float*, int*) __attribute__((weak));
void on_fs_packet(const float*, const float*, float*, int*) __attribute__((weak));
extern float off_uTime, on_uTime;
extern const int off___shader_packet_width, on___shader_packet_width;
extern const int off_fs_output_floats, on_fs_output_floats;
extern const int off_fs_output_doubles, on_fs_output_doubles;
}

constexpr int W = SHADER_PACKET_WIDTH;
using Clock = std::chrono::steady_clock;
using Scalar = void (*)(float*, float*, float*, double*);
using Packet = void (*)(const float*, const float*, float*, int*);
static double thread_ms() {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) std::abort();
    return ts.tv_sec * 1000.0 + ts.tv_nsec * 1e-6;
}
static int parse_positive(const char* s) {
    char* end = nullptr;
    long n = std::strtol(s, &end, 10);
    if (!*s || *end || n < 1 || n > 10000000) return -1;
    return static_cast<int>(n);
}
struct Timing { double wall_ms, cpu_ms; };
int main(int argc, char** argv) {
    if (argc != 7) {
        std::fprintf(stderr, "usage: bench scalar|packet rounds fragments sample_ms uTime seed\n");
        return 2;
    }
    const std::string mode = argv[1];
    const bool packet = mode == "packet";
    const int rounds = parse_positive(argv[2]), n = parse_positive(argv[3]);
    const int sample_ms = parse_positive(argv[4]), seed = parse_positive(argv[6]);
    const float timed_uTime = std::strtof(argv[5], nullptr);
    if ((mode != "scalar" && !packet) || rounds < 1 || n < W || n % W ||
        sample_ms < 1 || seed < 1 || !std::isfinite(timed_uTime)) return 2;
    if (off___shader_packet_width != W || on___shader_packet_width != W ||
        off_fs_output_floats != 4 || on_fs_output_floats != 4 ||
        off_fs_output_doubles != 0 || on_fs_output_doubles != 0) {
        std::fprintf(stderr, "shader ABI mismatch\n");
        return 2;
    }
    if (bool(off_fs_packet) != bool(on_fs_packet)) {
        std::fprintf(stderr, "packet entry availability differs\n");
        return 2;
    }
    if (packet && !off_fs_packet) {
        std::puts("{\"kind\":\"skip\",\"reason\":\"packet entry not emitted\"}");
        return 0;
    }
    unsigned long vlenb = 0;
#ifdef __riscv_vector
    __asm__ volatile("csrr %0, vlenb" : "=r"(vlenb));
#endif
    std::printf("{\"kind\":\"configuration\",\"mode\":\"%s\",\"cpu\":%d,"
                "\"packet_width\":%d,\"hardware_vlen_bits\":%lu,\"fragments\":%d,"
                "\"rounds\":%d,\"target_sample_ms\":%d,\"uTime\":%.9g,\"seed\":%d}\n",
                mode.c_str(), sched_getcpu(), W, vlenb * 8, n, rounds,
                sample_ms, timed_uTime, seed);
    std::fflush(stdout);
    std::vector<float> x(n), y(n), output[2] = {
        std::vector<float>(4 * n), std::vector<float>(4 * n)};
    std::vector<int> live[2] = {std::vector<int>(n, 1), std::vector<int>(n, 1)};
    std::mt19937 inputs(123);
    // Explicit conversion gives the same finite inputs with any C++ standard library.
    for (int i = 0; i < n; ++i) {
        x[i] = (inputs() >> 8) * (1.0f / 16777216.0f);
        y[i] = (inputs() >> 8) * (1.0f / 16777216.0f);
    }
    const Scalar scalar_fn[2] = {off_fs_invoke, on_fs_invoke};
    const Packet packet_fn[2] = {off_fs_packet, on_fs_packet};
    bool timing = false;
    auto render = [&](int variant) {
        const int destination = timing ? 0 : variant;
        float* dst = output[destination].data();
        if (!packet) {
            Scalar fn = scalar_fn[variant];
            for (int i = 0; i < n; ++i) {
                float frag[4] = {x[i] * 1024, y[i] * 1024, 0, 1};
                float vary[2] = {x[i], y[i]};
                double doubles[1] = {};
                fn(frag, vary, dst + 4 * i, doubles);
            }
        } else {
            Packet fn = packet_fn[variant];
            for (int i = 0; i < n; i += W) {
                alignas(64) float vary[2 * W], frag[4 * W], out[4 * W];
                alignas(64) int mask[W];
                for (int lane = 0; lane < W; ++lane) {
                    vary[lane] = x[i + lane]; vary[W + lane] = y[i + lane];
                    frag[lane] = x[i + lane] * 1024;
                    frag[W + lane] = y[i + lane] * 1024;
                    frag[2 * W + lane] = 0; frag[3 * W + lane] = 1;
                }
                fn(vary, frag, out, mask);
                for (int lane = 0; lane < W; ++lane) {
                    live[destination][i + lane] = mask[lane] != 0;
                    for (int c = 0; c < 4; ++c)
                        dst[4 * (i + lane) + c] = mask[lane] ? out[c * W + lane] : 0;
                }
            }
        }
    };
    double max_abs = 0, max_rel = 0;
    size_t checked = 0, mismatches = 0, nonfinite = 0;
    for (float t : {0.0f, 1.7f, 5.0f}) {
        off_uTime = on_uTime = t;
        for (auto& out : output) std::fill(out.begin(), out.end(), NAN);
        render(0); render(1);
        for (int i = 0; i < n; ++i) {
            if (live[0][i] != live[1][i]) ++mismatches;
            for (int c = 0; c < 4; ++c) {
                const float a = output[0][4 * i + c], b = output[1][4 * i + c];
                ++checked;
                if (!std::isfinite(a) || !std::isfinite(b)) { ++nonfinite; continue; }
                double error = std::abs(double(a) - double(b));
                double magnitude = std::max(std::abs(double(a)), std::abs(double(b)));
                max_abs = std::max(max_abs, error);
                max_rel = std::max(max_rel, error / std::max(magnitude, 1e-30));
                if (error > 1e-5 + 1e-5 * magnitude) ++mismatches;
            }
        }
    }
    std::printf("{\"kind\":\"validation\",\"components\":%zu,\"max_abs_error\":%.12g,"
                "\"max_rel_error\":%.12g,\"mismatches\":%zu,\"nonfinite\":%zu,"
                "\"absolute_tolerance\":1e-5,\"relative_tolerance\":1e-5}\n",
                checked, max_abs, max_rel, mismatches, nonfinite);
    std::fflush(stdout);
    if (mismatches || nonfinite) return 1;
    off_uTime = on_uTime = timed_uTime;
    timing = true;
    auto measure = [&](int variant, int reps) {
        double cpu_start = thread_ms();
        auto wall_start = Clock::now();
        for (int r = 0; r < reps; ++r) render(variant);
        double wall = std::chrono::duration<double, std::milli>(Clock::now() - wall_start).count();
        double cpu = thread_ms() - cpu_start;
        return Timing{wall / reps, cpu / reps};
    };
    // Touch code, data and the math library before calibration and measurement.
    for (int variant = 0; variant < 2; ++variant) {
        double warm_ms = 0;
        while (warm_ms < 200) warm_ms += measure(variant, 1).wall_ms;
    }
    std::vector<double> calibration[2];
    for (int i = 0; i < 5; ++i)
        for (int v = 0; v < 2; ++v) calibration[v].push_back(measure(v, 1).wall_ms);
    for (auto& samples : calibration) std::sort(samples.begin(), samples.end());
    double fastest = std::min(calibration[0][2], calibration[1][2]);
    int reps = std::max(1, std::min(100000, int(std::ceil(sample_ms / fastest))));
    std::printf("{\"kind\":\"calibration\",\"reps\":%d,\"off_ms\":%.9f,\"on_ms\":%.9f}\n",
                reps, calibration[0][2], calibration[1][2]);
    std::vector<int> first(rounds);
    for (int i = 0; i < rounds; ++i) first[i] = i % 2;
    std::mt19937 order(seed);
    std::shuffle(first.begin(), first.end(), order);
    for (int round = 0; round < rounds; ++round) {
        Timing samples[2];
        double checksums[2];
        for (int position = 0; position < 2; ++position) {
            int v = first[round] ^ position;
            samples[v] = measure(v, reps);
            checksums[v] = std::accumulate(output[0].begin(), output[0].end(), 0.0);
        }
        // Printing and consuming the output are outside the measured intervals.
        for (int v = 0; v < 2; ++v) {
            std::printf("{\"kind\":\"sample\",\"round\":%d,\"variant\":\"%s\","
                        "\"position\":%d,\"reps\":%d,\"wall_ms\":%.9f,\"cpu_ms\":%.9f,"
                        "\"ns_per_fragment\":%.6f,\"checksum\":%.12g}\n",
                        round, v ? "on" : "off", v == first[round] ? 0 : 1, reps,
                        samples[v].wall_ms, samples[v].cpu_ms,
                        samples[v].wall_ms * 1e6 / n, checksums[v]);
        }
        std::fflush(stdout);
    }
    return 0;
}
