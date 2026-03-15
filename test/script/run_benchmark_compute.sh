#!/usr/bin/env bash
# run_benchmark_compute.sh — Conway's Game of Life: GPU (Vulkan compute) vs CPU (RISC-V+OpenMP)
#
# GPU uses pipeline barriers between dispatches (correct approach — no CPU roundtrip).
# CPU uses OpenMP-parallelised loops.
#
# Usage: bash test/script/run_benchmark_compute.sh [OPTIONS]
#   --grid N    Single-grid run at N×N (default 256)
#   --gens N    Number of generations (default 1000)
#   --quick     Fast run (128×128, 100 gens)
#   --sweep     GPU vs CPU across grid sizes
#   --animate   Render 256×256 animation (GPU + CPU) and encode MP4s
#   --tiny      Add a 32×32 case
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
source "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

GRID=256
GENS=1000
QUICK=0
SWEEP=0
ANIMATE=0
TINY=0
for arg in "$@"; do
    case $arg in
        --grid)    shift; GRID="$1" ;;
        --gens)    shift; GENS="$1" ;;
        --quick)   QUICK=1; GRID=128; GENS=100 ;;
        --sweep)   SWEEP=1 ;;
        --animate) ANIMATE=1 ;;
        --tiny)    TINY=1 ;;
    esac
done

echo -e "${CYAN}Building…${RESET}"
make -j"$(nproc)" build/spirv/spirv_vulkan_life_host build/spirv/life.comp.spv 2>/dev/null | \
    grep -E "^g\+\+|error" || true

mkdir -p result "$BUILD_DIR"

# ── helper: run one (grid, gens) pair, print a table row ──────────────────────
run_pair() {
    local g="$1" n="$2"
    local gpu_out cpu_out gpu_ms cpu_ms

    gpu_out=$(./build/spirv/spirv_vulkan_life_host build/spirv/life.comp.spv "$n" "$g" 2>/dev/null || true)
    gpu_ms=$(parse_avg "$gpu_out")

    cpu_ms="N/A"
    if [[ -n "$QEMU_BIN" ]]; then
        riscv64-linux-gnu-g++ -std=c++20 -O3 -static -fopenmp \
            -DGRID="$g" -DNGENERATIONS="$n" \
            test/rv_host/rv_host_compute.cpp -o "$BUILD_DIR/_life_sweep_${g}.rv" >/dev/null 2>&1 || true
        if [[ -f "$BUILD_DIR/_life_sweep_${g}.rv" ]]; then
            cpu_out=$(OMP_NUM_THREADS="${NTHREADS}" "${QEMU_BIN}" \
                -L "$SYSROOT" "./$BUILD_DIR/_life_sweep_${g}.rv" 2>/dev/null || true)
            cpu_ms=$(parse_avg "$cpu_out")
            rm -f "$BUILD_DIR/_life_sweep_${g}.rv"
        fi
    fi

    if [[ "$gpu_ms" != "N/A" && "$cpu_ms" != "N/A" ]]; then
        speedup=$(awk "BEGIN { printf \"%.2f\", $cpu_ms / $gpu_ms }")
        if awk "BEGIN { exit !($cpu_ms < $gpu_ms) }"; then
            winner="${GREEN}CPU wins${RESET}"
        else
            winner="${YELLOW}GPU wins${RESET}"
        fi
        printf "│ %5d×%-5d │ %10s ms │ %10s ms │ %7sx │ " \
            "$g" "$g" "$gpu_ms" "$cpu_ms" "$speedup"
        echo -e "${winner}  │"
    else
        printf "│ %5d×%-5d │ %10s ms │ %10s ms │ %9s │ %12s │\n" \
            "$g" "$g" "$gpu_ms" "$cpu_ms" "N/A" "N/A"
    fi
}

