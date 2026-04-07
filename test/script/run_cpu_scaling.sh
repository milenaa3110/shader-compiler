#!/usr/bin/env bash
# run_cpu_scaling.sh — OpenMP thread scaling analysis (RISC-V via QEMU)
#
# Three workloads × four analyses:
#
#  Workloads:
#    mandelbrot  — uniform iteration count, embarrassingly parallel
#    diverge     — 50/50 split light/heavy pixels; dynamic scheduling balances load
#    life 256×256 — parallel within each generation, serial between generations
#
#  Analyses:
#    1. Thread scaling    — speedup & efficiency at 1,2,4,8 threads
#    2. Amdahl fit        — estimate serial fraction from T(1) and T(max)
#    3. Cache-size effect — life scaling at 32×32 (L1), 128×128 (L2), 256×256 (L3)
#    4. Scheduling effect — mandelbrot/diverge: static vs dynamic row scheduling
#    5. RVV vector width  — VLEN=128/256/512: runtime probe, timing, instr count
#
# Usage: bash test/script/run_cpu_scaling.sh [--quick] [--rvv-only]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
source "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

QUICK=0
RVV_ONLY=0
for arg in "$@"; do case $arg in --quick) QUICK=1 ;; --rvv-only) RVV_ONLY=1 ;; esac; done

MAX_THREADS="$NTHREADS"

if [[ "$RISCV_AVAIL" -eq 0 ]]; then
    echo "No RISC-V runtime found — install qemu-user-static or run on RISC-V hardware."; exit 1
fi

# Frame/gen counts — fewer for quick mode
FRAG_FRAMES=$([[ $QUICK -eq 1 ]] && echo 4  || echo 8)
FRAG_W=256; FRAG_H=256
LIFE_GENS=$([[ $QUICK -eq 1 ]] && echo 200 || echo 600)

# Thread counts to sweep (powers of 2 up to nproc)
THREAD_COUNTS=()
t=1; while (( t <= MAX_THREADS )); do THREAD_COUNTS+=("$t"); t=$((t*2)); done
# Include max if it isn't already a power of 2
(( MAX_THREADS > ${THREAD_COUNTS[-1]} )) && THREAD_COUNTS+=("$MAX_THREADS")

mkdir -p result "$BUILD_DIR"

if [[ "$RVV_ONLY" -eq 0 ]]; then

echo -e "${CYAN}Building objects and binaries…${RESET}"
make -j"$(nproc)" build/riscv/irgen_riscv >/dev/null 2>&1 || true
make build/riscv/mandelbrot_rv.o build/riscv/diverge_rv.o >/dev/null 2>&1 || true

# ── helper: build a fragment-shader bench binary ──────────────────────────────
build_frag() {
    local anim="$1" sched="$2" out="$3"   # sched: dynamic or static
    # Patch schedule in a temp copy of pipeline_runtime.cpp
    local tmp_rt="$BUILD_DIR/_tmp_rt_${sched}.cpp"
    sed "s/schedule(dynamic, 1)/schedule(${sched}, 1)/" \
        pipeline/pipeline_runtime.cpp > "$tmp_rt"
    $CROSS_CXX -std=c++20 -O3 -static -fopenmp -Ipipeline \
        -DANIM_NAME="\"${anim}\"" -DNFRAMES="$FRAG_FRAMES" \
        -DWIDTH="$FRAG_W" -DHEIGHT="$FRAG_H" \
        test/rv_host/rv_host_fragment.cpp \
        "$tmp_rt" \
        "build/riscv/${anim}_rv.o" -o "$out" >/dev/null 2>&1
    rm -f "$tmp_rt"
}

# ── helper: build a life binary ───────────────────────────────────────────────
build_life() {
    local grid="$1" out="$2"
    $CROSS_CXX -std=c++20 -O3 -static -fopenmp \
        -DGRID="$grid" -DNGENERATIONS="$LIFE_GENS" \
        test/rv_host/rv_host_compute.cpp \
        build/riscv/life_cs_rv.o -o "$out" >/dev/null 2>&1
}

