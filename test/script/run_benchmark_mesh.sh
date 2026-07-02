#!/usr/bin/env bash
# run_benchmark_mesh.sh — build, run, and compare the indexed-mesh pipeline
# (VS skinning-free transform + textured FS) for the "boss" model on GPU vs RV.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
source "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

mkdir -p result build/spirv build

# Benchmark target: the textured Mixamo "boss" OBJ (filename has a space).
MESH_OBJ="$ROOT/test/assets/boss/Rumba Dancing.obj"
FRAMES=60
RES=768
TRIS=10220
VERTS=30660

# ── build shaders ─────────────────────────────────────────────────────────────
echo -e "${CYAN}Compiling mesh shaders…${RESET}"
build_target mesh.vert.spv mesh.frag.spv 2>/dev/null && \
    echo -e "  mesh.vert.spv + mesh.frag.spv  ${GREEN}OK${RESET}" || \
    echo -e "  SPIR-V compile  ${YELLOW}SKIP${RESET}"

build_target mesh_rv.o 2>/dev/null && \
    echo -e "  mesh_rv.o  ${GREEN}OK${RESET}" || \
    echo -e "  RISC-V compile  ${YELLOW}SKIP${RESET}"

# ── static metrics ────────────────────────────────────────────────────────────
spv_vert_kb="N/A"; spv_frag_kb="N/A"; rv_kb="N/A"; rv_instrs="N/A"
[[ -f build/spirv/mesh.vert.spv ]] && \
    spv_vert_kb=$(awk "BEGIN{printf \"%.1f\", $(wc -c < build/spirv/mesh.vert.spv)/1024}")
[[ -f build/spirv/mesh.frag.spv ]] && \
    spv_frag_kb=$(awk "BEGIN{printf \"%.1f\", $(wc -c < build/spirv/mesh.frag.spv)/1024}")
[[ -f build/riscv/mesh_rv.o ]] && \
    rv_kb=$(awk "BEGIN{printf \"%.1f\", $(wc -c < build/riscv/mesh_rv.o)/1024}")
[[ -f build/riscv/mesh_rv.o ]] && \
    rv_instrs=$($OBJDUMP -d build/riscv/mesh_rv.o 2>/dev/null \
        | grep -cE '^\s+[0-9a-f]+:' || echo "N/A")

# ── Vulkan GPU ────────────────────────────────────────────────────────────────
# The host needs a working Vulkan ICD (real GPU driver or software LavaPipe). On
# boards without one (e.g. a bare RISC-V SBC), it can't open a device — surface
# that reason instead of silently reporting N/A.
vk_ms="N/A"; vk_mp4="-"; vk_reason=""
if [[ ! -f build/spirv/mesh.vert.spv || ! -f build/spirv/mesh.frag.spv ]]; then
    vk_reason="SPIR-V not built"
elif [[ ! -x build/spirv/spirv_vulkan_mesh_host ]]; then
    vk_reason="host not built (build/spirv/spirv_vulkan_mesh_host missing)"
else
    echo -e "\n${CYAN}Running Vulkan (GPU)…${RESET}"
    vk_err=$(mktemp)
    if vk_out=$(./build/spirv/spirv_vulkan_mesh_host \
            build/spirv/mesh.vert.spv build/spirv/mesh.frag.spv \
            boss "$FRAMES" "$MESH_OBJ" 2>"$vk_err"); then
        vk_ms=$(parse_avg "$vk_out")
        [[ "$vk_ms" == "N/A" ]] && vk_reason="ran but no 'avg:' in output"
    else
        vk_reason="host exited $? — $(grep -iE 'vulkan|vk|device|error' "$vk_err" | tail -1)"
        vk_reason="${vk_reason%% — }"
    fi
    rm -f "$vk_err"
    [[ -f result/boss.mp4 ]] && vk_mp4="result/boss.mp4"
fi
[[ "$vk_ms" == "N/A" && -n "$vk_reason" ]] && \
    echo -e "  ${YELLOW}Vulkan unavailable: ${vk_reason}${RESET}"

# ── RISC-V CPU ────────────────────────────────────────────────────────────────
rv_ms="N/A"; rv_mp4="-"
if [[ "$RISCV_AVAIL" -eq 0 ]]; then
    echo -e "  ${YELLOW}RISC-V: SKIP (no QEMU and not native RISC-V)${RESET}"
else
    build_target boss.rv 2>/dev/null
    echo -e "\n${CYAN}Running RISC-V (${NTHREADS} threads)…${RESET}"
    rv_out=$(OMP_NUM_THREADS="$NTHREADS" $RISCV_SIM \
        ./build/riscv/boss.rv boss "$FRAMES" "$MESH_OBJ" 2>/dev/null || true)
    rv_ms=$(parse_avg "$rv_out")
    [[ -f result/boss_rv.mp4 ]] && rv_mp4="result/boss_rv.mp4"
fi

# ── comparison table ──────────────────────────────────────────────────────────
# Only claim a baseline/speedup when BOTH sides produced a number; otherwise the
# comparison is meaningless (e.g. GPU unavailable on the board).
speedup=$(speedup_label "$vk_ms" "$rv_ms")
if [[ "$vk_ms" == "N/A" || "$rv_ms" == "N/A" ]]; then
    gpu_speed="—"; cpu_speed="—"
else
    gpu_speed="1x (baseline)"; cpu_speed="${speedup} slower"
fi

echo ""
echo -e "${BOLD}┌──────────────────────┬──────────────────┬──────────────────┐${RESET}"
echo -e "${BOLD}│ Metric               │ GPU (Vulkan)     │ CPU (RISC-V OMP) │${RESET}"
echo -e "${BOLD}├──────────────────────┼──────────────────┼──────────────────┤${RESET}"
printf  "│ %-20s │ %16s │ %16s │\n" "ms / frame"      "${vk_ms}ms"          "${rv_ms}ms"
printf  "│ %-20s │ %16s │ %16s │\n" "speedup"         "${gpu_speed}"        "${cpu_speed}"
printf  "│ %-20s │ %16s │ %16s │\n" "SPIR-V VS KB"    "${spv_vert_kb} KB"   "—"
printf  "│ %-20s │ %16s │ %16s │\n" "SPIR-V FS KB"    "${spv_frag_kb} KB"   "—"
printf  "│ %-20s │ %16s │ %16s │\n" "RV .o KB"        "—"                   "${rv_kb} KB"
printf  "│ %-20s │ %16s │ %16s │\n" "RV instructions" "—"                   "${rv_instrs}"
printf  "│ %-20s │ %16s │ %16s │\n" "resolution"      "${RES}×${RES}"       "${RES}×${RES}"
printf  "│ %-20s │ %16s │ %16s │\n" "frames"          "$FRAMES"             "$FRAMES"
printf  "│ %-20s │ %16s │ %16s │\n" "triangles"       "$TRIS"               "$TRIS"
printf  "│ %-20s │ %16s │ %16s │\n" "vertices"        "$VERTS"              "$VERTS"
echo -e "${BOLD}├──────────────────────┼──────────────────┼──────────────────┤${RESET}"
printf  "│ %-20s │ %16s │ %16s │\n" "MP4 output"      "$vk_mp4"             "$rv_mp4"
echo -e "${BOLD}└──────────────────────┴──────────────────┴──────────────────┘${RESET}"
