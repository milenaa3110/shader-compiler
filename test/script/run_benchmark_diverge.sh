#!/usr/bin/env bash
# run_benchmark_diverge.sh — Branch divergence + dispatch overhead benchmarks
#
# THREE focused measurements:
#
# 1. BRANCH DIVERGENCE
#    Compares diverge.frag vs mandelbrot.frag (uniform heavy workload).
#    Diverge shader: left half = 4 trig ops, right half = 96-iter Mandelbrot.
#    Ideal (no divergence penalty): diverge ≈ 50% of mandelbrot time.
#    GPU reality:  warps straddling x=0.5 execute ALL 96 iterations for all lanes
#                  → diverge ≈ mandelbrot time → efficiency ~50%
#    CPU reality:  each thread runs its own branch independently
#                  → diverge ≈ 50% of mandelbrot time → efficiency ~100%
#
# 2. DISPATCH OVERHEAD ISOLATION
#    Renders a 1×1 pixel frame — compute time is negligible.
#    Any measured latency is pure vkQueueSubmit + pipeline overhead.
#    CPU has no equivalent cost; loop iteration overhead is nanoseconds.
#
# 3. WARP BOUNDARY EFFECT
#    Renders diverge at three resolutions: 64, 256, 512.
#    At 64px the boundary region (warps straddling x=0.5) is proportionally
#    larger → penalty is worse. At 512px the boundary shrinks relative to
#    the total pixel count → penalty softens.
#
# Usage: bash test/script/run_benchmark_diverge.sh [--quick]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
source "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

QUICK=0
for arg in "$@"; do
    case $arg in --quick) QUICK=1 ;; esac
done

VK_FRAMES=$([[ $QUICK -eq 1 ]] && echo 20 || echo 60)
RV_FRAMES=$([[ $QUICK -eq 1 ]] && echo 4  || echo 8)
RV_W=256; RV_H=256

# ── Build ─────────────────────────────────────────────────────────────────────
echo -e "${CYAN}Building…${RESET}"
make -j"$(nproc)" build/spirv/spirv_vulkan_host build/riscv/irgen_riscv 2>/dev/null | grep -E "^g\+\+|error" || true
make build/spirv/quad.vert.spv build/spirv/diverge.frag.spv build/spirv/mandelbrot.frag.spv 2>/dev/null
mkdir -p result "$BUILD_DIR"

# ── helper: GPU ms/frame for a given .spv ─────────────────────────────────────
gpu_ms_for() {
    local spv="$1" name="$2" frames="$3" w="${4:-512}" h="${5:-512}"
    local out
    out=$(./build/spirv/spirv_vulkan_host build/spirv/quad.vert.spv "$spv" "$name" "$frames" "$w" "$h" --bench 2>/dev/null || true)
    echo "$out" | grep 'Vulkan avg:' | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "N/A"
}

# ── helper: build + run one RISC-V animation, return avg ms/frame ─────────────
rv_ms_for() {
    local anim="$1" frames="$2" w="$3" h="$4"
    local bin="$BUILD_DIR/_bench_${anim}_${w}.rv"
    [[ -z "$QEMU_BIN" ]] && { echo "N/A"; return; }

    make "build/riscv/${anim}_rv.o" >/dev/null 2>&1 || { echo "N/A"; return; }

    riscv64-linux-gnu-g++ -std=c++20 -O3 -static -fopenmp -Ipipeline \
        -DANIM_NAME="\"${anim}\"" -DNFRAMES="$frames" \
        -DWIDTH="$w" -DHEIGHT="$h" \
        test/rv_host/rv_host_fragment.cpp \
        pipeline/pipeline_runtime.cpp \
        "build/riscv/${anim}_rv.o" -o "$bin" >/dev/null 2>&1 || { echo "N/A"; return; }

    local out
    out=$(OMP_NUM_THREADS="$NTHREADS" "$QEMU_BIN" -L "$SYSROOT" "./$bin" 2>/dev/null || true)
    rm -f "$bin"
    echo "$out" | grep 'RISC-V avg:' | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "N/A"
}