# ── helper: run binary with T threads, return avg ms ─────────────────────────
run_rv() {
    local bin="$1" threads="$2"
    local out
    out=$(OMP_NUM_THREADS="$threads" $RISCV_SIM "$bin" 2>/dev/null || true)
    echo "$out" | grep -oE '(RISC-V|life-cpu).*avg: [0-9]+\.[0-9]+' \
        | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "N/A"
}

# ── helper: speedup and efficiency ───────────────────────────────────────────
speedup() { awk "BEGIN { printf \"%.2f\", $1 / $2 }"; }
efficiency() { awk "BEGIN { printf \"%.0f%%\", ($1 / $2) / $3 * 100 }"; }

# ── helper: Amdahl serial fraction from T(1) and T(N) ────────────────────────
# s = (1/speedup - 1/N) / (1 - 1/N)
amdahl_s() {
    local t1="$1" tN="$2" N="$3"
    awk "BEGIN {
        sp = $t1 / $tN
        s  = (1.0/sp - 1.0/$N) / (1.0 - 1.0/$N)
        if (s < 0) s = 0
        printf \"%.1f%%\", s * 100
    }"
}

# ── helper: Amdahl predicted speedup ─────────────────────────────────────────
amdahl_pred() {
    local s_pct="$1" N="$2"   # s_pct is the percentage string like "3.2%"
    awk "BEGIN {
        s = ${s_pct/\%/} / 100.0
        printf \"%.2fx\", 1.0 / (s + (1.0-s)/$N)
    }"
}

# ── print one scaling table ───────────────────────────────────────────────────
print_scaling_table() {
    local label="$1" bin="$2" t1_ms="$3"
    shift 3
    local times=("$@")   # indexed same as THREAD_COUNTS

    echo -e "${BOLD}┌──────────┬────────────┬──────────┬────────────┬──────────────┐${RESET}"
    echo -e "${BOLD}│ Threads  │ ms         │ Speedup  │ Efficiency │ vs Amdahl    │${RESET}"
    echo -e "${BOLD}├──────────┼────────────┼──────────┼────────────┼──────────────┤${RESET}"

    # Estimate serial fraction from T(1) and T(max)
    local tmax="${times[-1]}"
    local N_max="${THREAD_COUNTS[-1]}"
    local s_frac
    s_frac=$(amdahl_s "$t1_ms" "$tmax" "$N_max")

    for i in "${!THREAD_COUNTS[@]}"; do
        local T="${THREAD_COUNTS[$i]}"
        local ms="${times[$i]}"
        if [[ "$ms" == "N/A" ]]; then
            printf "│ %8s │ %10s │ %8s │ %10s │ %12s │\n" "$T" "N/A" "—" "—" "—"
            continue
        fi
        local sp eff pred
        sp=$(speedup "$t1_ms" "$ms")
        eff=$(efficiency "$t1_ms" "$ms" "$T")
        pred=$(amdahl_pred "$s_frac" "$T")
        printf "│ %8s │ %10s │ %8sx │ %10s │ %12s │\n" \
            "$T" "${ms}ms" "$sp" "$eff" "pred ${pred}"
    done

    echo -e "${BOLD}└──────────┴────────────┴──────────┴────────────┴──────────────┘${RESET}"
    echo -e "  Serial fraction (Amdahl fit from T=1 and T=${N_max}): ${BOLD}${s_frac}${RESET}"
}

mkdir -p result

# ══════════════════════════════════════════════════════════════════════════════
# 1. THREAD SCALING — fragment shaders
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}━━━ 1. Thread Scaling — Fragment Shaders (${FRAG_W}×${FRAG_H}, ${FRAG_FRAMES} frames) ━━━━━━━━━━━${RESET}"
echo ""

