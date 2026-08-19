#!/usr/bin/env bash
# run_reject_tests.sh - shaders that MUST fail to compile, and with which message
#
# run_tests.sh proves valid shaders compile; nothing proved invalid ones are
# rejected. That gap is how `-mat3` reached codegen and aborted, how `mat4*ivec4`
# emitted ill-typed IR, and how a 20-float varying set was accepted for a
# rasterizer with 16 slots. Each test/shaders/reject/*.src carries one or more
#   # EXPECT-ERROR: <substring>
# directives; the shader must exit non-zero AND print every listed substring. A
# crash (signal, exit >= 128) is a failure even though it is "non-zero".

set -euo pipefail

# Resolve absolute paths relative to the script location.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
IRGEN="${IRGEN:-$ROOT/build/riscv/irgen_riscv}"
REJECT_DIR="$ROOT/test/shaders/reject"
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

if [ ! -x "$IRGEN" ]; then
    log "${RED}Error: irgen executable not found at $IRGEN${RESET}"; exit 1
fi

#  Verification Core
run_reject() {
    local src="$1" name out rc
    name="$(basename "$src" .src)"

    # Enforce workspace isolation to avoid local 'module.ll' race conditions.
    local tmp_cwd; tmp_cwd="$(mktemp -d "$TMP_DIR/${name}.XXXX")"
    set +e
    out=$(cd "$tmp_cwd" && "$IRGEN" < "$src" 2>&1); rc=$?
    set -e

    if [ "$rc" -ge 128 ]; then
        log "  ${RED}FAIL${RESET}  ${name}  (compiler crashed with signal $((rc - 128)) instead of diagnosing)"
        FAIL=$((FAIL + 1)); return
    fi
    if [ "$rc" -eq 0 ]; then
        log "  ${RED}FAIL${RESET}  ${name}  (accepted a shader that must be rejected)"
        FAIL=$((FAIL + 1)); return
    fi

    # Every EXPECT-ERROR substring must appear in the diagnostics.
    local missing=0 pattern
    while IFS= read -r pattern; do
        [ -n "$pattern" ] || continue
        if ! grep -qF -- "$pattern" <<<"$out"; then
            log "  ${RED}FAIL${RESET}  ${name}  (missing expected diagnostic: ${pattern})"
            missing=1
        fi
    done < <(sed -n 's/^[[:space:]]*#[[:space:]]*EXPECT-ERROR:[[:space:]]*//p' "$src")

    if [ "$missing" -eq 1 ]; then
        echo "$out" | sed 's/^/         /'
        FAIL=$((FAIL + 1))
    else
        log "  ${GREEN}PASS${RESET}  ${name}"
        PASS=$((PASS + 1))
    fi
}

# Discover all rejection cases sequentially using null-terminated paths.
shaders=()
while IFS= read -r -d '' f; do shaders+=("$f"); done \
    < <(find "$REJECT_DIR" -name "*.src" -print0 | sort -z)

log "\n${CYAN}Evaluating ${#shaders[@]} rejection cases...${RESET}"
for s in "${shaders[@]}"; do run_reject "$s"; done

#  Summary and Diagnostics Evaluation
log ""
log " Rejection Suite: ${GREEN}PASS${RESET} $PASS  ${RED}FAIL${RESET} $FAIL"
log ""

[ "$FAIL" -eq 0 ]
