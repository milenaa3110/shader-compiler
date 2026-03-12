#!/usr/bin/env bash
# run_pipeline_tests.sh — build and run the VS+FS pipeline for each shader pair
# in test/shaders/pipeline/ and verify an output image is produced.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IRGEN="$ROOT/irgen"
LLC="${LLC:-llc-18}"
LLVM_LINK="${LLVM_LINK:-llvm-link-18}"
LLVM_AS="${LLVM_AS:-llvm-as-18}"
PIPELINE_DIR="$ROOT/pipeline"
TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

GREEN="\033[0;32m"; RED="\033[0;31m"; CYAN="\033[0;36m"; RESET="\033[0m"
PASS=0; FAIL=0

log() { echo -e "$*"; }

run_pair() {
    local vs_src="$1" fs_src="$2" name="$3"
    log "\n  ${CYAN}[${name}]${RESET}"

    local ws="$TMP/${name}"
    mkdir -p "$ws"

    # Compile VS
    if ! (cd "$ws" && "$IRGEN" < "$vs_src" > /dev/null 2>&1); then
        log "  ${RED}FAIL${RESET}  ${name}  (VS compile error)"
        FAIL=$((FAIL+1)); return
    fi
    mv "$ws/module.ll" "$ws/vs.ll"

    # Compile FS
    if ! (cd "$ws" && "$IRGEN" < "$fs_src" > /dev/null 2>&1); then
        log "  ${RED}FAIL${RESET}  ${name}  (FS compile error)"
        FAIL=$((FAIL+1)); return
    fi
    mv "$ws/module.ll" "$ws/fs.ll"

    # Link VS+FS
    if ! "$LLVM_LINK" "$ws/vs.ll" "$ws/fs.ll" -S -o "$ws/pipeline.ll" 2>/dev/null; then
        log "  ${RED}FAIL${RESET}  ${name}  (llvm-link failed)"
        FAIL=$((FAIL+1)); return
    fi

    # Validate combined IR
    if ! "$LLVM_AS" -o /dev/null "$ws/pipeline.ll" 2>/dev/null; then
        log "  ${RED}FAIL${RESET}  ${name}  (invalid combined IR)"
        FAIL=$((FAIL+1)); return
    fi

    # Compile to native .o
    if ! "$LLC" -O3 -filetype=obj -relocation-model=pic \
            "$ws/pipeline.ll" -o "$ws/shader.o" 2>/dev/null; then
        log "  ${RED}FAIL${RESET}  ${name}  (llc failed)"
        FAIL=$((FAIL+1)); return
    fi

    # Compile pipeline_runtime.cpp
    if ! g++ -std=c++20 -O3 -fPIC -I"$PIPELINE_DIR" \
             -c "$PIPELINE_DIR/pipeline_runtime.cpp" \
             -o "$ws/pipeline_runtime.o" 2>/dev/null; then
        log "  ${RED}FAIL${RESET}  ${name}  (pipeline_runtime compile failed)"
        FAIL=$((FAIL+1)); return
    fi

    # Link host + runtime + shader
    local out_bin="$ws/pipeline_host"
    if ! g++ -std=c++20 -O3 -I"$PIPELINE_DIR" \
             "$PIPELINE_DIR/pipeline_host.cpp" \
             "$ws/shader.o" "$ws/pipeline_runtime.o" \
             -o "$out_bin" 2>/dev/null; then
        log "  ${RED}FAIL${RESET}  ${name}  (link failed)"
        FAIL=$((FAIL+1)); return
    fi

    # Run
    mkdir -p "$ROOT/result"
    if ! (cd "$ROOT" && "$out_bin" > /dev/null 2>&1); then
        log "  ${RED}FAIL${RESET}  ${name}  (runtime crash)"
        FAIL=$((FAIL+1)); return
    fi

    # Check output image
    if [ ! -s "$ROOT/result/pipeline_out.ppm" ]; then
        log "  ${RED}FAIL${RESET}  ${name}  (no image produced)"
        FAIL=$((FAIL+1)); return
    fi

    # Move to named output
    cp "$ROOT/result/pipeline_out.ppm" "$ROOT/result/pipeline_${name}.ppm"
    log "  ${GREEN}PASS${RESET}  ${name}  → result/pipeline_${name}.ppm"
    PASS=$((PASS+1))
}

log "${CYAN}Pipeline tests${RESET}"

SHADER_DIR="$ROOT/test/shaders/pipeline"
run_pair "$SHADER_DIR/triangle_vs.src"  "$SHADER_DIR/triangle_fs.src"  "triangle"
run_pair "$SHADER_DIR/scene_vs.src"     "$SHADER_DIR/scene_fs.src"     "scene"
run_pair "$SHADER_DIR/anim_vs.src"      "$SHADER_DIR/anim_fs.src"      "anim"

echo ""
echo "────────────────────────────────"
echo " Pipeline results: PASS=$PASS  FAIL=$FAIL"
echo "────────────────────────────────"
[ "$FAIL" -eq 0 ]