for anim in mandelbrot diverge; do
    echo -e "${CYAN}Building ${anim} (dynamic scheduling)…${RESET}"
    build_frag "$anim" "dynamic" "$BUILD_DIR/_scale_${anim}.rv"

    echo -e "${CYAN}Running ${anim} at ${THREAD_COUNTS[*]} threads…${RESET}"
    times=()
    t1_ms="N/A"
    for T in "${THREAD_COUNTS[@]}"; do
        ms=$(run_rv "$BUILD_DIR/_scale_${anim}.rv" "$T")
        times+=("$ms")
        [[ "$T" -eq 1 ]] && t1_ms="$ms"
        echo -e "  T=${T}: ${ms} ms"
    done
    rm -f "$BUILD_DIR/_scale_${anim}.rv"

    echo ""
    echo -e "${BOLD}${anim}:${RESET}"
    print_scaling_table "$anim" "" "$t1_ms" "${times[@]}"
    echo ""
done

# ══════════════════════════════════════════════════════════════════════════════
# 2. THREAD SCALING — Game of Life (serial dependency between generations)
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}━━━ 2. Thread Scaling — Game of Life 256×256 (${LIFE_GENS} generations) ━━━━━━━━━━━━━${RESET}"
echo ""

echo -e "${CYAN}Building life 256×256…${RESET}"
build_life 256 "$BUILD_DIR/_scale_life256.rv"

echo -e "${CYAN}Running at ${THREAD_COUNTS[*]} threads…${RESET}"
life_times=(); life_t1="N/A"
for T in "${THREAD_COUNTS[@]}"; do
    ms=$(run_rv "$BUILD_DIR/_scale_life256.rv" "$T")
    life_times+=("$ms")
    [[ "$T" -eq 1 ]] && life_t1="$ms"
    echo -e "  T=${T}: ${ms} ms/gen"
done
rm -f "$BUILD_DIR/_scale_life256.rv"

echo ""
echo -e "${BOLD}life 256×256:${RESET}"
print_scaling_table "life-256" "" "$life_t1" "${life_times[@]}"

# ══════════════════════════════════════════════════════════════════════════════
# 3. CACHE-SIZE EFFECT ON SCALING
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}━━━ 3. Cache-Size Effect on Thread Scaling ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo ""

declare -A cache_label=([32]="L1 (4 KB)" [128]="L2 (64 KB)" [256]="L3+ (256 KB)")

echo -e "${BOLD}┌──────────────┬────────────┬──────────────────────────────────────────────┐${RESET}"
echo -e "${BOLD}│ Grid (cache) │ T=1 ms/gen │ Speedup at each thread count                 │${RESET}"
printf  "${BOLD}│              │            │"
for T in "${THREAD_COUNTS[@]}"; do printf " %5sT" "$T"; done
echo -e "  │${RESET}"
echo -e "${BOLD}├──────────────┼────────────┼──────────────────────────────────────────────┤${RESET}"

for grid in 32 128 256; do
    echo -e "${CYAN}  Building life ${grid}×${grid}…${RESET}" >&2
    build_life "$grid" "$BUILD_DIR/_scale_life${grid}.rv"
    t1="N/A"
    row_times=()
    for T in "${THREAD_COUNTS[@]}"; do
        ms=$(run_rv "$BUILD_DIR/_scale_life${grid}.rv" "$T")
        row_times+=("$ms")
        [[ "$T" -eq 1 ]] && t1="$ms"
    done
    rm -f "$BUILD_DIR/_scale_life${grid}.rv"

    lbl="${grid}×${grid} (${cache_label[$grid]})"
    printf "│ %-12s │ %10s │" "$lbl" "${t1}ms"
    for i in "${!THREAD_COUNTS[@]}"; do
        ms="${row_times[$i]}"
        if [[ "$ms" == "N/A" || "$t1" == "N/A" ]]; then
            printf " %5s" "N/A"
        else
            sp=$(awk "BEGIN { printf \"%.1f\", $t1 / $ms }")
            printf " %4sx" "$sp"
        fi
    done
    echo " │"
done

echo -e "${BOLD}└──────────────┴────────────┴──────────────────────────────────────────────┘${RESET}"
echo ""

# ══════════════════════════════════════════════════════════════════════════════
# 4. SCHEDULING EFFECT — static vs dynamic for diverge
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}━━━ 4. Scheduling Effect — static vs dynamic (diverge shader) ━━━━━━━━━━━━━━${RESET}"
echo ""