# ══════════════════════════════════════════════════════════════════════════════
# SWEEP MODE
# ══════════════════════════════════════════════════════════════════════════════
if [[ "$SWEEP" -eq 1 ]]; then
    SWEEP_GENS=500
    SWEEP_GRIDS=(16 32 64 128 256 512)

    echo ""
    echo -e "${BOLD}Grid-size sweep — ${SWEEP_GENS} generations, ${NTHREADS} CPU threads${RESET}"
    echo -e "  Goal: observe how GPU/CPU balance shifts across grid sizes."
    echo -e "  Under QEMU, GPU wins at all sizes — emulation overhead masks CPU cache advantage."
    echo -e "  On real RISC-V hardware, small grids (L1 cache, low GPU occupancy) would favor CPU."
    echo ""
    echo -e "${BOLD}┌────────────┬──────────────┬──────────────┬─────────┬──────────────┐${RESET}"
    echo -e "${BOLD}│ Grid       │ GPU ms/gen   │ CPU ms/gen   │ Speedup │ Winner       │${RESET}"
    echo -e "${BOLD}├────────────┼──────────────┼──────────────┼─────────┼──────────────┤${RESET}"

    for g in "${SWEEP_GRIDS[@]}"; do
        run_pair "$g" "$SWEEP_GENS"
    done

    echo -e "${BOLD}└────────────┴──────────────┴──────────────┴─────────┴──────────────┘${RESET}"
    echo ""
    echo -e "${BOLD}What this shows:${RESET}"
    echo -e "  GPU uses pipeline barriers — all generations in one command buffer, one submit."
    echo -e "  CPU uses OpenMP with cache-friendly sequential access."
    echo -e "  Under QEMU, the crossover does not appear — emulation overhead keeps CPU slower at all sizes."
    echo -e "  On real hardware: small grid (L1, low occupancy) → CPU wins; large grid → GPU wins."
    echo -e "  GPU uses one submit for all ${SWEEP_GENS} generations via pipeline barriers (no roundtrip)."
    echo ""
    echo -e "  Cells in memory per grid:"
    for g in "${SWEEP_GRIDS[@]}"; do
        kb=$(( g * g * 4 / 1024 ))
        if (( kb < 64 )); then cache="L1"; elif (( kb < 512 )); then cache="L2"; else cache="L3+"; fi
        printf "    %4d×%-4d = %5d KB  (%s cache)\n" "$g" "$g" "$kb" "$cache"
    done
    exit 0
fi

# ══════════════════════════════════════════════════════════════════════════════
# ANIMATE MODE
# ══════════════════════════════════════════════════════════════════════════════
if [[ "$ANIMATE" -eq 1 ]]; then
    ANIM_GRID=256
    ANIM_GENS=600     # 600 gens, snap every 5 → 120 frames → 4 sec at 30fps
    SNAP=5

    echo ""
    echo -e "${BOLD}Animation mode — ${ANIM_GRID}×${ANIM_GRID}, ${ANIM_GENS} gens, snap every ${SNAP}${RESET}"
    echo ""

    # GPU animation
    echo -e "${CYAN}GPU animation…${RESET}"
    ./build/spirv/spirv_vulkan_life_host build/spirv/life.comp.spv "$ANIM_GENS" "$ANIM_GRID" "$SNAP"

    # CPU animation
    if [[ -n "$QEMU_BIN" ]]; then
        echo -e "${CYAN}Compiling CPU life (${ANIM_GRID}×${ANIM_GRID}, snap every ${SNAP})…${RESET}"
        riscv64-linux-gnu-g++ -std=c++20 -O3 -static -fopenmp \
            -DGRID="${ANIM_GRID}" -DNGENERATIONS="${ANIM_GENS}" -DSNAP_EVERY="${SNAP}" \
            test/rv_host/rv_host_compute.cpp -o "$BUILD_DIR/life_anim.rv" 2>/dev/null
        echo -e "${CYAN}CPU animation…${RESET}"
        OMP_NUM_THREADS="${NTHREADS}" "${QEMU_BIN}" \
            -L "$SYSROOT" "./$BUILD_DIR/life_anim.rv"
        rm -f "$BUILD_DIR/life_anim.rv"
    fi

    echo ""
    echo -e "${BOLD}Output:${RESET}"
    [[ -f result/life_gpu.mp4 ]] && echo -e "  ${GREEN}GPU:${RESET} result/life_gpu.mp4"
    [[ -f result/life_cpu.mp4 ]] && echo -e "  ${GREEN}CPU:${RESET} result/life_cpu.mp4"
    exit 0
fi

# ══════════════════════════════════════════════════════════════════════════════
# SINGLE-GRID RUN (default) — optionally with a 32×32 CPU-wins case
# ══════════════════════════════════════════════════════════════════════════════

