// Optional compiler phase timings. Enable with SHADER_PROFILE=1.
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

class CompileProfile {
    using Clock = std::chrono::steady_clock;
    bool enabled_ = std::getenv("SHADER_PROFILE") != nullptr;
    Clock::time_point last_{};
    std::vector<std::pair<const char*, double>> phases_;
public:
    CompileProfile() {
        if (enabled_) { phases_.reserve(12); last_ = Clock::now(); }
    }
    void mark(const char* phase) {
        if (!enabled_) return;
        const auto now = Clock::now();
        phases_.emplace_back(phase,
            std::chrono::duration<double, std::milli>(now - last_).count());
        last_ = now;
    }
    void report() const {
        if (!enabled_) return;
        for (const auto& [name, ms] : phases_)
            std::fprintf(stderr, "[shader-profile] phase=%s ms=%.6f\n", name, ms);
    }
};
