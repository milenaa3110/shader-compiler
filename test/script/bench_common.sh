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

# ── parse_avg <output_string> ─────────────────────────────────────────────────
# Extracts the first "avg: N.NN" value from benchmark output.
# Works for: spirv_vulkan_host ("avg: X"), bench_host ("RISC-V avg: X"),
#            spirv_vulkan_life_host ("avg: X"), life_host ("avg: X").
parse_avg() {
    echo "$1" | grep -oE 'avg: [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "N/A"
}

# ── speedup_label <gpu_ms> <cpu_ms> ──────────────────────────────────────────
# Prints how many times slower the CPU is: "6.2x"
speedup_label() {
    local a="$1" b="$2"
    [[ "$a" == "N/A" || "$b" == "N/A" ]] && { echo "N/A"; return; }
    awk "BEGIN { printf \"%.1fx\", $b / $a }"
}
