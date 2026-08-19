#!/usr/bin/env bash
# bench_common.sh — shared helpers for all benchmark scripts
#
# Source at the top of each script:
#   source "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

# ── Terminal colors ──────────────────────────────────────────────────────────
RED="\033[0;31m"
GREEN="\033[0;32m"
YELLOW="\033[1;33m"
BLUE="\033[0;34m"
CYAN="\033[0;36m"
BOLD="\033[1m"
RESET="\033[0m"

# ── Host environment ─────────────────────────────────────────────────────────
NTHREADS="$(nproc)"
SYSROOT="/usr/riscv64-linux-gnu"
BUILD_DIR="build"
HOST_ARCH="$(uname -m)"

# ── Build helpers (CMake) ────────────────────────────────────────────────────
# build_target NAME [NAME...]   — build one or more CMake targets
# build_all                      — build everything (configure if needed)
build_target() {
    [[ -d "$BUILD_DIR" ]] || cmake -S . -B "$BUILD_DIR" >/dev/null
    cmake --build "$BUILD_DIR" -j"$NTHREADS" --target "$@" 2>&1 \
        | grep -E "^(g\+\+|riscv|error)" || true
}
build_all() {
    [[ -d "$BUILD_DIR" ]] || cmake -S . -B "$BUILD_DIR" >/dev/null
    cmake --build "$BUILD_DIR" -j"$NTHREADS" 2>&1 \
        | grep -E "^(g\+\+|riscv|error)" || true
}

# ── RISC-V execution: native hardware or QEMU emulation ──────────────────────
QEMU_BIN="$(which qemu-riscv64-static 2>/dev/null || which qemu-riscv64 2>/dev/null || true)"

if [[ "$HOST_ARCH" == "riscv64" ]]; then
    NATIVE_RISCV=1
    CROSS_CXX="g++"
    OBJDUMP="objdump"
    RISCV_SIM=""
    RISCV_AVAIL=1
else
    NATIVE_RISCV=0
    CROSS_CXX="riscv64-linux-gnu-g++"
    OBJDUMP="riscv64-linux-gnu-objdump"
    if [[ -n "$QEMU_BIN" ]]; then
        RISCV_SIM="$QEMU_BIN -L $SYSROOT"
        RISCV_AVAIL=1
    else
        RISCV_SIM=""
        RISCV_AVAIL=0
    fi
fi

# ── SPMD packet width (from the CMake-resolved build; default 4) ──────────────
# Scripts that cross-compile pipeline_runtime.cpp themselves must bake the SAME
# lane count the CMake-built irgen_riscv baked into the shader IR, or the SoA
# stride disagrees (the runtime guard would then abort). CMake writes it here.
if [[ -f "$BUILD_DIR/packet_width.env" ]]; then source "$BUILD_DIR/packet_width.env"; fi
: "${SHADER_PACKET_WIDTH:=4}"
export SHADER_PACKET_WIDTH

# ── Vulkan ICD (from the CMake-resolved build) ────────────────────────────────
# On a host with no usable GPU ICD the loader silently falls back to lavapipe and
# the "GPU" column becomes a CPU number with nothing marking it as such. If the
# build was configured with -DDZN_ICD=… (Vulkan over D3D12, the only GPU path on
# WSL2), reuse that here so the benchmark measures the same device the vk-*
# targets do. An ICD already in the environment always wins.
if [[ -z "${VK_ICD_FILENAMES:-}" && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    _dzn_icd=$(grep -m1 '^DZN_ICD:FILEPATH=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2-)
    if [[ -n "$_dzn_icd" && -f "$_dzn_icd" ]]; then
        export VK_ICD_FILENAMES="$_dzn_icd"
        export LD_LIBRARY_PATH="/usr/lib/wsl/lib:${LD_LIBRARY_PATH:-}"
    fi
    unset _dzn_icd
fi

# ── parse_avg <output_string> ─────────────────────────────────────────────────
# Extracts the first "avg: N.NN" value from benchmark output.
# Works for: spirv_vulkan_host ("avg: X"), bench_host ("RISC-V avg: X"),
#            spirv_vulkan_life_host ("avg: X"), life_host ("avg: X").
parse_avg() {
    echo "$1" | grep -oE 'avg: [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "N/A"
}

# ── median_of <runs> <command…> ───────────────────────────────────────────────
# Runs the command `runs` times and prints the median of the numbers it echoes.
#
# A single run is not a stable figure on an emulated target: QEMU's translation
# cache, the OpenMP team startup per frame and ordinary host scheduling move the
# reported mean by several percent between otherwise identical runs — enough to
# change a speedup column between invocations of the same script. The median is
# taken rather than the mean so one scheduler hiccup cannot drag the number.
#
# Returns N/A as soon as any run does, rather than a median over partial data.
median_of() {
    local runs="$1"; shift
    local vals=() v
    for ((i = 0; i < runs; i++)); do
        v="$("$@")"
        [[ "$v" == "N/A" || -z "$v" ]] && { echo "N/A"; return; }
        vals+=("$v")
    done
    printf '%s\n' "${vals[@]}" | sort -g | \
        awk '{a[NR]=$1} END { printf "%.4g", (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2 }'
}

# ── speedup_label <gpu_ms> <cpu_ms> ──────────────────────────────────────────
# Prints how many times slower the CPU is: "6.2x"
speedup_label() {
    local a="$1" b="$2"
    [[ "$a" == "N/A" || "$b" == "N/A" ]] && { echo "N/A"; return; }
    awk "BEGIN { printf \"%.1fx\", $b / $a }"
}
