#!/usr/bin/env bash
# packet_report.sh — per-shader packetizer coverage and loop-unroll report.
#
# Two questions per shader, both cheap and both easy to regress silently:
#   1. Does the packetizer emit at all, or does it bail on an unsupported
#      construct? A bail is invisible at runtime — the scalar path is used, the
#      pixels are correct, and the measurement simply stops being about packet
#      code. city_fs.src did this for its whole life.
#   2. Do the loops inside fs_packet/vs_packet get unrolled? The uniform-loop
#      path exists so SCEV can see a lane-independent trip count; when that
#      works LoopFullUnroll fires and the per-iteration constants fold.
#
# Remarks come from `opt` (-pass-remarks=...), not clang (-Rpass=...).
#
# Usage: bash test/script/packet_report.sh [--build-dir DIR] [--shaders a b ...]

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SHADERS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --shaders)   shift; while [ $# -gt 0 ] && [[ "$1" != --* ]]; do SHADERS+=("$1"); shift; done ;;
        -h|--help)   sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

GREEN="\033[0;32m"; RED="\033[0;31m"; YEL="\033[1;33m"; DIM="\033[2m"; RST="\033[0m"

IRGEN="$BUILD_DIR/riscv/irgen_riscv"
[ -x "$IRGEN" ] || { echo "no $IRGEN — build irgen_riscv first" >&2; exit 1; }

OPT="$(command -v opt-18 || command -v opt-17 || command -v opt || true)"
[ -n "$OPT" ] || { echo "opt not found" >&2; exit 1; }

ANIM="$ROOT/test/shaders/animations"
if [ ${#SHADERS[@]} -eq 0 ]; then
    for f in "$ANIM"/*_fs.src "$ANIM"/*_vs.src; do
        [ -e "$f" ] && SHADERS+=("$(basename "$f" .src)")
    done
fi

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
emitted=0; bailed=0; unrolled=0

printf '%-24s %-22s %s\n' "shader" "packetizer" "loop-unroll in *_packet"
printf '%-24s %-22s %s\n' "------" "----------" "-----------------------"

for name in "${SHADERS[@]}"; do
    # Accept both "ocean" and "ocean_fs"; the default list carries the suffix.
    src="$ANIM/$name.src"
    [ -f "$src" ] || src="$ANIM/${name}_fs.src"
    [ -f "$src" ] || { printf '%-24s %b\n' "$name" "${YEL}no such shader${RST}"; continue; }

    ll="$TMP/$name.ll"
    out="$(SHADER_EMIT_PACKET=verbose "$IRGEN" "$ll" < "$src" 2>&1)"
    if ! grep -q 'Emitted packet' <<<"$out"; then
        if grep -q 'packet: bailed' <<<"$out"; then
            printf '%-24s %b %s\n' "$name" "${RED}bailed${RST}                " \
                   "$(printf "${DIM}(scalar fallback — not packet code)${RST}")"
            bailed=$((bailed+1))
        else
            printf '%-24s %b\n' "$name" "${DIM}no stage entry${RST}"
        fi
        continue
    fi
    emitted=$((emitted+1))

    yaml="$TMP/$name.yaml"
    "$OPT" -O3 --enable-unsafe-fp-math --fp-contract=fast \
           -pass-remarks-output="$yaml" -pass-remarks-filter='loop-unroll' \
           -S "$ll" -o /dev/null 2>/dev/null

    # One YAML record per remark; keep only those inside a *_packet function and
    # report the unroll verdict for each.
    verdict="$(awk '
        /^--- !/           { kind=$2; name=""; fn=""; cnt="" }
        /^Name:/            { name=$2 }
        /^Function:/        { fn=$2 }
        /UnrollCount:/      { gsub(/[^0-9]/,"",$2); cnt=$2 }
        /^\.\.\./ {
            if (fn ~ /_packet$/ && name != "") {
                if (kind == "!Passed") passed[name (cnt!=""?" x" cnt:"")]++
                else missed[name]++
            }
        }
        END {
            o=""
            for (k in passed) o = o (o==""?"":", ") k
            if (o == "") { for (k in missed) o = o (o==""?"":", ") "missed:" k }
            print (o==""?"no loops":o)
        }' "$yaml" 2>/dev/null)"

    case "$verdict" in
        *FullyUnrolled*) col="$GREEN"; unrolled=$((unrolled+1)) ;;
        "no loops")      col="$DIM" ;;
        *)               col="$YEL" ;;
    esac
    printf '%-24s %b %b\n' "$name" "${GREEN}emitted${RST}               " "${col}${verdict}${RST}"
done

echo
printf 'emitted: %d   bailed: %d   with a fully unrolled loop: %d\n' \
       "$emitted" "$bailed" "$unrolled"
[ "$bailed" -eq 0 ]