# Build main RISC-V binary
if [[ -n "$QEMU_BIN" ]]; then
    echo -e "${CYAN}Compiling RISC-V life host (${GRID}×${GRID}, ${GENS} gens)…${RESET}"
    riscv64-linux-gnu-g++ -std=c++20 -O3 -static -fopenmp \
        -DGRID="${GRID}" -DNGENERATIONS="${GENS}" \
        test/rv_host/rv_host_compute.cpp -o "$BUILD_DIR/life.rv" 2>/dev/null && \
        echo -e "  life.rv  ${GREEN}OK${RESET}" || echo -e "  life.rv  ${YELLOW}FAIL${RESET}"
fi

# ── 32×32 CPU-wins case ───────────────────────────────────────────────────────
if [[ "$TINY" -eq 1 ]]; then
    TINY_GRID=32
    TINY_GENS=2000

    echo ""
    echo -e "${BOLD}━━━ 32×32 — low GPU occupancy case ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
    echo -e "  At this grid size GPU occupancy is very low (4 workgroups of 16×16)."
    echo -e "  Data fits in CPU L1 cache — theoretically favours CPU, but QEMU overhead still applies."
    echo ""

    gpu_tiny_out=$(./build/spirv/spirv_vulkan_life_host build/spirv/life.comp.spv "$TINY_GENS" "$TINY_GRID" 2>/dev/null || true)
    GPU_TINY=$(parse_avg "$gpu_tiny_out")

    CPU_TINY="N/A"
    if [[ -n "$QEMU_BIN" ]]; then
        riscv64-linux-gnu-g++ -std=c++20 -O3 -static -fopenmp \
            -DGRID="${TINY_GRID}" -DNGENERATIONS="${TINY_GENS}" \
            test/rv_host/rv_host_compute.cpp -o "$BUILD_DIR/_life_tiny.rv" >/dev/null 2>&1 || true
        if [[ -f "$BUILD_DIR/_life_tiny.rv" ]]; then
            cpu_tiny_out=$(OMP_NUM_THREADS="${NTHREADS}" "${QEMU_BIN}" \
                -L "$SYSROOT" "./$BUILD_DIR/_life_tiny.rv" 2>/dev/null || true)
            CPU_TINY=$(parse_avg "$cpu_tiny_out")
            rm -f "$BUILD_DIR/_life_tiny.rv"
        fi
    fi

    echo -e "${BOLD}┌────────────────────┬──────────────────┬──────────────────┬──────────┐${RESET}"
    echo -e "${BOLD}│ Backend            │ ms/generation    │ gen/s            │ Winner   │${RESET}"
    echo -e "${BOLD}├────────────────────┼──────────────────┼──────────────────┼──────────┤${RESET}"
    if [[ "$GPU_TINY" != "N/A" ]]; then
        gpu_gs=$(awk "BEGIN { printf \"%.0f\", 1000.0 / ${GPU_TINY} }")
        printf "│ %-18s │ %16s │ %16s │ %8s │\n" \
            "GPU (Vulkan)" "${GPU_TINY} ms" "${gpu_gs}" "—"
    fi
    if [[ "$CPU_TINY" != "N/A" ]]; then
        cpu_gs=$(awk "BEGIN { printf \"%.0f\", 1000.0 / ${CPU_TINY} }")
        if [[ "$GPU_TINY" != "N/A" ]]; then
            if awk "BEGIN { exit !($CPU_TINY < $GPU_TINY) }"; then
                winner="CPU ✓"
            else
                winner="GPU ✓"
            fi
            speedup=$(awk "BEGIN { printf \"%.1fx\", $GPU_TINY / $CPU_TINY }")
            printf "│ %-18s │ %16s │ %16s │ %8s │\n" \
                "CPU (RISC-V+OMP)" "${CPU_TINY} ms" "${cpu_gs}" "${winner} ${speedup}"
        else
            printf "│ %-18s │ %16s │ %16s │ %8s │\n" \
                "CPU (RISC-V+OMP)" "${CPU_TINY} ms" "${cpu_gs}" "—"
        fi
    fi
    echo -e "${BOLD}└────────────────────┴──────────────────┴──────────────────┴──────────┘${RESET}"
    echo ""
    echo -e "  32×32 = 1024 cells = ${BOLD}4 KB${RESET} — fits entirely in L1 cache."
    echo -e "  4 workgroups → GPU underutilized. Under QEMU, GPU still wins due to emulation overhead."
    echo -e "  On real RISC-V hardware (e.g. Banana Pi F3), expect CPU to win at this size."
    echo ""
