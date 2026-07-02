#!/usr/bin/env bash
# run_tests.sh - Compiler integration test runner and IR validator

set -euo pipefail

# Resolve project directories relative to script location.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
IRGEN="$ROOT/build/riscv/irgen_riscv"
SHADER_DIR="$ROOT/test/shaders/compiler_tests"
LLVM_AS="${LLVM_AS:-llvm-as-18}"
TMP_DIR="$(mktemp -d)"

# ANSI terminal escape color codes.
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

# Parse command line flags.
for arg in "$@"; do
    case "$arg" in
        --no-build) NO_BUILD=1 ;;
        --verbose)  VERBOSE=1  ;;
    esac
done

# Lifecycle hook to ensure temporary directories are destroyed on exit.
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

log() { echo -e "$*"; }
verbose() { [ "$VERBOSE" -eq 1 ] && echo -e "$*" || true; }

#  Compilation Infrastructure Setup ─
if [ "$NO_BUILD" -eq 0 ]; then
    log "${CYAN}Building compiler binary...${RESET}"
    [ -d "$ROOT/build" ] || cmake -S "$ROOT" -B "$ROOT/build" >/dev/null
    
    # Compile using all available CPU cores. Trim log output unless verbose.
    if ! cmake --build "$ROOT/build" -j"$(nproc)" 2>&1 | \
            { [ "$VERBOSE" -eq 1 ] && cat || tail -5; }; then
        log "${RED}Build pipeline failed. Aborting test suite.${RESET}"
        exit 1
    fi
    log "${GREEN}Build successful${RESET}"
fi

if [ ! -x "$IRGEN" ]; then
    log "${RED}Error: irgen executable not found at $IRGEN${RESET}"
    exit 1
fi

#  Isolated Test Execution Engine ─
run_test() {
    local src="$1"
    local name
    name="$(basename "$src" .src)"
    local out_ll="$TMP_DIR/${name}.ll"

    verbose "\n  ${CYAN}[Testing ${name}]${RESET} from $src"

    # Create an isolated CWD subdirectory since irgen outputs 'module.ll' locally.
    local tmp_cwd
    tmp_cwd="$(mktemp -d "$TMP_DIR/${name}.XXXX")"

    # Execute code generation pass by piping source string into stdin.
    local irgen_out
    if ! irgen_out=$(cd "$tmp_cwd" && "$IRGEN" < "$src" 2>&1); then
        log "  ${RED}FAIL${RESET}  ${name}  (irgen returned non-zero exit status)"
        verbose "       irgen diagnostics:\n$(echo "$irgen_out" | sed 's/^/          /')"
        FAIL=$((FAIL + 1))
        return
    fi

    # Stage generated LLVM IR file for structural verification.
    cp "$tmp_cwd/module.ll" "$out_ll" 2>/dev/null || true

    if [ ! -f "$out_ll" ]; then
        log "  ${RED}FAIL${RESET}  ${name}  (irgen pass skipped file output generation)"
        verbose "       irgen diagnostics:\n$(echo "$irgen_out" | sed 's/^/          /')"
        FAIL=$((FAIL + 1))
        return
    fi

    # Assemble the text IR to /dev/null to trigger the LLVM IR Verifier pass.
    local as_out
    if ! as_out=$("$LLVM_AS" -o /dev/null "$out_ll" 2>&1); then
        log "  ${RED}FAIL${RESET}  ${name}  (llvm-as rejected malformed LLVM IR architecture)"
        verbose "       llvm-as diagnostics:\n$(echo "$as_out" | sed 's/^/          /')"
        FAIL=$((FAIL + 1))
        return
    fi

    log "  ${GREEN}PASS${RESET}  ${name}"
    PASS=$((PASS + 1))
}

#  Test Discovery Pass 
shaders=()
if [ $# -gt 0 ] && [ -f "$1" ]; then
    # Targeted run: use specific test file passed via arguments.
    shaders=("$@")
else
    # Suite run: discover all *.src files recursively inside the test tree.
    while IFS= read -r -d '' f; do
        shaders+=("$f")
    done < <(find "$SHADER_DIR" -name "*.src" -print0 | sort -z)
fi

log "\n${CYAN}Running ${#shaders[@]} shader test(s)...${RESET}"

# Execute main processing loop over identified targets.
for s in "${shaders[@]}"; do
    run_test "$s"
done

#  Summary and Suite Evaluation ─
TOTAL=$((PASS + FAIL + SKIP))
log ""
log " Results Evaluation: ${TOTAL} total"
log "  ${GREEN}PASS${RESET}  $PASS"
[ "$FAIL" -gt 0 ] && log "  ${RED}FAIL${RESET}  $FAIL" || log "  FAIL  $FAIL"
[ "$SKIP" -gt 0 ] && log "  ${YELLOW}SKIP${RESET}  $SKIP" || true

# Exit with non-zero code if any structural verification checks failed.
[ "$FAIL" -eq 0 ]