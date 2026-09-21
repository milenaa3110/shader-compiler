#!/usr/bin/env bash
# libm_census.sh — how many scalar libm calls survive inside each *_packet body.
#
# The packet path only pays off where RVV has a real instruction. sqrt does
# (vfsqrt.v); sin/cos/exp/log do not, so llc scalarizes llvm.<fn>.vNf32 into W
# separate libm calls plus the pack/unpack around them — the packet version then
# issues exactly as many libm calls as the scalar one and adds shuffles on top.
# This counts them directly from the generated assembly, which is the number the
# vector-math library has to drive down.
#
# Usage: bash test/script/libm_census.sh [--build-dir DIR] [--shaders a b ...]

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SHADERS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --shaders)   shift; while [ $# -gt 0 ] && [[ "$1" != --* ]]; do SHADERS+=("$1"); shift; done ;;
        -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

IRGEN="$BUILD_DIR/riscv/irgen_riscv"
[ -x "$IRGEN" ] || { echo "no $IRGEN — build irgen_riscv first" >&2; exit 1; }
OPT="$(command -v opt-18 || command -v opt-17 || command -v opt || true)"
LLC="$(command -v llc-18 || command -v llc-17 || command -v llc || true)"
[ -n "$OPT" ] && [ -n "$LLC" ] || { echo "opt/llc not found" >&2; exit 1; }

ANIM="$ROOT/test/shaders/animations"
if [ ${#SHADERS[@]} -eq 0 ]; then
    for f in "$ANIM"/*_fs.src; do [ -e "$f" ] && SHADERS+=("$(basename "$f" .src)"); done
fi

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

printf '%-22s %-8s %s\n' "shader" "calls" "breakdown inside fs_packet"
printf '%-22s %-8s %s\n' "------" "-----" "--------------------------"

for name in "${SHADERS[@]}"; do
    # Accept both "ocean" and "ocean_fs"; the default list carries the suffix.
    src="$ANIM/$name.src"
    [ -f "$src" ] || src="$ANIM/${name}_fs.src"
    [ -f "$src" ] || { printf '%-22s %-8s %s\n' "$name" "-" "no such shader"; continue; }

    ll="$TMP/$name.ll"
    if ! SHADER_EMIT_PACKET=verbose "$IRGEN" "$ll" < "$src" 2>&1 | grep -q 'Emitted packet'; then
        printf '%-22s %-8s %s\n' "$name" "-" "packetizer bailed"
        continue
    fi

    # Same pipeline the build uses, minus the sincos plugin (it only rewrites the
    # scalar form and would not change the packet body).
    "$OPT" -O3 --enable-unsafe-fp-math --fp-contract=fast -S "$ll" -o "$TMP/$name.opt.ll" 2>/dev/null
    "$LLC" -O3 --fp-contract=fast -relocation-model=pic \
           -mtriple=riscv64-unknown-linux-gnu -mattr=+m,+a,+f,+d,+v \
           "$TMP/$name.opt.ll" -o "$TMP/$name.s" 2>/dev/null || {
        printf '%-22s %-8s %s\n' "$name" "?" "llc failed"; continue; }

    # Only the packet body: the scalar fs_main next door calls the same symbols.
    body="$(awk '/^fs_packet:/,/\.size[[:space:]]+fs_packet/' "$TMP/$name.s")"
    counts="$(grep -oE 'call[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' <<<"$body" \
              | awk '{print $2}' | sort | uniq -c | sort -rn \
              | awk '{printf "%s×%s ", $2, $1}')"
    total="$(grep -cE 'call[[:space:]]+[A-Za-z_]' <<<"$body")"
    printf '%-22s %-8s %s\n' "$name" "$total" "${counts:-none}"
done
