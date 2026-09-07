#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Strip named options before the hosts interpret their legacy positional args.
struct BenchmarkOptions {
    bool video = false;
    bool bench = false;
    BenchmarkOptions(int& argc, char** argv) {
        int dst = 1;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--video")) video = true;
            else if (!std::strcmp(argv[i], "--no-video")) video = false;
            else if (!std::strcmp(argv[i], "--bench")) bench = true;
            else if (!std::strcmp(argv[i], "--packet-mode")) {
                if (++i == argc || (std::strcmp(argv[i], "scalar") && std::strcmp(argv[i], "packet"))) {
                    std::fprintf(stderr, "--packet-mode requires scalar or packet\n");
                    std::exit(2);
                }
                if (!std::strcmp(argv[i], "packet")) setenv("SHADER_PACKET", "1", 1);
                else unsetenv("SHADER_PACKET");
            } else if (!std::strncmp(argv[i], "--", 2)) {
                std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
                std::exit(2);
            } else argv[dst++] = argv[i];
        }
        argc = dst;
        argv[dst] = nullptr;
        std::printf("Video: %s\n", video ? "enabled" : "disabled");
    }
};

struct BenchmarkWallTime {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~BenchmarkWallTime() {
        std::printf("Host wall total: %.6f ms\n", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
};
