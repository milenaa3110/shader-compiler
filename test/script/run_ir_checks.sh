#!/usr/bin/env bash
# run_ir_checks.sh - FileCheck-based LLVM IR structural regression runner

set -euo pipefail

# Resolve absolute paths relative to the script location.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
IRGEN="$ROOT/build/riscv/irgen_riscv"
CHECK_DIR="$ROOT/test/shaders/codegen_checks"
FILECHECK="${FILECHECK:-FileCheck-18}"
TMP_DIR="$(mktemp -d)"

GREEN="\033[0;32m"; RED="\033[0;31m"; CYAN="\033[0;36m"; RESET="\033[0m"
PASS=0; FAIL=0; NO_BUILD=0

# Process target flags.
for arg in "$@"; do
    case "$arg" in --no-build) NO_BUILD=1 ;; esac
done

# RAII-like cleanup tracking for temporary scratchpads.
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT
log() { echo -e "$*"; }

#  Build Trigger 
if [ "$NO_BUILD" -eq 0 ]; then
    log "${CYAN}Rebuilding compiler binary...${RESET}"
    [ -d "$ROOT/build" ] || cmake -S "$ROOT" -B "$ROOT/build" >/dev/null
    cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null
fi

# Validate third-party and in-tree binary dependencies.
if ! command -v "$FILECHECK" >/dev/null 2>&1; then
    log "${RED}Error: LLVM FileCheck utility not found: $FILECHECK (set \$FILECHECK)${RESET}"
    exit 1
fi
if [ ! -x "$IRGEN" ]; then
    log "${RED}Error: irgen executable not found at $IRGEN${RESET}"; exit 1
fi

#  Verification Core ─
run_check() {
    local src="$1" name
    name="$(basename "$src" .src)"
    
    # Enforce workspace isolation to avoid local 'module.ll' race conditions.
    local tmp_cwd; tmp_cwd="$(mktemp -d "$TMP_DIR/${name}.XXXX")"

    # Compile source shader into text LLVM IR.
    if ! (cd "$tmp_cwd" && "$IRGEN" < "$src" >/dev/null 2>&1) || [ ! -f "$tmp_cwd/module.ll" ]; then
        log "  ${RED}FAIL${RESET}  ${name}  (codegen pass crashed or skipped file generation)"
        FAIL=$((FAIL + 1)); return
    fi

    # Pipe generated text IR into FileCheck to match directives against the source.
    local fc_out
    if fc_out=$("$FILECHECK" "$src" < "$tmp_cwd/module.ll" 2>&1); then
        log "  ${GREEN}PASS${RESET}  ${name}"
        PASS=$((PASS + 1))
    else
        log "  ${RED}FAIL${RESET}  ${name}  (emitted LLVM IR structure decoupled from CHECK directives)"
        echo "$fc_out" | sed 's/^/         /'
        FAIL=$((FAIL + 1))
    fi
}

# Discover all testing source scripts sequentially using null-terminated paths.
shaders=()
while IFS= read -r -d '' f; do shaders+=("$f"); done \
    < <(find "$CHECK_DIR" -name "*.src" -print0 | sort -z)

log "\n${CYAN}Evaluating ${#shaders[@]} structural IR verifications...${RESET}"
for s in "${shaders[@]}"; do run_check "$s"; done

#  Summary and Diagnostics Evaluation 
log ""
log " IR Structural Suite: ${GREEN}PASS${RESET} $PASS  ${RED}FAIL${RESET} $FAIL"
log ""

# Terminate with failure if structural matching checks broke invariance rules.
[ "$FAIL" -eq 0 ]