# ── helper: divergence efficiency (%) ────────────────────────────────────────
# Ideal: diverge_time = 50% of mandelbrot_time (only half pixels do heavy work).
# Efficiency = ideal / actual = (0.5 * mandel_ms) / diverge_ms * 100
div_efficiency() {
    local diverge_ms="$1" mandel_ms="$2"
    [[ "$diverge_ms" == "N/A" || "$mandel_ms" == "N/A" ]] && { echo "N/A"; return; }
    awk "BEGIN { printf \"%.0f%%\", (0.5 * $mandel_ms) / $diverge_ms * 100 }"
}

mkdir -p result

# ══════════════════════════════════════════════════════════════════════════════
# 1. BRANCH DIVERGENCE
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}━━━ 1. Branch Divergence ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo -e "  Baseline: mandelbrot (all pixels run 96-iter loop — uniform workload)"
echo -e "  Test:     diverge    (left half: 4 ops, right half: 96-iter Mandelbrot)"
echo -e "  Ideal efficiency: diverge ≈ 50%% of mandelbrot time (only half pixels heavy)"
echo ""

echo -e "${CYAN}Running GPU…${RESET}"
GPU_MANDEL=$(gpu_ms_for build/spirv/mandelbrot.frag.spv mandelbrot "$VK_FRAMES")
GPU_DIVERGE=$(gpu_ms_for build/spirv/diverge.frag.spv   diverge    "$VK_FRAMES")

echo -e "${CYAN}Running CPU (RISC-V + OpenMP)…${RESET}"
CPU_MANDEL=$(rv_ms_for mandelbrot "$RV_FRAMES" "$RV_W" "$RV_H")
CPU_DIVERGE=$(rv_ms_for diverge   "$RV_FRAMES" "$RV_W" "$RV_H")

GPU_EFF=$(div_efficiency "$GPU_DIVERGE" "$GPU_MANDEL")
CPU_EFF=$(div_efficiency "$CPU_DIVERGE" "$CPU_MANDEL")
GPU_SPD_MANDEL=$(speedup_label "$GPU_MANDEL"  "$CPU_MANDEL")
GPU_SPD_DIVERGE=$(speedup_label "$GPU_DIVERGE" "$CPU_DIVERGE")

echo ""
echo -e "${BOLD}┌──────────────┬─────────────┬─────────────┬──────────────┐${RESET}"
echo -e "${BOLD}│ Shader       │ GPU ms/f    │ CPU ms/f    │ GPU speedup  │${RESET}"
echo -e "${BOLD}├──────────────┼─────────────┼─────────────┼──────────────┤${RESET}"
printf "│ %-12s │ %11s │ %11s │ %12s │\n" \
    "mandelbrot" "${GPU_MANDEL}ms" "${CPU_MANDEL}ms" "${GPU_SPD_MANDEL}"
printf "│ %-12s │ %11s │ %11s │ %12s │\n" \
    "diverge" "${GPU_DIVERGE}ms" "${CPU_DIVERGE}ms" "${GPU_SPD_DIVERGE}"
echo -e "${BOLD}├──────────────┴─────────────┴─────────────┴──────────────┤${RESET}"
printf "│ %-56s │\n" "Divergence efficiency (ideal = 50%% of mandelbrot time)"
printf "│   GPU: %s   CPU: %s%-30s │\n" "${GPU_EFF}" "${CPU_EFF}" ""
echo -e "${BOLD}└──────────────────────────────────────────────────────────┘${RESET}"

