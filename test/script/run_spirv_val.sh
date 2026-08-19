#!/usr/bin/env bash
# run_spirv_val.sh - spirv-val conformance runner for every emitted SPIR-V module
#
# llvm-as (run_tests.sh) proves the LLVM IR is well-formed and FileCheck
# (run_ir_checks.sh) pins its shape, but neither says anything about the .spv
# the SPIR-V backend writes. emit_spirv_from_ir.h fails silently by design
# (typeFor returns 0, lowerConstant falls back to OpConstantNull, the binary-op
# switch defaults to FAdd) — a malformed module lands on disk and the driver
# still reports success. This is the only gate that catches that.

set -euo pipefail

# Resolve absolute paths relative to the script location.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
SPV_DIR="${SPV_DIR:-$ROOT/build/spirv}"
SPIRV_VAL="${SPIRV_VAL:-spirv-val}"

GREEN="\033[0;32m"; RED="\033[0;31m"; CYAN="\033[0;36m"; RESET="\033[0m"
PASS=0; FAIL=0; NO_BUILD=0

# Process target flags.
for arg in "$@"; do
    case "$arg" in --no-build) NO_BUILD=1 ;; esac
done

log() { echo -e "$*"; }

#  Build Trigger
if [ "$NO_BUILD" -eq 0 ]; then
    log "${CYAN}Rebuilding SPIR-V artifacts...${RESET}"
    [ -d "$ROOT/build" ] || cmake -S "$ROOT" -B "$ROOT/build" >/dev/null
    cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null
fi

# Validate third-party and in-tree binary dependencies.
if ! command -v "$SPIRV_VAL" >/dev/null 2>&1; then
    log "${RED}Error: SPIR-V validator not found: $SPIRV_VAL (set \$SPIRV_VAL)${RESET}"
    exit 1
fi

#  Verification Core
run_val() {
    local spv="$1" name val_out
    name="$(basename "$spv")"

    if val_out=$("$SPIRV_VAL" "$spv" 2>&1); then
        log "  ${GREEN}PASS${RESET}  ${name}"
        PASS=$((PASS + 1))
    else
        log "  ${RED}FAIL${RESET}  ${name}  (emitted module rejected by the SPIR-V validator)"
        echo "$val_out" | sed 's/^/         /'
        FAIL=$((FAIL + 1))
    fi
}

# Discover every emitted module; sorted for stable output ordering.
modules=()
while IFS= read -r -d '' f; do modules+=("$f"); done \
    < <(find "$SPV_DIR" -maxdepth 1 -name "*.spv" -print0 2>/dev/null | sort -z)

if [ "${#modules[@]}" -eq 0 ]; then
    log "${RED}Error: no .spv modules found in $SPV_DIR — build the shaders first${RESET}"
    exit 1
fi

log "\n${CYAN}Validating ${#modules[@]} SPIR-V modules...${RESET}"
for m in "${modules[@]}"; do run_val "$m"; done

#  Summary and Diagnostics Evaluation
log ""
log " SPIR-V Validation Suite: ${GREEN}PASS${RESET} $PASS  ${RED}FAIL${RESET} $FAIL"
log ""

[ "$FAIL" -eq 0 ]
