#!/usr/bin/env bash
# run_benchmark_diverge.sh — Branch divergence + dispatch overhead benchmarks
#
# THREE focused measurements:

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
build_target spirv_vulkan_host irgen_riscv 2>/dev/null | grep -E "^g\+\+|error" || true
build_target quad.vert.spv diverge.frag.spv mandelbrot.frag.spv 2>/dev/null
mkdir -p result "$BUILD_DIR"

# ── helper: GPU ms/frame for a given .spv ─────────────────────────────────────
gpu_ms_for() {
    local spv="$1" name="$2" frames="$3" w="${4:-512}" h="${5:-512}"
    local out
    out=$(./build/spirv/spirv_vulkan_host build/spirv/quad.vert.spv "build/spirv/$spv" "$name" "$frames" "$w" "$h" --bench 2>/dev/null || true)
    echo "$out" | grep 'Vulkan avg:' | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "N/A"
}

# ── helper: build one RISC-V animation, echo the binary path ("" on failure) ──
# The same binary serves both execution modes — the runtime picks the fragment
# dispatch path at run time (weak fs_packet symbol + SHADER_PACKET), so there is
# no reason to link twice.
rv_build() {
    local anim="$1" frames="$2" w="$3" h="$4"
    local bin="$BUILD_DIR/_bench_${anim}_${w}.rv"
    [[ "$RISCV_AVAIL" -eq 0 ]] && { echo ""; return; }

    build_target "${anim}_rv.o" >/dev/null 2>&1 || { echo ""; return; }

    $CROSS_CXX -std=c++20 -O3 -static -fopenmp -Isrc/runtime \
        -DANIM_NAME="\"${anim}\"" -DNFRAMES="$frames" \
        -DWIDTH="$w" -DHEIGHT="$h" \
        test/rv_host/rv_host_fragment.cpp \
        src/runtime/pipeline_runtime.cpp \
        "build/riscv/${anim}_rv.o" -o "$bin" >/dev/null 2>&1 || { echo ""; return; }

    echo "$bin"
}

# ── helper: run a built binary, return avg ms/frame ──────────────────────────
# mode = packet → SHADER_PACKET=1, width-4 SPMD fs_packet (4 pixels per call,
#                 one execution mask shared by all 4 lanes)
# mode = scalar → SHADER_PACKET unset, per-pixel fs_invoke (each pixel owns its
#                 own control flow). Unset explicitly: the benchmark-diverge
#                 CMake target exports SHADER_PACKET=1 for the whole script.
rv_run() {
    local bin="$1" mode="$2"
    [[ -z "$bin" ]] && { echo "N/A"; return; }

    local out
    if [[ "$mode" == "scalar" ]]; then
        out=$(env -u SHADER_PACKET OMP_NUM_THREADS="$NTHREADS" $RISCV_SIM "./$bin" 2>/dev/null || true)
    else
        out=$(env SHADER_PACKET=1 OMP_NUM_THREADS="$NTHREADS" $RISCV_SIM "./$bin" 2>/dev/null || true)
    fi
    echo "$out" | grep 'RISC-V avg:' | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "N/A"
}