echo -e "${CYAN}Building mandelbrot static / dynamic…${RESET}"
build_frag mandelbrot static  "$BUILD_DIR/_sched_mandelbrot_s.rv"
build_frag mandelbrot dynamic "$BUILD_DIR/_sched_mandelbrot_d.rv"

echo -e "${CYAN}Building diverge static / dynamic…${RESET}"
build_frag diverge static  "$BUILD_DIR/_sched_diverge_s.rv"
build_frag diverge dynamic "$BUILD_DIR/_sched_diverge_d.rv"

echo -e "${BOLD}┌──────────────┬──────────┬──────────┬───────────┬──────────────────────────────┐${RESET}"
echo -e "${BOLD}│ Shader       │ Threads  │ Static   │ Dynamic   │ Dynamic faster by            │${RESET}"
echo -e "${BOLD}├──────────────┼──────────┼──────────┼───────────┼──────────────────────────────┤${RESET}"

for anim in mandelbrot diverge; do
    for T in 1 "${MAX_THREADS}"; do
        ms_s=$(run_rv "$BUILD_DIR/_sched_${anim}_s.rv" "$T")
        ms_d=$(run_rv "$BUILD_DIR/_sched_${anim}_d.rv" "$T")
        if [[ "$ms_s" != "N/A" && "$ms_d" != "N/A" ]]; then
            gain=$(awk "BEGIN { diff = $ms_s - $ms_d; pct = diff / $ms_s * 100;
                printf \"%+.1f%%\", -pct }")
        else
            gain="N/A"
        fi
        printf "│ %-12s │ %8s │ %8s │ %9s │ %-28s │\n" \
            "$anim" "${T}T" "${ms_s}ms" "${ms_d}ms" "$gain"
    done
    echo -e "${BOLD}├──────────────┼──────────┼──────────┼───────────┼──────────────────────────────┤${RESET}"
done

echo -e "${BOLD}└──────────────┴──────────┴──────────┴───────────┴──────────────────────────────┘${RESET}"
rm -f "$BUILD_DIR"/_sched_mandelbrot*.rv "$BUILD_DIR"/_sched_diverge*.rv
echo ""

fi  # end RVV_ONLY guard

# ══════════════════════════════════════════════════════════════════════════════
# 5. RVV VECTOR WIDTH SCALING
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}━━━ 5. RVV Vector Width Scaling ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo ""

# ── part A: build probe binary first, then test QEMU vlen= with it ───────────
echo -e "${BOLD}A) Detect runtime VLEN via vlenb CSR:${RESET}"
echo -e "   Compiling a probe that reads the vlenb CSR register…"

PROBE_SRC="$BUILD_DIR/_vlenb_probe.cpp"
PROBE_BIN="$BUILD_DIR/_vlenb_probe.rv"
cat > "$PROBE_SRC" <<'EOF'
#include <cstdio>
int main() {
    unsigned long vlenb;
    __asm__ volatile("csrr %0, vlenb" : "=r"(vlenb));
    printf("VLEN = %lu bits  (%lu bytes per vector register)\n",
           vlenb * 8UL, vlenb);
    return 0;
}
EOF

PROBE_OK=0
PROBE_ARCH_FLAGS=$([[ "$NATIVE_RISCV" -eq 0 ]] && echo "-mabi=lp64d -march=rv64gcv" || echo "")
if $CROSS_CXX -std=c++20 -O2 -static $PROBE_ARCH_FLAGS \
       "$PROBE_SRC" -o "$PROBE_BIN" 2>/dev/null; then
    PROBE_OK=1
fi

# VLEN probing via QEMU -cpu flag (only supported under emulation)
QEMU_VLEN_OK=0
if [[ "$NATIVE_RISCV" -eq 1 ]]; then
    echo -e "   ${CYAN}Native RISC-V hardware — VLEN is fixed by hardware.${RESET}"
    if [[ "$PROBE_OK" -eq 1 ]]; then
        printf "   Native run → "
        $RISCV_SIM "$PROBE_BIN" 2>/dev/null || echo "(failed)"
    fi
