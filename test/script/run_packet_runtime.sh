#!/usr/bin/env bash
# run_packet_runtime.sh — packet-width RUNTIME gate (the part run_packet_test.sh
# can't cover because it never links the runtime).
#
# Two checks, both under QEMU with the packet path active (SHADER_PACKET=1):
#   1. Banner smoke  — get_vlen_bits()/report_vector_config() print VLEN + the
#                      compiled packet width, and a matching build does NOT trip
#                      the build-width guard (no FATAL).
#   2. Guard test    — compile the runtime at a DIFFERENT width than the shader's
#                      baked __shader_packet_width marker; the runtime MUST abort
#                      loudly (SoA stride would otherwise silently corrupt pixels).
#
# The shader IR width is baked into irgen_riscv at CMake configure time; we do NOT
# rebuild it. The mismatch is produced purely by a -DSHADER_PACKET_WIDTH override
# on the pipeline_runtime.cpp compile — cheap and self-contained.
#
# Skips cleanly (exit 0) when the cross toolchain / QEMU / shader object is absent.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$ROOT/build"
RVO="$BUILD/riscv/mandelbrot_rv.o"
HOSTSRC="$ROOT/test/rv_host/rv_host_fragment.cpp"
RTSRC="$ROOT/src/runtime/pipeline_runtime.cpp"
INC="$ROOT/src/runtime"
SYSROOT="/usr/riscv64-linux-gnu"

GREEN="\033[0;32m"; RED="\033[0;31m"; YEL="\033[1;33m"; RST="\033[0m"
fail=0
pass(){ echo -e "  ${GREEN}PASS${RST}  $*"; }
bad(){  echo -e "  ${RED}FAIL${RST}  $*"; fail=1; }
skip(){ echo -e "  ${YEL}SKIP${RST}  $*"; }

CROSS_CXX="$(command -v riscv64-linux-gnu-g++ 2>/dev/null || true)"
QEMU="$(command -v qemu-riscv64-static 2>/dev/null || command -v qemu-riscv64 2>/dev/null || true)"

[ -n "$CROSS_CXX" ] && [ -n "$QEMU" ] || { skip "cross-cc or QEMU missing — packet runtime gate skipped"; exit 0; }
[ -f "$RVO" ]     || { skip "no $RVO (build a shader first)"; exit 0; }

# Compiled packet width baked into the shader (irgen_riscv's kW == the marker).
W=4
[ -f "$BUILD/packet_width.env" ] && source "$BUILD/packet_width.env"
W="${SHADER_PACKET_WIDTH:-4}"
MIS=$([ "$W" -eq 4 ] && echo 8 || echo 4)   # any width != W trips the guard

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

build_host() { # $1 = runtime width, $2 = output binary
    "$CROSS_CXX" -std=c++20 -O2 -static -fopenmp -march=rv64gcv -mabi=lp64d -I"$INC" \
        -DANIM_NAME='"pkt"' -DNFRAMES=1 -DFPS=30 -DWIDTH=32 -DHEIGHT=32 \
        -DSHADER_PACKET_WIDTH="$1" \
        "$HOSTSRC" "$RTSRC" "$RVO" -o "$2" 2>"$TMP/cc.err"
}

echo "Packet-width runtime gate (shader marker width = $W)"

# ── 1. Banner smoke: matching runtime width → banner prints, no false abort ────
if build_host "$W" "$TMP/match.rv"; then
    out="$(SHADER_PACKET=1 "$QEMU" -L "$SYSROOT" "$TMP/match.rv" </dev/null 2>&1)"
    if grep -q "VLEN=" <<<"$out" \
       && grep -q "packet width=$W lanes" <<<"$out" \
       && ! grep -q "FATAL" <<<"$out"; then
        pass "banner: VLEN detected + width=$W + no false guard"
    else
        bad "banner: unexpected output"; sed -n '1,3p' <<<"$out"
    fi
else
    bad "banner: host build @${W} failed"; tail -3 "$TMP/cc.err"
fi

# ── 2. Guard test: mismatched runtime width → loud abort, not silent garbage ───
if build_host "$MIS" "$TMP/mis.rv"; then
    out="$(SHADER_PACKET=1 "$QEMU" -L "$SYSROOT" "$TMP/mis.rv" </dev/null 2>&1)"; rc=$?
    if grep -q "FATAL: SPMD packet-width mismatch" <<<"$out" && [ "$rc" -ne 0 ]; then
        pass "guard: marker=$W vs runtime=$MIS → loud abort (rc=$rc)"
    else
        bad "guard: expected FATAL abort (marker=$W, runtime=$MIS), got rc=$rc"
        sed -n '1,3p' <<<"$out"
    fi
else
    bad "guard: host build @${MIS} failed"; tail -3 "$TMP/cc.err"
fi

echo ""
[ "$fail" -eq 0 ] && echo -e " Suite: ${GREEN}OK${RST}" || echo -e " Suite: ${RED}FAILED${RST}"
exit "$fail"
