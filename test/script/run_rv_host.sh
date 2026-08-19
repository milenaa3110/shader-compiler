#!/usr/bin/env bash
# run_rv_host.sh <binary> <label> — run a self-verifying RISC-V host under QEMU.
#
# The host binary does its own checks and returns 0 (pass) / non-zero (fail); this
# wrapper just runs it under QEMU and propagates the exit code. Skips cleanly
# (exit 0) when QEMU or the binary is absent so the gate is a no-op off-target.

set -uo pipefail
BIN="${1:?usage: run_rv_host.sh <binary> <label>}"
LABEL="${2:-rv-host}"
SYSROOT="/usr/riscv64-linux-gnu"

GREEN="\033[0;32m"; RED="\033[0;31m"; YEL="\033[1;33m"; RST="\033[0m"

QEMU="$(command -v qemu-riscv64-static 2>/dev/null || command -v qemu-riscv64 2>/dev/null || true)"
[ -n "$QEMU" ] || { echo -e "  ${YEL}SKIP${RST}  $LABEL (QEMU missing)"; exit 0; }
[ -f "$BIN" ]  || { echo -e "  ${YEL}SKIP${RST}  $LABEL (binary not built: $BIN)"; exit 0; }

out="$("$QEMU" -L "$SYSROOT" "$BIN" </dev/null 2>&1)"; rc=$?
echo "$out"
if [ "$rc" -eq 0 ]; then
    echo -e "  ${GREEN}PASS${RST}  $LABEL"
else
    echo -e "  ${RED}FAIL${RST}  $LABEL (rc=$rc)"
fi
exit "$rc"