elif [[ "$PROBE_OK" -eq 1 && -n "$QEMU_BIN" ]]; then
    if "$QEMU_BIN" -cpu "rv64,v=true,vlen=256,vext_spec=v1.0" \
           -L "$SYSROOT" "$PROBE_BIN" >/dev/null 2>&1; then
        QEMU_VLEN_OK=1
    fi
    if [[ "$QEMU_VLEN_OK" -eq 1 ]]; then
        for vlen in 128 256 512; do
            printf "   QEMU vlen=%-4s → " "$vlen"
            "$QEMU_BIN" -cpu "rv64,v=true,vlen=${vlen},vext_spec=v1.0" \
                -L "$SYSROOT" "$PROBE_BIN" 2>/dev/null || echo "(failed)"
        done
    else
        printf "   Default QEMU → "
        "$QEMU_BIN" -L "$SYSROOT" "$PROBE_BIN" 2>/dev/null || echo "(failed)"
        echo -e "   ${YELLOW}QEMU $(${QEMU_BIN} --version 2>/dev/null | head -1) does not support -cpu vlen= in user-mode.${RESET}"
        echo -e "   QEMU user-mode restricts CPU properties; system-mode (qemu-system-riscv64)"
        echo -e "   supports full VLEN control. The binary itself adapts to any VLEN via vsetvli."
    fi
else
    echo -e "   ${YELLOW}Probe compile failed or no RISC-V runtime found.${RESET}"
fi
rm -f "$PROBE_SRC" "$PROBE_BIN"

# ── part B: timing at different VLENs (only if QEMU vlen= works) ─────────────
echo ""
echo -e "${BOLD}B) Timing: mandelbrot at VLEN=128/256/512 (${FRAG_FRAMES} frames, ${FRAG_W}×${FRAG_H}):${RESET}"

