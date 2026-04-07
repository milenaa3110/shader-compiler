#!/usr/bin/env bash
# run_benchmark_compute_blur.sh — GPU (Vulkan compute) vs CPU (RISC-V + OpenMP) blur benchmark
# Shows: data-parallel throughput, Mpixels/ms, GPU vs CPU compute speedup
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
source "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

NRUNS=${NRUNS:-100}

mkdir -p result "$BUILD_DIR"

# ── Build GPU compute host ────────────────────────────────────────────────────
echo -e "${CYAN}Building Vulkan compute host…${RESET}"
make build/spirv/spirv_vulkan_compute_host 2>/dev/null | grep -E "^g\+\+|error" || true
make build/spirv/blur.comp.spv 2>/dev/null

# ── Build CPU compute binary (RISC-V + OpenMP) ───────────────────────────────
echo -e "${CYAN}Building CPU compute binary (RISC-V + OpenMP)…${RESET}"

if [[ "$RISCV_AVAIL" -eq 1 ]]; then
    $CROSS_CXX -std=c++20 -O3 -static -fopenmp \
        -DWIDTH=512 -DHEIGHT=512 -DNRUNS="$NRUNS" \
        test/rv_host/rv_host_compute_blur.cpp \
        build/riscv/blur_cs_rv.o -o "$BUILD_DIR/blur.rv" 2>/dev/null
    echo -e "  blur.rv  ${GREEN}OK${RESET}"
else
    echo -e "  ${YELLOW}SKIP: no RISC-V runtime${RESET}"
fi

# ── GPU run ───────────────────────────────────────────────────────────────────
echo ""
echo -e "${CYAN}Running GPU compute (Vulkan)…${RESET}"
gpu_out=$(./build/spirv/spirv_vulkan_compute_host build/spirv/blur.comp.spv blur "$NRUNS" 2>/dev/null || true)
echo "$gpu_out"
gpu_avg=$(parse_avg "$gpu_out")
gpu_mpx=$(echo "$gpu_out" | grep -o 'Throughput: [0-9.]*' | grep -o '[0-9.]*' || echo "N/A")

# ── CPU run ───────────────────────────────────────────────────────────────────
echo ""
echo -e "${CYAN}Running CPU compute (RISC-V + OpenMP, ${NTHREADS} threads)…${RESET}"
cpu_out=""
cpu_avg="N/A"
cpu_mpx="N/A"
if [[ "$RISCV_AVAIL" -eq 1 && -f "$BUILD_DIR/blur.rv" ]]; then
    cpu_out=$(OMP_NUM_THREADS="$NTHREADS" $RISCV_SIM "./$BUILD_DIR/blur.rv" 2>/dev/null || true)
    echo "$cpu_out"
    cpu_avg=$(parse_avg "$cpu_out")
    cpu_mpx=$(echo "$cpu_out" | grep -o 'Throughput: [0-9.]*' | grep -o '[0-9.]*' || echo "N/A")
else
    echo -e "  ${YELLOW}SKIP (no RISC-V runtime or blur.rv)${RESET}"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}Compute benchmark — 512x512 Gaussian blur (5x5 kernel, ${NRUNS} runs):${RESET}"
echo -e "${BOLD}┌────────────────────┬────────────┬──────────────────┐${RESET}"
echo -e "${BOLD}│ Backend            │ ms/run     │ Mpixels/ms       │${RESET}"
echo -e "${BOLD}├────────────────────┼────────────┼──────────────────┤${RESET}"
printf "│ %-18s │ %10s │ %-16s │\n" \
    "GPU Vulkan" "${gpu_avg}ms" "${gpu_mpx}"
printf "│ %-18s │ %10s │ %-16s │\n" \
    "CPU RISC-V+OMP(${NTHREADS}t)" "${cpu_avg}ms" "${cpu_mpx}"

if [[ "$gpu_avg" != "N/A" && "$cpu_avg" != "N/A" ]]; then
    speedup=$(awk "BEGIN { printf \"%.1fx\", $cpu_avg / $gpu_avg }")
    echo -e "${BOLD}├────────────────────┴────────────┴──────────────────┤${RESET}"
    printf "│ Speedup: GPU vs CPU %-31s│\n" "$speedup"
fi
echo -e "${BOLD}└───────────────────────────────────────────────────────┘${RESET}"

echo ""
echo -e "${CYAN}Output images: result/blur_gpu.ppm  result/blur_cpu.ppm${RESET}"
