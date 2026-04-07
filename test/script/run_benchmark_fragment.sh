#!/usr/bin/env bash
# run_benchmark_fragment.sh — CPU (RISC-V + OpenMP via QEMU) vs GPU (Vulkan SPIR-V) fragment shader benchmark
#
# Metrics reported:
#   - Speed: ms/frame for both backends
#   - Binary size: SPIR-V .spv KB vs RISC-V .o KB
#   - Instruction count: from riscv64-linux-gnu-objdump
#   - Speedup factor: CPU ms / GPU ms
#   - MP4 paths for visual comparison
#
# Usage: bash test/script/run_benchmark_fragment.sh [OPTIONS]
#   --rv-only      Skip Vulkan, only run RISC-V
#   --vk-only      Skip RISC-V, only run Vulkan
#   --quick        Fewer frames (faster run)
#   --render       (no-op, kept for compatibility — render is always on)
#   --bench-only   Timing only, skip MP4 generation
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
source "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

# ── config ────────────────────────────────────────────────────────────────────
ANIMATIONS=(mandelbrot julia voronoi waves tunnel ripple galaxy fire reaction cellular earth scene3d)
VK_FRAMES=60
VK_WIDTH=512
VK_HEIGHT=512
VK_HOST="./build/spirv/spirv_vulkan_host"

# ── detect simulators ─────────────────────────────────────────────────────────
# QEMU: used for all rendering + OpenMP benchmark timing
if [[ "$RISCV_AVAIL" -eq 1 ]]; then
    SIM_RENDER="$RISCV_SIM"
    SIM_NAME="$([[ "$NATIVE_RISCV" -eq 1 ]] && echo "native+OpenMP(${NTHREADS}t)" || echo "QEMU+OpenMP(${NTHREADS}t)")"
    RV_WIDTH=512
    RV_HEIGHT=512
else
    SIM_RENDER=""
    SIM_NAME="none"
    RV_WIDTH=512
    RV_HEIGHT=512
fi

# ── flags ─────────────────────────────────────────────────────────────────────
RV_ONLY=0; VK_ONLY=0; QUICK=0; RENDER=1; BENCH_ONLY=0
for arg in "$@"; do
    case $arg in
        --rv-only)    RV_ONLY=1 ;;
        --vk-only)    VK_ONLY=1 ;;
        --quick)      QUICK=1; VK_FRAMES=10 ;;
        --render)     RENDER=1 ;;
        --bench-only) BENCH_ONLY=1; RENDER=0 ;;
    esac
done

mkdir -p result "$BUILD_DIR"

# ── build check ───────────────────────────────────────────────────────────────
echo -e "${CYAN}Building tools…${RESET}"
make -j"$(nproc)" build/riscv/irgen_riscv build/spirv/spirv_vulkan_host 2>/dev/null | grep -E "^g\+\+|^riscv|error" || true

# ── compile SPIR-V shaders ────────────────────────────────────────────────────
if [[ $RV_ONLY -eq 0 ]]; then
    echo -e "${CYAN}Compiling Vulkan SPIR-V shaders…${RESET}"
    make build/spirv/quad.vert.spv 2>/dev/null
    for A in "${ANIMATIONS[@]}"; do
        make "build/spirv/${A}.frag.spv" 2>/dev/null && \
            echo -e "  ${A}.frag.spv  ${GREEN}OK${RESET}" || \
            echo -e "  ${A}.frag.spv  ${YELLOW}SKIP${RESET}"
    done
fi

# ── compile RISC-V objects ────────────────────────────────────────────────────
if [[ $VK_ONLY -eq 0 ]]; then
    echo -e "${CYAN}Compiling RISC-V shaders (irgen_riscv + llc-18)…${RESET}"
    for A in "${ANIMATIONS[@]}"; do
        make "build/riscv/${A}_rv.o" 2>/dev/null && \
            echo -e "  build/riscv/${A}_rv.o  ${GREEN}OK${RESET}" || \
            echo -e "  build/riscv/${A}_rv.o  ${YELLOW}SKIP${RESET}"
    done
fi

# ── helper: get file size in KB ───────────────────────────────────────────────
file_kb() { local f="$1"; [[ -f "$f" ]] && awk "BEGIN{printf \"%.1f\", $(wc -c < "$f") / 1024}" || echo "N/A"; }

# ── helper: count RISC-V instructions from objdump ───────────────────────────
rv_instr_count() {
    local obj="$1"
    [[ -f "$obj" ]] || { echo "N/A"; return; }
    $OBJDUMP -d "$obj" 2>/dev/null \
        | grep -cE '^\s+[0-9a-f]+:' || echo "N/A"
}

# ── results collection ────────────────────────────────────────────────────────
declare -A RV_MS VK_MS RV_INSTRS SPV_KB RV_KB RV_MP4 VK_MP4

echo ""
echo -e "${BOLD}Running benchmarks…${RESET}"
echo ""

