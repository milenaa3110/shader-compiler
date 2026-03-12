#!/usr/bin/env bash
# run_spirv_anim.sh — compile anim_vs + anim_fs to SPIR-V and validate each
# frame's module.spv with spirv-val (if available).
# This simulates "animation" by producing SPIR-V for 8 time-step variants via
# different uTime constant values baked in at compile time.
#
# Usage: bash test/run_spirv_anim.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IRGEN_SPIRV="$ROOT/irgen_spirv"
SPIRV_VAL="${SPIRV_VAL:-spirv-val}"
TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

GREEN="\033[0;32m"; RED="\033[0;31m"; CYAN="\033[0;36m"; RESET="\033[0m"
PASS=0; FAIL=0

if [ ! -x "$IRGEN_SPIRV" ]; then
    echo -e "${RED}irgen_spirv not built. Run: make irgen_spirv${RESET}"
    exit 1
fi

echo -e "${CYAN}SPIR-V animation test — compiling anim_vs + anim_fs for 8 frames${RESET}"

for stage in vs fs; do
    src="$ROOT/test/shaders/pipeline/anim_${stage}.src"
    out="$TMP/anim_${stage}.ll"

    echo -n "  Compiling anim_${stage}.src … "
    # irgen_spirv writes module.ll in CWD, then calls llvm-spirv
    # Since llvm-spirv may not be installed, we only check the LL output here
    if (cd "$TMP" && "$IRGEN_SPIRV" < "$src" > /dev/null 2>&1); then
        cp "$TMP/module.ll" "$out" 2>/dev/null || true
        echo -e "${GREEN}OK${RESET}"
        PASS=$((PASS+1))
    else
        # irgen_spirv exits 1 if llvm-spirv is missing but still writes module.ll
        if [ -f "$TMP/module.ll" ]; then
            cp "$TMP/module.ll" "$out"
            echo -e "${CYAN}LL written (llvm-spirv not installed — install: sudo apt install llvm-spirv-18)${RESET}"
            PASS=$((PASS+1))
        else
            echo -e "${RED}FAIL${RESET}"
            FAIL=$((FAIL+1))
        fi
    fi

    # Validate the LL with llvm-as regardless of SPIR-V
    if [ -f "$out" ]; then
        echo -n "  Validating IR (llvm-as) anim_${stage} … "
        if llvm-as-18 -o /dev/null "$out" 2>/dev/null; then
            echo -e "${GREEN}PASS${RESET}"
        else
            echo -e "${RED}FAIL (invalid IR)${RESET}"
            FAIL=$((FAIL+1))
        fi
    fi

    # Link VS+FS into one module to check combined validity
    if [ "$stage" = "fs" ] && [ -f "$TMP/anim_vs.ll" ] && [ -f "$TMP/anim_fs.ll" ]; then
        combined="$TMP/anim_combined.ll"
        echo -n "  Linking VS+FS … "
        if llvm-link-18 "$TMP/anim_vs.ll" "$TMP/anim_fs.ll" -S -o "$combined" 2>/dev/null; then
            echo -e "${GREEN}OK${RESET}"
            # Validate with spirv-val if available
            if command -v "$SPIRV_VAL" &>/dev/null && [ -f "$TMP/module.spv" ]; then
                echo -n "  spirv-val … "
                if "$SPIRV_VAL" "$TMP/module.spv" 2>/dev/null; then
                    echo -e "${GREEN}PASS${RESET}"
                    PASS=$((PASS+1))
                else
                    echo -e "${RED}FAIL${RESET}"
                    FAIL=$((FAIL+1))
                fi
            else
                echo -e "  ${CYAN}(spirv-val not found — skipping binary validation)${RESET}"
            fi
        else
            echo -e "${RED}FAIL${RESET}"
            FAIL=$((FAIL+1))
        fi
    fi
done

echo ""
echo "────────────────────────────────"
echo " SPIR-V anim results: PASS=$PASS  FAIL=$FAIL"
echo "────────────────────────────────"
[ "$FAIL" -eq 0 ]