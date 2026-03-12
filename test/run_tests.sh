#!/usr/bin/env bash
# run_tests.sh - compile and validate each test shader
# Usage: ./test/run_tests.sh [--no-build] [--verbose]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
IRGEN="$ROOT/irgen"
SHADER_DIR="$SCRIPT_DIR/shaders"
LLVM_AS="${LLVM_AS:-llvm-as-18}"
TMP_DIR="$(mktemp -d)"

# ── colours ──────────────────────────────────────────────────────────────────
GREEN="\033[0;32m"
RED="\033[0;31m"
YELLOW="\033[0;33m"
CYAN="\033[0;36m"
RESET="\033[0m"

PASS=0
FAIL=0
SKIP=0
VERBOSE=0
NO_BUILD=0

for arg in "$@"; do
    case "$arg" in
        --no-build) NO_BUILD=1 ;;
        --verbose)  VERBOSE=1  ;;
    esac
done

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

log() { echo -e "$*"; }
verbose() { [ "$VERBOSE" -eq 1 ] && echo -e "$*" || true; }

# ── build ─────────────────────────────────────────────────────────────────────
if [ "$NO_BUILD" -eq 0 ]; then
    log "${CYAN}Building...${RESET}"
    if ! make -C "$ROOT" all -j"$(nproc)" 2>&1 | \
            { [ "$VERBOSE" -eq 1 ] && cat || tail -5; }; then
        log "${RED}Build failed. Aborting tests.${RESET}"
        exit 1
    fi
    log "${GREEN}Build OK${RESET}"
fi

if [ ! -x "$IRGEN" ]; then
    log "${RED}irgen not found at $IRGEN${RESET}"
    exit 1
fi

# ── run one test ──────────────────────────────────────────────────────────────
run_test() {
    local src="$1"
    local name
    name="$(basename "$src" .src)"
    local out_ll="$TMP_DIR/${name}.ll"

    verbose "\n  ${CYAN}[${name}]${RESET} $src"

    # irgen reads from stdin, writes module.ll in CWD
    local tmp_cwd
    tmp_cwd="$(mktemp -d "$TMP_DIR/${name}.XXXX")"

    local irgen_out
    if ! irgen_out=$(cd "$tmp_cwd" && "$IRGEN" < "$src" 2>&1); then
        log "  ${RED}FAIL${RESET}  ${name}  (irgen exited non-zero)"
        verbose "       irgen output:\n$(echo "$irgen_out" | sed 's/^/         /')"
        FAIL=$((FAIL + 1))
        return
    fi

    # copy the generated IR
    cp "$tmp_cwd/module.ll" "$out_ll" 2>/dev/null || true

    if [ ! -f "$out_ll" ]; then
        log "  ${RED}FAIL${RESET}  ${name}  (module.ll not produced)"
        verbose "       irgen output:\n$(echo "$irgen_out" | sed 's/^/         /')"
        FAIL=$((FAIL + 1))
        return
    fi

    # validate IR with llvm-as
    local as_out
    if ! as_out=$("$LLVM_AS" -o /dev/null "$out_ll" 2>&1); then
        log "  ${RED}FAIL${RESET}  ${name}  (invalid LLVM IR)"
        verbose "       llvm-as:\n$(echo "$as_out" | sed 's/^/         /')"
        FAIL=$((FAIL + 1))
        return
    fi

    log "  ${GREEN}PASS${RESET}  ${name}"
    PASS=$((PASS + 1))
}

# ── discover shaders ──────────────────────────────────────────────────────────
shaders=()
if [ $# -gt 0 ] && [ -f "$1" ]; then
    # specific file passed
    shaders=("$@")
else
    # all shaders in test/shaders/
    while IFS= read -r -d '' f; do
        shaders+=("$f")
    done < <(find "$SHADER_DIR" -name "*.src" -print0 | sort -z)
fi

log "\n${CYAN}Running ${#shaders[@]} shader test(s)...${RESET}"

for s in "${shaders[@]}"; do
    run_test "$s"
done

# ── summary ───────────────────────────────────────────────────────────────────
TOTAL=$((PASS + FAIL + SKIP))
log ""
log "────────────────────────────────"
log " Results: ${TOTAL} tests"
log "  ${GREEN}PASS${RESET}  $PASS"
[ "$FAIL"  -gt 0 ] && log "  ${RED}FAIL${RESET}  $FAIL"  || log "  FAIL  $FAIL"
[ "$SKIP"  -gt 0 ] && log "  ${YELLOW}SKIP${RESET}  $SKIP" || true
log "────────────────────────────────"

[ "$FAIL" -eq 0 ]