echo ""
echo -e "${BOLD}Interpretation:${RESET}"
echo -e "  ${BOLD}Note:${RESET} mandelbrot.frag uses up to 256 iterations; diverge.frag heavy"
echo -e "  path uses 96 — so diverge is faster in absolute time on both backends."
echo -e "  The meaningful metric is ${BOLD}efficiency${RESET}: how close diverge is to the ideal"
echo -e "  50%% of mandelbrot time (since only half the pixels do the heavy work)."
echo -e ""
if [[ "$GPU_EFF" != "N/A" && "$CPU_EFF" != "N/A" ]]; then
    echo -e "  GPU efficiency ${GPU_EFF}: warps spanning x=0.5 execute ALL iterations for"
    echo -e "    every lane — the warp runs at the speed of its slowest pixel."
    echo -e "  CPU efficiency ${CPU_EFF}: each RISC-V thread takes exactly its own branch."
    echo -e "    Light pixels (~4 ops) and heavy pixels (~768 ops) run independently."
fi
echo -e "  GPU speedup: ${GPU_SPD_MANDEL} (uniform mandelbrot) vs ${GPU_SPD_DIVERGE} (divergent) —"
echo -e "  divergence narrows the advantage by changing the effective workload balance."

# ══════════════════════════════════════════════════════════════════════════════
# 2. DISPATCH OVERHEAD ISOLATION
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}━━━ 2. Dispatch Overhead Isolation ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo -e "  Render a 1×1 pixel frame — negligible compute, pure pipeline overhead."
echo -e "  Measured cost = vkQueueSubmit + renderpass + vkWaitForFences."
echo ""

OVERHEAD_FRAMES=$([[ $QUICK -eq 1 ]] && echo 50 || echo 200)
echo -e "${CYAN}Running GPU at 1×1 (${OVERHEAD_FRAMES} frames)…${RESET}"
OVERHEAD_MS=$(gpu_ms_for build/spirv/mandelbrot.frag.spv overhead_1x1 "$OVERHEAD_FRAMES" 1 1)

echo -e "${CYAN}Running GPU at 512×512 (${VK_FRAMES} frames, reference)…${RESET}"
FULL_MS=$(gpu_ms_for build/spirv/mandelbrot.frag.spv overhead_512 "$VK_FRAMES" 512 512)

echo ""
echo -e "${BOLD}┌──────────────────────┬──────────────┬─────────────────────────────────┐${RESET}"
echo -e "${BOLD}│ Measurement          │ ms/frame     │ Notes                           │${RESET}"
echo -e "${BOLD}├──────────────────────┼──────────────┼─────────────────────────────────┤${RESET}"
printf "│ %-20s │ %12s │ %-31s │\n" \
    "Dispatch overhead" "${OVERHEAD_MS}ms" "1×1 px: compute ≈ 0"
printf "│ %-20s │ %12s │ %-31s │\n" \
    "Mandelbrot 512×512" "${FULL_MS}ms" "real workload"

if [[ "$OVERHEAD_MS" != "N/A" && "$FULL_MS" != "N/A" ]]; then
    OVERHEAD_PCT=$(awk "BEGIN { printf \"%.1f%%\", $OVERHEAD_MS / $FULL_MS * 100 }")
    COMPUTE_MS=$(awk "BEGIN { printf \"%.4f\", $FULL_MS - $OVERHEAD_MS }")
    printf "│ %-20s │ %12s │ %-31s │\n" \
        "Pure compute" "${COMPUTE_MS}ms" "= full − overhead"
    printf "│ %-20s │ %12s │ %-31s │\n" \
        "Overhead fraction" "" "${OVERHEAD_PCT} of total frame time"
fi
echo -e "${BOLD}└──────────────────────┴──────────────┴─────────────────────────────────┘${RESET}"

echo ""
echo -e "${BOLD}Interpretation:${RESET}"
echo -e "  This ~${OVERHEAD_MS}ms is paid on EVERY frame regardless of shader complexity."
echo -e "  For the Game of Life sweep: grids where compute_time < ${OVERHEAD_MS}ms → CPU wins."
echo -e "  For fragment shaders: overhead amortized over ${BOLD}W×H pixel invocations${RESET} at once"
echo -e "  (single dispatch vs N=generations sequential dispatches in Life)."