fi

# ── Main grid run ─────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}Running Game of Life benchmark (${GRID}×${GRID} grid, ${GENS} generations)…${RESET}"
echo ""

GPU_MSPG="N/A"
if [[ -f "build/spirv/life.comp.spv" ]]; then
    echo -e "${CYAN}GPU (Vulkan compute, ${GENS} gens — pipeline barriers, single submit):${RESET}"
    gpu_out=$(./build/spirv/spirv_vulkan_life_host build/spirv/life.comp.spv "$GENS" "$GRID" 2>/dev/null || true)
    echo "$gpu_out" | grep -v "^$"
    GPU_MSPG=$(parse_avg "$gpu_out")
else
    echo -e "${YELLOW}GPU: SKIP (no build/spirv/life.comp.spv)${RESET}"
fi

CPU_MSPG="N/A"
if [[ -n "$QEMU_BIN" && -f "$BUILD_DIR/life.rv" ]]; then
    echo ""
    echo -e "${CYAN}CPU (RISC-V+OpenMP via QEMU, ${NTHREADS} threads, ${GENS} loop iterations):${RESET}"
    cpu_out=$(OMP_NUM_THREADS="${NTHREADS}" "${QEMU_BIN}" -L "$SYSROOT" "./$BUILD_DIR/life.rv" 2>/dev/null || true)
    echo "$cpu_out" | grep -v "^$"
    CPU_MSPG=$(parse_avg "$cpu_out")
else
    echo -e "${YELLOW}CPU: SKIP (no QEMU or life.rv build failed)${RESET}"
fi

echo ""
echo -e "${BOLD}┌────────────────────┬──────────────────┬──────────────────┬──────────┐${RESET}"
echo -e "${BOLD}│ Backend            │ ms/generation    │ gen/s            │ Speedup  │${RESET}"
echo -e "${BOLD}├────────────────────┼──────────────────┼──────────────────┼──────────┤${RESET}"

if [[ "$GPU_MSPG" != "N/A" ]]; then
    gpu_gs=$(awk "BEGIN { printf \"%.1f\", 1000.0 / ${GPU_MSPG} }")
    printf "│ %-18s │ %16s │ %16s │ %8s │\n" \
        "GPU (Vulkan)" "${GPU_MSPG} ms" "${gpu_gs}" "1x (ref)"
fi
if [[ "$CPU_MSPG" != "N/A" && "$GPU_MSPG" != "N/A" ]]; then
    cpu_gs=$(awk "BEGIN { printf \"%.1f\", 1000.0 / ${CPU_MSPG} }")
    speedup=$(awk "BEGIN { printf \"%.1fx\", ${CPU_MSPG} / ${GPU_MSPG} }")
    printf "│ %-18s │ %16s │ %16s │ %8s │\n" \
        "CPU (RISC-V+OMP)" "${CPU_MSPG} ms" "${cpu_gs}" "${speedup}"
elif [[ "$CPU_MSPG" != "N/A" ]]; then
    cpu_gs=$(awk "BEGIN { printf \"%.1f\", 1000.0 / ${CPU_MSPG} }")
    printf "│ %-18s │ %16s │ %16s │ %8s │\n" \
        "CPU (RISC-V+OMP)" "${CPU_MSPG} ms" "${cpu_gs}" "N/A"
fi

echo -e "${BOLD}└────────────────────┴──────────────────┴──────────────────┴──────────┘${RESET}"

echo ""
echo -e "${BOLD}What this shows:${RESET}"
echo -e "  GPU: pipeline barriers between dispatches — no CPU roundtrip per generation."
echo -e "       All ${GENS} generations in one command buffer, one vkQueueSubmit."
echo -e "  CPU: ${GENS} iterations in a tight OpenMP loop, ${NTHREADS} threads."
echo -e "       ${GRID}×${GRID} grid = $((GRID * GRID * 4)) bytes."
echo ""
echo -e "  Flags: ${BOLD}--tiny${RESET} (32×32)  ${BOLD}--sweep${RESET} (crossover chart)  ${BOLD}--animate${RESET} (MP4)"