# ── helper: did the shader actually packetize? ───────────────────────────────
# If the packetizer bailed (construct outside its subset) no @fs_packet is
# emitted, the runtime silently uses the scalar path, and the two CPU columns
# would be the same number measured twice.
rv_packetized() {
    grep -q '@fs_packet' "build/riscv/${1}_rv.ll" 2>/dev/null && echo "yes" || echo "no"
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
echo -e "  Test: diverge    (left half: 4 ops, right half: 96-iter Mandelbrot)"
echo -e "  Ideal efficiency: diverge ≈ 50% of mandelbrot time (only half pixels heavy)"
echo -e "  CPU is measured twice: width-4 packet (SHADER_PACKET=1) and scalar per-pixel."
echo ""

echo -e "${CYAN}Running GPU…${RESET}"
GPU_MANDEL=$(gpu_ms_for mandelbrot.frag.spv mandelbrot "$VK_FRAMES")
GPU_DIVERGE=$(gpu_ms_for diverge.frag.spv   diverge    "$VK_FRAMES")

echo -e "${CYAN}Linking CPU (RISC-V) binaries…${RESET}"
BIN_MANDEL=$(rv_build mandelbrot "$RV_FRAMES" "$RV_W" "$RV_H")
BIN_DIVERGE=$(rv_build diverge   "$RV_FRAMES" "$RV_W" "$RV_H")

echo -e "${CYAN}Running CPU packet (width-4 SPMD + OpenMP)…${RESET}"
CPU_MANDEL=$(rv_run "$BIN_MANDEL"  packet)
CPU_DIVERGE=$(rv_run "$BIN_DIVERGE" packet)

echo -e "${CYAN}Running CPU scalar (per-pixel fs_invoke + OpenMP)…${RESET}"
CPU_MANDEL_S=$(rv_run "$BIN_MANDEL"  scalar)
CPU_DIVERGE_S=$(rv_run "$BIN_DIVERGE" scalar)

rm -f "$BIN_MANDEL" "$BIN_DIVERGE" 2>/dev/null || true

GPU_EFF=$(div_efficiency "$GPU_DIVERGE"   "$GPU_MANDEL")
CPU_EFF=$(div_efficiency "$CPU_DIVERGE"   "$CPU_MANDEL")
CPU_EFF_S=$(div_efficiency "$CPU_DIVERGE_S" "$CPU_MANDEL_S")
GPU_SPD_MANDEL=$(speedup_label "$GPU_MANDEL"  "$CPU_MANDEL")
GPU_SPD_DIVERGE=$(speedup_label "$GPU_DIVERGE" "$CPU_DIVERGE")
# scalar ÷ packet: what the width-4 packetizer bought on this shader
PKT_GAIN_MANDEL=$(speedup_label "$CPU_MANDEL"  "$CPU_MANDEL_S")
PKT_GAIN_DIVERGE=$(speedup_label "$CPU_DIVERGE" "$CPU_DIVERGE_S")

EFF_LINE=$(printf "  GPU: %-8s CPU packet: %-8s CPU scalar: %-8s" \
    "${GPU_EFF}" "${CPU_EFF}" "${CPU_EFF_S}")

echo ""
echo -e "${BOLD}┌──────────────┬─────────────┬─────────────┬─────────────┬───────────┬─────────────┐${RESET}"
echo -e "${BOLD}│ Shader       │ GPU ms/f    │ CPU pkt ms/f│ CPU scl ms/f│ pkt gain  │ GPU speedup │${RESET}"
echo -e "${BOLD}├──────────────┼─────────────┼─────────────┼─────────────┼───────────┼─────────────┤${RESET}"
printf "│ %-12s │ %11s │ %11s │ %11s │ %9s │ %11s │\n" \
    "mandelbrot" "${GPU_MANDEL}ms" "${CPU_MANDEL}ms" "${CPU_MANDEL_S}ms" \
    "${PKT_GAIN_MANDEL}" "${GPU_SPD_MANDEL}"
printf "│ %-12s │ %11s │ %11s │ %11s │ %9s │ %11s │\n" \
    "diverge" "${GPU_DIVERGE}ms" "${CPU_DIVERGE}ms" "${CPU_DIVERGE_S}ms" \
    "${PKT_GAIN_DIVERGE}" "${GPU_SPD_DIVERGE}"
echo -e "${BOLD}├──────────────┴─────────────┴─────────────┴─────────────┴───────────┴─────────────┤${RESET}"
printf "│ %-80s │\n" "Divergence efficiency (ideal = 50% of mandelbrot time)"
printf "│ %-80s │\n" "$EFF_LINE"
# NOTE: bash printf pads to a byte count, so keep every %-80s cell ASCII-only.
printf "│ %-80s │\n" "  packet: 4 pixels share one execution mask - a split packet runs both sides"
printf "│ %-80s │\n" "  scalar: each pixel branches on its own - the gap to 100% is fixed frame cost"
echo -e "${BOLD}└──────────────────────────────────────────────────────────────────────────────────┘${RESET}"

# A shader outside the packetizer's subset emits no @fs_packet: the runtime
# falls back to fs_invoke and both CPU columns measure the same path.
for a in mandelbrot diverge; do
    if [[ "$(rv_packetized "$a")" == "no" ]]; then
        echo -e "  ${YELLOW}note:${RESET} $a did not packetize — its two CPU columns are the same path"
    fi
done

# QEMU expands each RVV instruction into a scalar loop, so the packet path pays
# for its SoA gather/scatter without getting any vector width back.
if [[ "$RISCV_AVAIL" -eq 1 && "$NATIVE_RISCV" -eq 0 ]]; then
    echo -e "  ${YELLOW}note:${RESET} CPU runs under QEMU — a 'pkt gain' below 1.0x is an emulation"
    echo -e "        artifact, not the packetizer. See TESTING.md / bench_packet.sh."
fi


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
OVERHEAD_MS=$(gpu_ms_for mandelbrot.frag.spv overhead_1x1 "$OVERHEAD_FRAMES" 1 1)

echo -e "${CYAN}Running GPU at 512×512 (${VK_FRAMES} frames, reference)…${RESET}"
FULL_MS=$(gpu_ms_for mandelbrot.frag.spv overhead_512 "$VK_FRAMES" 512 512)

echo ""
echo -e "${BOLD}┌──────────────────────┬──────────────┬─────────────────────────────────┐${RESET}"
echo -e "${BOLD}│ Measurement          │ ms/frame     │ Notes                           │${RESET}"
echo -e "${BOLD}├──────────────────────┼──────────────┼─────────────────────────────────┤${RESET}"
printf "│ %-20s │ %12s │ %-31s │\n" \
    "Dispatch overhead" "${OVERHEAD_MS}ms" "1x1 px: compute ~ 0"
printf "│ %-20s │ %12s │ %-31s │\n" \
    "Mandelbrot 512x512" "${FULL_MS}ms" "real workload"

if [[ "$OVERHEAD_MS" != "N/A" && "$FULL_MS" != "N/A" ]]; then
    OVERHEAD_PCT=$(awk "BEGIN { printf \"%.1f%%\", $OVERHEAD_MS / $FULL_MS * 100 }")
    COMPUTE_MS=$(awk "BEGIN { printf \"%.4f\", $FULL_MS - $OVERHEAD_MS }")
    printf "│ %-20s │ %12s │ %-31s │\n" \
        "Pure compute" "${COMPUTE_MS}ms" "= full - overhead"
    printf "│ %-20s │ %12s │ %-31s │\n" \
        "Overhead fraction" "" "${OVERHEAD_PCT} of total frame time"
fi
echo -e "${BOLD}└──────────────────────┴──────────────┴─────────────────────────────────┘${RESET}"


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
DIV_64=$(gpu_ms_for  diverge.frag.spv  div64  "$BOUNDARY_FRAMES" 64  64)
DIV_256=$(gpu_ms_for diverge.frag.spv  div256 "$BOUNDARY_FRAMES" 256 256)
DIV_512=$(gpu_ms_for diverge.frag.spv  div512 "$BOUNDARY_FRAMES" 512 512)

echo -e "${CYAN}Running GPU mandelbrot at 64, 256, 512 (uniform baseline)…${RESET}"
MAN_64=$(gpu_ms_for  mandelbrot.frag.spv man64  "$BOUNDARY_FRAMES" 64  64)
MAN_256=$(gpu_ms_for mandelbrot.frag.spv man256 "$BOUNDARY_FRAMES" 256 256)
MAN_512=$(gpu_ms_for mandelbrot.frag.spv man512 "$BOUNDARY_FRAMES" 512 512)

echo ""
echo -e "${BOLD}┌─────────┬────────────────────┬────────────────────┬────────────┬──────────────────────┐${RESET}"
echo -e "${BOLD}│ Res     │ diverge ms/f       │ mandelbrot ms/f    │ Efficiency │ Boundary pixels      │${RESET}"
echo -e "${BOLD}├─────────┼────────────────────┼────────────────────┼────────────┼──────────────────────┤${RESET}"

for res in 64 256 512; do
    eval "div_ms=\$DIV_${res}"; eval "man_ms=\$MAN_${res}"
    eff=$(div_efficiency "$div_ms" "$man_ms")
    # boundary ≈ warp_width (32) columns × height pixels, as % of total
    boundary_pct=$(awk "BEGIN { printf \"~%.1f%%\", 32.0 / $res * 100 }")
    printf "│ %7s │ %18s │ %18s │ %10s │ %-20s │\n" \
        "${res}x${res}" "${div_ms}ms" "${man_ms}ms" "$eff" "$boundary_pct of pixels"
done

echo -e "${BOLD}└─────────┴────────────────────┴────────────────────┴────────────┴──────────────────────┘${RESET}"

