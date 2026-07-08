#!/usr/bin/env bash
# compare_backends.sh — static, hardware-independent structural comparison of the
# RISC-V and SPIR-V backends.
#
# Both descend from the same LLVM IR, so this is the fairest basis: "from
# identical intermediate code, each backend produces this." No QEMU, hardware, or
# GPU involved — every number is read off the compiled artifacts. Per shader it
# reports, side by side:
#   - SPIR-V: .spv bytes / words / opcodes (opcodes from spv_count.py, a
#     dependency-free word-stream parser, since spirv-tools isn't in the build).
#     A portable intermediate the GPU driver lowers *further*.
#   - RISC-V: .text bytes (llvm-size on <name>_rv.o, the shader module — NOT the
#     .rv executable, which statically links runtime + libc and so isn't
#     comparable) / instruction count / RVV vector-FP ops (objdump).
# These sit at different abstraction levels, so size reflects representation
# *density*, not quality: one SPIR-V opcode (e.g. OpExtInst sin) is a library
# call or expanded sequence in RISC-V — read trends across the set, not per-row
# equality. A closing section contrasts the parallelization models (two-level
# CPU: OpenMP threads + RVV/SPMD lanes, vs SIMT delegated to the GPU) and prints
# the shared measurable axis, the compute workgroup size. Registered as the
# compare_backends ctest (skipped if python3 is absent).
#   Usage: bash test/script/compare_backends.sh [--csv]   (--csv = machine-readable)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
SPV_DIR="$ROOT/build/spirv"
RV_DIR="$ROOT/build/riscv"

CSV=0
[[ "${1:-}" == "--csv" ]] && CSV=1

# Pick a RISC-V-capable disassembler. On a riscv64 host (e.g. Banana Pi F3) the
# native objdump handles riscv; cross-hosting (x86) needs the cross objdump or
# llvm-objdump — never plain `objdump`, which would be the wrong architecture.
if [[ "$(uname -m)" == "riscv64" ]]; then
    OBJDUMP="$(command -v objdump || command -v llvm-objdump-18 || command -v llvm-objdump)"
else
    OBJDUMP="$(command -v riscv64-linux-gnu-objdump || command -v llvm-objdump-18 || command -v llvm-objdump)"
fi
SIZE="$(command -v llvm-size || command -v size)"
PY="$(command -v python3)"
SPVC="$SCRIPT_DIR/spv_count.py"

for tool_pair in "OBJDUMP:$OBJDUMP" "SIZE:$SIZE" "PY:$PY"; do
    if [[ -z "${tool_pair#*:}" ]]; then
        echo "compare_backends: missing required tool (${tool_pair%%:*}). Need" >&2
        echo "  objdump (or llvm-objdump), llvm-size (or binutils size), and python3." >&2
        exit 1
    fi
done

BOLD="\033[1m"; DIM="\033[2m"; CYAN="\033[36m"; RST="\033[0m"

#  SPIR-V metrics: "<bytes> <words> <instr> <lx> <ly> <lz>" ─
# Sums multiple .spv files (a graphics pipeline = vert + frag) component-wise.
spv_metrics() {
    local b=0 w=0 n=0 lx=0 ly=0 lz=0
    for f in "$@"; do
        [[ -f "$f" ]] || { echo "MISSING"; return 1; }
        read -r fb fw fn flx fly flz < <("$PY" "$SPVC" "$f")
        b=$((b+fb)); w=$((w+fw)); n=$((n+fn))
        # A pipeline declares LocalSize only on its compute stage, if any.
        (( flx > 0 )) && { lx=$flx; ly=$fly; lz=$flz; }
    done
    echo "$b $w $n $lx $ly $lz"
}

#  RISC-V metrics: "<text_bytes> <instr> <vec_fp> <scalar_fp>" 
rv_metrics() {
    local obj="$1"
    [[ -f "$obj" ]] || { echo "MISSING"; return 1; }
    local text d total vfp sfp
    text=$("$SIZE" "$obj" 2>/dev/null | awk 'NR==2{print $1}')
    d=$("$OBJDUMP" -d "$obj" 2>/dev/null)
    total=$(grep -cE '^[[:space:]]+[0-9a-f]+:' <<<"$d")
    vfp=$(grep -cE '\bvf[a-z]+\.' <<<"$d")
    sfp=$(grep -cE '\bfn?(add|mul|sub|div|madd|msub|max|min|sqrt|cvt|sgnj)\.[sd]\b' <<<"$d")
    echo "${text:-0} ${total:-0} ${vfp:-0} ${sfp:-0}"
}

#  shader sets 
# Fragment shaders whose <name>_rv.o is the fragment alone (1:1 with .frag.spv).
FRAG=(cellular city diverge earth fire galaxy julia mandelbrot ocean
      reaction ripple scene3d tunnel voronoi waves)
# Compute shaders: <name>.comp.spv  vs  <name>_cs_rv.o.
COMP=(blur life)
# Graphics pipelines: vert+frag .spv  vs  one bundled <name>_rv.o.
PIPE=(mesh terrain)

# Collected for the optional CSV and the trend totals.
declare -a CSV_ROWS
SPV_INSTR_TOTAL=0; RV_INSTR_TOTAL=0; RV_VEC_TOTAL=0; COMPARED=0

