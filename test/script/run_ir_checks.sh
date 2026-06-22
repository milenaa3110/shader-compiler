#!/usr/bin/env bash
# run_ir_checks.sh — FileCheck-based regression tests on the generated LLVM IR.
#
# Unlike run_tests.sh (which only asserts the IR is *valid*), these pin the
# *shape* of the IR for properties that validity alone can't catch — e.g. that
# && short-circuits, or that a ternary's cast lands in the right block. Each
# .src in test/shaders/codegen_checks/ carries its own `# CHECK:` directives.
#
# Usage: ./test/script/run_ir_checks.sh [--no-build]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
IRGEN="$ROOT/build/riscv/irgen_riscv"
CHECK_DIR="$ROOT/test/shaders/codegen_checks"
FILECHECK="${FILECHECK:-FileCheck-18}"
TMP_DIR="$(mktemp -d)"

GREEN="\033[0;32m"; RED="\033[0;31m"; CYAN="\033[0;36m"; RESET="\033[0m"
PASS=0; FAIL=0; NO_BUILD=0

for arg in "$@"; do
    case "$arg" in --no-build) NO_BUILD=1 ;; esac
done

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT
log() { echo -e "$*"; }

if [ "$NO_BUILD" -eq 0 ]; then
    log "${CYAN}Building...${RESET}"
    [ -d "$ROOT/build" ] || cmake -S "$ROOT" -B "$ROOT/build" >/dev/null
    cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null
fi

if ! command -v "$FILECHECK" >/dev/null 2>&1; then
    log "${RED}FileCheck not found: $FILECHECK (set \$FILECHECK)${RESET}"
    exit 1
fi
if [ ! -x "$IRGEN" ]; then
    log "${RED}irgen not found at $IRGEN${RESET}"; exit 1
fi

run_check() {
    local src="$1" name
    name="$(basename "$src" .src)"
    local tmp_cwd; tmp_cwd="$(mktemp -d "$TMP_DIR/${name}.XXXX")"

    if ! (cd "$tmp_cwd" && "$IRGEN" < "$src" >/dev/null 2>&1) || [ ! -f "$tmp_cwd/module.ll" ]; then
        log "  ${RED}FAIL${RESET}  ${name}  (irgen produced no module.ll)"
        FAIL=$((FAIL + 1)); return
    fi

    local fc_out
    if fc_out=$("$FILECHECK" "$src" < "$tmp_cwd/module.ll" 2>&1); then
        log "  ${GREEN}PASS${RESET}  ${name}"
        PASS=$((PASS + 1))
    else
        log "  ${RED}FAIL${RESET}  ${name}  (IR did not match CHECK directives)"
        echo "$fc_out" | sed 's/^/         /'
        FAIL=$((FAIL + 1))
    fi
}

shaders=()
while IFS= read -r -d '' f; do shaders+=("$f"); done \
    < <(find "$CHECK_DIR" -name "*.src" -print0 | sort -z)

log "\n${CYAN}Running ${#shaders[@]} IR check(s)...${RESET}"
for s in "${shaders[@]}"; do run_check "$s"; done

log "────────────────────────────────"
log " IR checks: ${GREEN}PASS${RESET} $PASS  ${RED}FAIL${RESET} $FAIL"
log "────────────────────────────────"
[ "$FAIL" -eq 0 ]