for A in "${ANIMATIONS[@]}"; do
    echo -e "${CYAN}── ${A} ──────────────────────────────────────────${RESET}"

    # ── static metrics (don't require running) ────────────────────────────────
    SPV_KB[$A]="$(file_kb "build/spirv/${A}.frag.spv")"
    RV_KB[$A]="$(file_kb "build/riscv/${A}_rv.o")"
    RV_INSTRS[$A]="$(rv_instr_count "build/riscv/${A}_rv.o")"

    echo -e "  SPIR-V size  : ${SPV_KB[$A]} KB   RV .o size: ${RV_KB[$A]} KB   RV instrs: ${RV_INSTRS[$A]}"

    # ── Vulkan GPU ────────────────────────────────────────────────────────────
    if [[ $RV_ONLY -eq 0 ]]; then
        if [[ -f "build/spirv/${A}.frag.spv" ]]; then
            vk_out=$("$VK_HOST" "build/spirv/quad.vert.spv" "build/spirv/${A}.frag.spv" "${A}" "$VK_FRAMES" "$VK_WIDTH" "$VK_HEIGHT" 2>/dev/null || true)
            vk_avg=$(parse_avg "$vk_out")
            VK_MS[$A]="$vk_avg"
            vk_mp4="result/${A}.mp4"
            VK_MP4[$A]="$([[ -f "$vk_mp4" ]] && echo "$vk_mp4" || echo "-")"
            echo -e "  GPU (Vulkan) : ${GREEN}${vk_avg} ms/frame${RESET}  →  ${VK_MP4[$A]}"
        else
            VK_MS[$A]="N/A"; VK_MP4[$A]="-"
            echo -e "  GPU (Vulkan) : ${YELLOW}SKIP (no .spv)${RESET}"
        fi
    fi

    # ── RISC-V CPU + OpenMP ───────────────────────────────────────────────────
    if [[ $VK_ONLY -eq 0 ]]; then
        if [[ -z "$SIM_RENDER" ]]; then
            RV_MS[$A]="N/A"; RV_MP4[$A]="-"
            echo -e "  CPU (RISC-V) : ${YELLOW}SKIP (no RISC-V runtime — install qemu-user-static or run on RISC-V hardware)${RESET}"
        elif [[ -f "build/riscv/${A}_rv.o" ]]; then
            # Build rv binary with OpenMP
            $CROSS_CXX -std=c++20 -O3 -static -fopenmp -Ipipeline \
                -DANIM_NAME="\"${A}\"" -DNFRAMES="${VK_FRAMES}" -DFPS=30 \
                -DWIDTH="${RV_WIDTH}" -DHEIGHT="${RV_HEIGHT}" \
                test/rv_host/rv_host_fragment.cpp \
                pipeline/pipeline_runtime.cpp \
                "build/riscv/${A}_rv.o" -o "build/riscv/${A}_bench.rv" 2>/dev/null

            echo -n "  CPU (RISC-V) : running on ${SIM_NAME} (${RV_WIDTH}x${RV_HEIGHT}, ${VK_FRAMES} frame(s))…"
            rv_out=$(OMP_NUM_THREADS="${NTHREADS}" $SIM_RENDER "./build/riscv/${A}_bench.rv" 2>/dev/null || true)
            rv_avg=$(parse_avg "$rv_out")
            RV_MS[$A]="$rv_avg"
            rv_mp4="result/${A}_rv.mp4"
            RV_MP4[$A]="$([[ -f "$rv_mp4" ]] && echo "$rv_mp4" || echo "-")"
            echo -e "  ${YELLOW}${rv_avg} ms/frame${RESET}  →  ${RV_MP4[$A]}"
        else
            RV_MS[$A]="N/A"; RV_MP4[$A]="-"
            echo -e "  CPU (RISC-V) : ${YELLOW}SKIP (no build/riscv/${A}_rv.o)${RESET}"
        fi
    fi

done

# ── summary table ─────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}┌─────────────┬──────────┬──────────────────┬─────────┬──────────┬──────────┬───────────────┐${RESET}"
echo -e "${BOLD}│ Animation   │ GPU ms/f │ CPU ms/f (OMP)   │ Speedup │ SPIR-V   │ RV .o    │ RV instrs     │${RESET}"
echo -e "${BOLD}├─────────────┼──────────┼──────────────────┼─────────┼──────────┼──────────┼───────────────┤${RESET}"

for A in "${ANIMATIONS[@]}"; do
    vk="${VK_MS[$A]:-N/A}"
    rv="${RV_MS[$A]:-N/A}"
    spv_sz="${SPV_KB[$A]:-N/A}"
    rv_sz="${RV_KB[$A]:-N/A}"
    instrs="${RV_INSTRS[$A]:-N/A}"

    if [[ "$vk" != "N/A" && "$rv" != "N/A" ]]; then
        speedup=$(awk "BEGIN { printf \"%.0fx\", $rv / $vk }")
    else
        speedup="N/A"
    fi

    printf "│ %-11s │ %8s │ %-16s │ %-7s │ %6s KB │ %6s KB │ %-13s │\n" \
        "$A" "${vk}ms" "${rv}ms" "$speedup" "$spv_sz" "$rv_sz" "$instrs"
done

echo -e "${BOLD}└─────────────┴──────────┴──────────────────┴─────────┴──────────┴──────────┴───────────────┘${RESET}"

# ── MP4 summary ───────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}MP4 output:${RESET}"
echo -e "  ${CYAN}GPU (Vulkan):${RESET}"
for A in "${ANIMATIONS[@]}"; do
    mp4="${VK_MP4[$A]:-}"; [[ "$mp4" != "-" && -n "$mp4" ]] && echo "    $mp4"
done
echo -e "  ${YELLOW}CPU (RISC-V+OpenMP):${RESET}"
for A in "${ANIMATIONS[@]}"; do
    mp4="${RV_MP4[$A]:-}"; [[ "$mp4" != "-" && -n "$mp4" ]] && echo "    $mp4"
done