row() {  # name stage  spvbytes spvwords spvinstr  rvtext rvinstr rvvec rvscalar
    local name="$1" stage="$2"
    local sb="$3" sw="$4" si="$5" rt="$6" ri="$7" rv="$8" rs="$9"
    CSV_ROWS+=("$name,$stage,$sb,$sw,$si,$rt,$ri,$rv,$rs")
    COMPARED=$((COMPARED+1))
    SPV_INSTR_TOTAL=$((SPV_INSTR_TOTAL+si))
    RV_INSTR_TOTAL=$((RV_INSTR_TOTAL+ri))
    RV_VEC_TOTAL=$((RV_VEC_TOTAL+rv))
    if [[ "$CSV" -eq 0 ]]; then
        printf "│ %-11s │ %-5s │ %8s %7s %7s │ %9s %8s %8s │\n" \
            "$name" "$stage" "$sb" "$sw" "$si" "$rt" "$ri" "$rv"
    fi
}

emit_one() {  # name stage  obj  spvfile...
    local name="$1" stage="$2" obj="$3"; shift 3
    local sm rm
    sm=$(spv_metrics "$@")       || { echo -e "  ${DIM}skip $name (.spv missing)${RST}" >&2; return; }
    rm=$(rv_metrics "$obj")      || { echo -e "  ${DIM}skip $name ($obj missing)${RST}" >&2; return; }
    read -r sb sw si _ _ _ <<<"$sm"
    read -r rt ri rv rs     <<<"$rm"
    row "$name" "$stage" "$sb" "$sw" "$si" "$rt" "$ri" "$rv" "$rs"
}

#  table ─
if [[ "$CSV" -eq 0 ]]; then
    echo -e "${BOLD}━━━ Backend representation: same LLVM IR → SPIR-V vs RISC-V (static) ━━━${RST}"
    echo -e "${DIM}SPIR-V = portable intermediate (driver lowers further); RISC-V = final"
    echo -e "machine code. Different abstraction levels — read trends, not per-row equality.${RST}"
    echo -e "${BOLD}┌─────────────┬───────┬──────────────────────────┬─────────────────────────────┐${RST}"
    echo -e "${BOLD}│             │       │        SPIR-V (.spv)     │        RISC-V (_rv.o .text) │${RST}"
    echo -e "${BOLD}│ shader      │ stage │   bytes   words opcodes  │     bytes    insns  vec-fp  │${RST}"
    echo -e "${BOLD}├─────────────┼───────┼──────────────────────────┼─────────────────────────────┤${RST}"
fi

for s in "${FRAG[@]}"; do emit_one "$s" "frag" "$RV_DIR/${s}_rv.o"    "$SPV_DIR/${s}.frag.spv"; done
for s in "${PIPE[@]}"; do emit_one "$s" "vs+fs" "$RV_DIR/${s}_rv.o"   "$SPV_DIR/${s}.vert.spv" "$SPV_DIR/${s}.frag.spv"; done
for s in "${COMP[@]}"; do emit_one "$s" "comp" "$RV_DIR/${s}_cs_rv.o" "$SPV_DIR/${s}.comp.spv"; done

if [[ "$COMPARED" -eq 0 ]]; then
    echo "compare_backends: no built artifacts found. Build them first, e.g.:" >&2
    echo "  cmake --build build --target compare-backends" >&2
    exit 1
fi

if [[ "$CSV" -eq 1 ]]; then
    echo "shader,stage,spv_bytes,spv_words,spv_opcodes,rv_text_bytes,rv_insns,rv_vec_fp,rv_scalar_fp"
    printf '%s\n' "${CSV_ROWS[@]}"
    exit 0
fi

echo -e "${BOLD}└─────────────┴───────┴──────────────────────────┴─────────────────────────────┘${RST}"
echo -e "${DIM}totals across set:  SPIR-V opcodes=${SPV_INSTR_TOTAL}   RISC-V insns=${RV_INSTR_TOTAL}   RVV vector-FP ops=${RV_VEC_TOTAL}${RST}"
echo ""

#  parallelization model (architectural, not a single number) ─
echo -e "${BOLD}━━━ Parallelization model ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
echo -e "${CYAN}RISC-V (CPU), two levels:${RST}"
echo    "  • OpenMP over tiles/rows  — thread scaling; this is TIME, measured on"
echo    "    hardware (run_cpu_scaling.sh), not statically."
echo    "  • RVV / SPMD over lanes   — width kW=4 (packetizer) and VLEN; this IS"
echo    "    static: the 'vec-fp' column above is its direct evidence."
echo -e "${CYAN}SPIR-V (GPU): SIMT.${RST}"
echo    "  Parallelism is delegated to the GPU, so the .spv carries no lane count."
echo ""
echo -e "${CYAN}Common, measurable axis — compute workgroup size${RST} (same shader.workgroup_size"
echo    "metadata → SPIR-V LocalSize, → RISC-V cs_dispatch):"
for s in "${COMP[@]}"; do
    read -r _ _ _ lx ly lz <<<"$(spv_metrics "$SPV_DIR/${s}.comp.spv")"
    printf "  • %-6s  LocalSize = %s × %s × %s\n" "$s" "$lx" "$ly" "$lz"
done
echo ""
echo -e "${DIM}These are descriptive structural measures: they show how each backend"
echo -e "represents and parallelizes the same shader — not which is faster.${RST}"
