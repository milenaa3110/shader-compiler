#!/usr/bin/env bash
# run_benchmark_vertex.sh — build, run, and compare terrain animation (vertex+fragment, VK vs RV)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
source "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

mkdir -p result build/spirv build

# ── build shaders ─────────────────────────────────────────────────────────────
echo -e "${CYAN}Compiling terrain shaders…${RESET}"
make build/spirv/terrain.vert.spv build/spirv/terrain.frag.spv 2>/dev/null && \
    echo -e "  terrain.vert.spv + terrain.frag.spv  ${GREEN}OK${RESET}" || \
    echo -e "  SPIR-V compile  ${YELLOW}SKIP${RESET}"

make build/riscv/terrain_rv.o 2>/dev/null && \
    echo -e "  terrain_rv.o  ${GREEN}OK${RESET}" || \
    echo -e "  RISC-V compile  ${YELLOW}SKIP${RESET}"

# ── static metrics ────────────────────────────────────────────────────────────
spv_vert_kb="N/A"; spv_frag_kb="N/A"; rv_kb="N/A"; rv_instrs="N/A"
[[ -f build/spirv/terrain.vert.spv ]] && \
    spv_vert_kb=$(awk "BEGIN{printf \"%.1f\", $(wc -c < build/spirv/terrain.vert.spv)/1024}")
[[ -f build/spirv/terrain.frag.spv ]] && \
    spv_frag_kb=$(awk "BEGIN{printf \"%.1f\", $(wc -c < build/spirv/terrain.frag.spv)/1024}")
[[ -f build/riscv/terrain_rv.o ]] && \
    rv_kb=$(awk "BEGIN{printf \"%.1f\", $(wc -c < build/riscv/terrain_rv.o)/1024}")
[[ -f build/riscv/terrain_rv.o ]] && \
    rv_instrs=$(riscv64-linux-gnu-objdump -d build/riscv/terrain_rv.o 2>/dev/null \
        | grep -cE '^\s+[0-9a-f]+:' || echo "N/A")

# ── Vulkan GPU ────────────────────────────────────────────────────────────────
vk_ms="N/A"; vk_mp4="-"
if [[ -f build/spirv/terrain.vert.spv && -f build/spirv/terrain.frag.spv ]]; then
    echo -e "\n${CYAN}Running Vulkan (GPU)…${RESET}"
    vk_out=$(./build/spirv/spirv_vulkan_host \
        build/spirv/terrain.vert.spv build/spirv/terrain.frag.spv \
        terrain 60 512 512 6144 2>/dev/null || true)
    vk_ms=$(parse_avg "$vk_out")
    [[ -f result/terrain.mp4 ]] && vk_mp4="result/terrain.mp4"
fi

# ── RISC-V CPU ────────────────────────────────────────────────────────────────
rv_ms="N/A"; rv_mp4="-"
if [[ "$RISCV_AVAIL" -eq 0 ]]; then
    echo -e "  ${YELLOW}RISC-V: SKIP (no QEMU and not native RISC-V)${RESET}"
else
    make build/riscv/terrain.rv 2>/dev/null
    echo -e "\n${CYAN}Running RISC-V (${NTHREADS} threads)…${RESET}"
    rv_out=$(OMP_NUM_THREADS="$NTHREADS" $RISCV_SIM \
        ./build/riscv/terrain.rv 2>/dev/null || true)
    rv_ms=$(parse_avg "$rv_out")
    [[ -f result/terrain_rv.mp4 ]] && rv_mp4="result/terrain_rv.mp4"
fi

# ── comparison table ──────────────────────────────────────────────────────────
speedup=$(speedup_label "$vk_ms" "$rv_ms")

echo ""
echo -e "${BOLD}┌──────────────────────┬──────────────────┬──────────────────┐${RESET}"
echo -e "${BOLD}│ Metric               │ GPU (Vulkan)     │ CPU (RISC-V OMP) │${RESET}"
echo -e "${BOLD}├──────────────────────┼──────────────────┼──────────────────┤${RESET}"
printf  "│ %-20s │ %16s │ %16s │\n" "ms / frame"   "${vk_ms}ms"              "${rv_ms}ms"
printf  "│ %-20s │ %16s │ %16s │\n" "speedup"      "1x (baseline)"          "${speedup} slower"
printf  "│ %-20s │ %16s │ %16s │\n" "SPIR-V VS KB" "${spv_vert_kb} KB"      "—"
printf  "│ %-20s │ %16s │ %16s │\n" "SPIR-V FS KB" "${spv_frag_kb} KB"      "—"
printf  "│ %-20s │ %16s │ %16s │\n" "RV .o KB"     "—"                      "${rv_kb} KB"
printf  "│ %-20s │ %16s │ %16s │\n" "RV instructions" "—"                   "${rv_instrs}"
printf  "│ %-20s │ %16s │ %16s │\n" "resolution"   "512×512"                "512×512"
printf  "│ %-20s │ %16s │ %16s │\n" "frames"       "60"                     "60"
printf  "│ %-20s │ %16s │ %16s │\n" "vertices"     "6144"                   "6144"
echo -e "${BOLD}├──────────────────────┼──────────────────┼──────────────────┤${RESET}"
printf  "│ %-20s │ %16s │ %16s │\n" "MP4 output"   "$vk_mp4"               "$rv_mp4"
echo -e "${BOLD}└──────────────────────┴──────────────────┴──────────────────┘${RESET}"