if [[ "$NATIVE_RISCV" -eq 1 ]]; then
    echo -e "   ${CYAN}Native RISC-V — VLEN is fixed by hardware; no per-VLEN timing sweep.${RESET}"
    echo -e "   Run at fixed VLEN:"
    make -j"$(nproc)" "build/riscv/mandelbrot.rv" >/dev/null 2>&1 || true
    if [[ -f "build/riscv/mandelbrot.rv" ]]; then
        ms=$(OMP_NUM_THREADS="$MAX_THREADS" ./build/riscv/mandelbrot.rv 2>/dev/null \
             | grep -oE 'avg: [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "N/A")
        echo -e "   avg: ${ms}ms/frame (${MAX_THREADS} threads)"
    fi
elif [[ "$QEMU_VLEN_OK" -eq 0 ]]; then
    echo -e "   ${YELLOW}Skipped — QEMU user-mode cannot override VLEN on this system.${RESET}"
    echo -e "   The binary is correct for all VLENs; wall-clock difference would be"
    echo -e "   minimal under QEMU anyway (each vector op simulated individually)."
else
    make -j"$(nproc)" "build/riscv/mandelbrot.rv" >/dev/null 2>&1 || true
    if [[ ! -f "build/riscv/mandelbrot.rv" ]]; then
        echo -e "   ${YELLOW}build/riscv/mandelbrot.rv not found — run make first.${RESET}"
    else
        echo -e "${BOLD}   ┌──────────┬──────────────┬──────────┬─────────────────────────────────────┐${RESET}"
        echo -e "${BOLD}   │ VLEN     │ ms/frame     │ Speedup  │ Note                                │${RESET}"
        echo -e "${BOLD}   ├──────────┼──────────────┼──────────┼─────────────────────────────────────┤${RESET}"
        baseline_ms="N/A"
        for vlen in 128 256 512; do
            ms=$(OMP_NUM_THREADS="$MAX_THREADS" \
                 "$QEMU_BIN" -cpu "rv64,v=true,vlen=${vlen},vext_spec=v1.0" \
                 -L "$SYSROOT" ./build/riscv/mandelbrot.rv 2>/dev/null \
                 | grep -oE 'avg: [0-9]+\.[0-9]+' \
                 | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "N/A")
            [[ "$vlen" -eq 128 ]] && baseline_ms="$ms"
            sp="—"; note=""
            if [[ "$vlen" -eq 128 ]]; then note="baseline"
            elif [[ "$ms" != "N/A" && "$baseline_ms" != "N/A" ]]; then
                sp=$(awk "BEGIN { printf \"%.2fx\", $baseline_ms / $ms }")
                note="vs 128-bit baseline"
            fi
            printf "   │ %-8s │ %12s │ %8s │ %-35s │\n" \
                "${vlen}-bit" "${ms}ms" "$sp" "$note"
        done
        echo -e "${BOLD}   └──────────┴──────────────┴──────────┴─────────────────────────────────────┘${RESET}"
    fi
fi

# ── part C: RVV vectorization — blur_cs_rv compiled with / without +v ────────
echo ""
echo -e "${BOLD}C) RVV vectorization — blur compute shader compiled with and without +v:${RESET}"
echo ""
echo -e "   The same optimised IR (blur_cs_rv.opt.ll) is compiled twice by llc."
echo -e "   cs_dispatch_row is the X-loop over pixels with cs_main inlined inside."
echo -e "     • with    +v  →  loop is vectorised; each iteration processes multiple pixels"
echo -e "     • without +v  →  scalar FP only; one pixel per loop iteration"
echo ""

LLC="$(which llc-18 2>/dev/null || which llc-17 2>/dev/null || which llc 2>/dev/null || echo llc-18)"
RISCV_TRIPLE="riscv64-unknown-linux-gnu"

# Extract one function's disassembly, skipping local .L* labels within it.
dump_fn() {
    local obj="$1" fn="$2"
    set +o pipefail
    $OBJDUMP -d "$obj" 2>/dev/null \
      | awk "
          /^[[:xdigit:]]+ <${fn}>:/ { found=1; next }
          found && /^[[:xdigit:]]+ <[^.>][^>]*>:/ { exit }
          found { print }
      " || true
    set -o pipefail
}

BLR_OPT_LL="build/riscv/blur_cs_rv.opt.ll"
if [[ ! -f "$BLR_OPT_LL" ]]; then
    make "build/riscv/blur_cs_rv.o" >/dev/null 2>&1 || true
fi

if [[ ! -f "$BLR_OPT_LL" ]]; then
    echo -e "   ${YELLOW}blur_cs_rv.opt.ll not found — run 'make benchmark-compute-blur' first.${RESET}"
else
    RVV_OBJ="$BUILD_DIR/_blur_rvv.o"
    NOVEC_OBJ="$BUILD_DIR/_blur_scalar.o"
    NOVEC_LL="$BUILD_DIR/_blur_novec.ll"

    # Strip +v from the IR function attributes so llc really generates scalar code.
    sed 's/"+m,+a,+f,+d,+v[^"]*"/"+m,+a,+f,+d"/' "$BLR_OPT_LL" > "$NOVEC_LL"

    "$LLC" -O3 --fp-contract=fast -filetype=obj -relocation-model=pic \
        -mtriple="$RISCV_TRIPLE" -mattr=+m,+a,+f,+d,+v \
        "$BLR_OPT_LL" -o "$RVV_OBJ" 2>/dev/null
    "$LLC" -O3 --fp-contract=fast -filetype=obj -relocation-model=pic \
        -mtriple="$RISCV_TRIPLE" -mattr=+m,+a,+f,+d \
        "$NOVEC_LL" -o "$NOVEC_OBJ" 2>/dev/null

    echo -e "${BOLD}   ┌──────────────────────────┬──────────────┬───────────────┬────────────────┬─────────────────┐${RESET}"
    echo -e "${BOLD}   │ Compilation              │ Function     │ Total insns   │ Scalar FP ops  │ Vector FP ops   │${RESET}"
    echo -e "${BOLD}   ├──────────────────────────┼──────────────┼───────────────┼────────────────┼─────────────────┤${RESET}"

    for variant in rvv scalar; do
        obj=$([[ "$variant" == rvv ]] && echo "$RVV_OBJ" || echo "$NOVEC_OBJ")
        label=$([[ "$variant" == rvv ]] && echo "with RVV  (+v)" || echo "without RVV")
        for fn in cs_main cs_dispatch_row; do
            fn_dump=$(dump_fn "$obj" "$fn")
            set +o pipefail
            total=$(echo "$fn_dump" | grep -cE '^\s+[0-9a-f]+:' 2>/dev/null || echo 0)
            sfp=$(echo "$fn_dump" | grep -cE '\bfn?(add|mul|sub|div|madd|msub|max|min|sqrt|cvt|mv|class|sgn)\.s\b' 2>/dev/null || echo 0)
            vfp=$(echo "$fn_dump" | grep -cE '\bvf[a-z]+\.' 2>/dev/null || echo 0)
            set -o pipefail
            printf "   │ %-24s │ %-12s │ %13s │ %14s │ %15s │\n" \
                "$label" "$fn" "$total" "$sfp" "$vfp"
        done
        echo -e "${BOLD}   ├──────────────────────────┼──────────────┼───────────────┼────────────────┼─────────────────┤${RESET}"
    done

    echo -e "${BOLD}   └──────────────────────────┴──────────────┴───────────────┴────────────────┴─────────────────┘${RESET}"
    echo ""

    set +o pipefail
    rvv_vfp=$(dump_fn "$RVV_OBJ" "cs_dispatch_row" | grep -cE '\bvf[a-z]+\.' 2>/dev/null || echo 0)
    sc_sfp=$(dump_fn "$NOVEC_OBJ" "cs_dispatch_row" | grep -cE '\bfn?(add|mul|sub|div|madd|msub|max|min|sqrt|cvt|mv|class|sgn)\.s\b' 2>/dev/null || echo 0)
    rvv_total=$(dump_fn "$RVV_OBJ" "cs_dispatch_row" | grep -cE '^\s+[0-9a-f]+:' 2>/dev/null || echo 0)
    sc_total=$(dump_fn "$NOVEC_OBJ" "cs_dispatch_row" | grep -cE '^\s+[0-9a-f]+:' 2>/dev/null || echo 0)
    set -o pipefail

    # Sample 3 FP instructions from each variant to make the contrast concrete.
    rvv_samples=$(dump_fn "$RVV_OBJ"   "cs_dispatch_row" \
        | grep -oE '\bvf[a-z]+\.[a-z]+\s+[^,\n]+,[^,\n]+,[^\n]+' \
        | head -3 | sed 's/^/      /')
    sc_samples=$(dump_fn  "$NOVEC_OBJ" "cs_dispatch_row" \
        | grep -oE '\bfn?(add|mul|sub|div|madd|msub|max|min|sqrt)\.s\s+[^\n]+' \
        | head -3 | sed 's/^/      /')

    echo -e "   ${BOLD}Sample FP instructions in cs_dispatch_row:${RESET}"
    echo ""
    echo -e "   ${GREEN}with RVV (+v) — vector registers, one op covers multiple pixels:${RESET}"
    if [[ -n "$rvv_samples" ]]; then
        echo "$rvv_samples"
    else
        echo -e "   ${YELLOW}   (no vf* instructions found)${RESET}"
    fi
    echo ""
    echo -e "   ${YELLOW}without RVV — scalar registers, one op per float value:${RESET}"
    if [[ -n "$sc_samples" ]]; then
        echo "$sc_samples"
    else
        echo -e "   ${YELLOW}   (no scalar f*.s instructions found)${RESET}"
    fi
    echo ""

    if [[ "$rvv_vfp" -gt 0 && "$sc_sfp" -gt 0 ]]; then
        echo -e "   ${GREEN}RVV replaces ${sc_sfp} scalar FP ops with ${rvv_vfp} vector FP ops in cs_dispatch_row.${RESET}"
    fi
    if [[ "$sc_total" -gt 0 && "$rvv_total" -gt 0 ]]; then
        ratio=$(awk "BEGIN { printf \"%.1f\", $sc_total / $rvv_total }")
        echo -e "   cs_dispatch_row instruction count: ${BOLD}${ratio}x${RESET} more instructions without RVV."
    fi

    rm -f "$RVV_OBJ" "$NOVEC_OBJ" "$NOVEC_LL"
fi