# ══════════════════════════════════════════════════════════════════════════════
# 3. WARP BOUNDARY EFFECT
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}━━━ 3. Warp Boundary Effect ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo -e "  Diverge shader at three resolutions. The boundary column (warps at x=0.5)"
echo -e "  is roughly constant width in pixels (~warp_width pixels wide)."
echo -e "  As resolution grows: boundary pixels / total pixels → 0 → penalty shrinks."
echo ""

BOUNDARY_FRAMES=$([[ $QUICK -eq 1 ]] && echo 20 || echo 40)
echo -e "${CYAN}Running GPU diverge at 64, 256, 512…${RESET}"
DIV_64=$(gpu_ms_for  build/spirv/diverge.frag.spv  div64  "$BOUNDARY_FRAMES" 64  64)
DIV_256=$(gpu_ms_for build/spirv/diverge.frag.spv  div256 "$BOUNDARY_FRAMES" 256 256)
DIV_512=$(gpu_ms_for build/spirv/diverge.frag.spv  div512 "$BOUNDARY_FRAMES" 512 512)

echo -e "${CYAN}Running GPU mandelbrot at 64, 256, 512 (uniform baseline)…${RESET}"
MAN_64=$(gpu_ms_for  build/spirv/mandelbrot.frag.spv man64  "$BOUNDARY_FRAMES" 64  64)
MAN_256=$(gpu_ms_for build/spirv/mandelbrot.frag.spv man256 "$BOUNDARY_FRAMES" 256 256)
MAN_512=$(gpu_ms_for build/spirv/mandelbrot.frag.spv man512 "$BOUNDARY_FRAMES" 512 512)

echo ""
echo -e "${BOLD}┌────────┬────────────────────┬────────────────────┬────────────┬──────────────────────┐${RESET}"
echo -e "${BOLD}│ Res    │ diverge ms/f       │ mandelbrot ms/f    │ Efficiency │ Boundary pixels      │${RESET}"
echo -e "${BOLD}├────────┼────────────────────┼────────────────────┼────────────┼──────────────────────┤${RESET}"

for res in 64 256 512; do
    eval "div_ms=\$DIV_${res}"; eval "man_ms=\$MAN_${res}"
    eff=$(div_efficiency "$div_ms" "$man_ms")
    # boundary ≈ warp_width (32) columns × height pixels, as % of total
    boundary_pct=$(awk "BEGIN { printf \"~%.1f%%\", 32.0 / $res * 100 }")
    printf "│ %6s │ %18s │ %18s │ %10s │ %-20s │\n" \
        "${res}×${res}" "${div_ms}ms" "${man_ms}ms" "$eff" "$boundary_pct of pixels"
done

echo -e "${BOLD}└────────┴────────────────────┴────────────────────┴────────────┴──────────────────────┘${RESET}"

echo ""
echo -e "${BOLD}Interpretation:${RESET}"
echo -e "  At 64×64: boundary warps (~32 cols) cover ${BOLD}50%${RESET} of the image → high penalty."
echo -e "  At 512×512: boundary warps cover only ${BOLD}~6%${RESET} → penalty diluted by uniform pixels."
echo -e "  Efficiency rises with resolution because interior warps (all heavy or all light)"
echo -e "  are not divergent — only warps that straddle x=0.5 pay the full penalty."
echo ""
echo -e "${CYAN}Summary of all three effects:${RESET}"
echo -e "  1. ${BOLD}Divergence penalty${RESET}:     GPU speedup on diverge vs uniform workload"
echo -e "  2. ${BOLD}Dispatch overhead${RESET}:      ~${OVERHEAD_MS}ms per vkQueueSubmit, paid every frame"
echo -e "  3. ${BOLD}Boundary dilution${RESET}:      penalty shrinks at higher resolution"
echo -e ""
echo -e "  Together these explain why GPU advantage varies across workloads:"
echo -e "  blur (uniform, massively parallel) → diverge (${GPU_SPD_DIVERGE}, warp penalty) → life (sequential gen dependency)."
