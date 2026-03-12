#!/usr/bin/env bash
# render.sh - compile a shade(vec2)->vec4 shader and render it to a PPM image
# Usage: ./render.sh [path/to/shader.src] [output.ppm]
#   Defaults: input/shader.src → result/<name>.ppm

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHADER="${1:-$ROOT/input/shader.src}"
NAME="$(basename "$SHADER" .src)"
OUTPUT="${2:-$ROOT/result/${NAME}.ppm}"

IRGEN="$ROOT/irgen"
OPT="${LLVM_OPT:-opt-18}"
LLC="${LLC:-llc-18}"
CXX="${CXX:-g++}"

mkdir -p "$ROOT/result"
TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

echo "Compiling $SHADER ..."

# 1. Compile shader → LLVM IR
"$IRGEN" < "$SHADER" 2>&1
mv "$ROOT/module.ll" "$TMP/module.ll"

# 2. Optimise IR
"$OPT" -O3 -S "$TMP/module.ll" -o "$TMP/module.opt.ll"

# 3. Compile to native object (PIC required for linking with PIE executables)
"$LLC" -O3 -filetype=obj -relocation-model=pic -o "$TMP/shader_native.o" "$TMP/module.opt.ll"

# 4. Link with test_host
"$CXX" -std=c++20 -O3 "$ROOT/test/riscv/test_host.cpp" "$TMP/shader_native.o" \
    -o "$TMP/render_host"

# 5. Render
(cd "$ROOT" && "$TMP/render_host")
mv "$ROOT/result/shader_out.ppm" "$OUTPUT"

echo "Image written to $OUTPUT"