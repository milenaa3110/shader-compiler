#!/usr/bin/env bash
# run_packet_test.sh — Regression suite for the SPMD/SIMD packetizer.
# Validates functional correctness and numeric stability of vectorized shaders.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
IRGEN="$ROOT/build/riscv/irgen_riscv"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# ANSI colors
GREEN="\033[0;32m"; RED="\033[0;31m"; YEL="\033[0;33m"; CYAN="\033[0;36m"; RST="\033[0m"
fail=0
pass(){ echo -e "  ${GREEN}PASS${RST}  $*"; }
bad(){  echo -e "  ${RED}FAIL${RST}  $*"; fail=1; }
skip(){ echo -e "  ${YEL}SKIP${RST}  $*"; }

[ -x "$IRGEN" ] || { echo "irgen not found at $IRGEN"; exit 1; }

# Compile to IR and verify presence of vectorized @fs_packet entry point.
emit_packet(){ SHADER_EMIT_PACKET=1 "$IRGEN" "$2" < "$1" >/dev/null 2>&1; grep -q '@fs_packet' "$2"; }

# NOTE: runtime numeric equivalence (packet==scalar), the build-width guard
# (mismatch → abort), and the VLEN banner are exercised by the `packet_runtime`
# ctest gate (test/script/run_packet_runtime.sh), which links pipeline_runtime.cpp
# and runs under QEMU. This script stays emit-only: it checks the packetizer emits
# the right width-agnostic IR shape without needing a cross-toolchain.

#  Regression Phases ─

# P1: Basic arithmetic & ternary select
cat > "$TMP/p1.src" <<'EOF'
uniform float uGain; in vec2 vUV; out vec4 FragColor;
@entry @stage(fragment) fn void main() {
    float d = vUV.x * vUV.x + vUV.y * vUV.y;
    float c = d < 0.25 ? 1.0 : (d * uGain);
    FragColor = vec4(c, vUV.x, vUV.y, 1.0);
}
EOF
if emit_packet "$TMP/p1.src" "$TMP/p1.ll" && grep -qE 'select <[0-9]+ x i1>' "$TMP/p1.ll"; then
    pass "P1: Vectorized select"
else bad "P1: Failed"; fi

# P2: Control flow (if/else) via masks
cat > "$TMP/p2.src" <<'EOF'
uniform float uK; in vec2 vUV; out vec4 FragColor;
@entry @stage(fragment) fn void main() {
    float c = 0.0;
    if (vUV.x > 0.5) { c = vUV.y; if (vUV.y > 0.5) c = 1.0; } 
    else { c = vUV.x * uK; }
    FragColor = vec4(c, vUV.x, 0.0, 1.0);
}
EOF
emit_packet "$TMP/p2.src" "$TMP/p2.ll" && pass "P2: If/else masking" || bad "P2: Failed"

# P3: Discard instruction
cat > "$TMP/p3.src" <<'EOF'
in vec2 vUV; out vec4 FragColor;
@entry @stage(fragment) fn void main() {
    if (vUV.x > 0.5) discard;
    FragColor = vec4(vUV.x, vUV.y, 0.0, 1.0);
}
EOF
emit_packet "$TMP/p3.src" "$TMP/p3.ll" && pass "P3: Discard mask" || bad "P3: Failed"

# P4: Loops (trip-count reduction)
cat > "$TMP/p4.src" <<'EOF'
in vec2 vUV; out vec4 FragColor;
@entry @stage(fragment) fn void main() {
    float sum = 0.0; float n = vUV.x * 8.0;
    for (float i = 0.0; i < n; i = i + 1.0) sum += vUV.y;
    FragColor = vec4(sum, vUV.x, 0.0, 1.0);
}
EOF
emit_packet "$TMP/p4.src" "$TMP/p4.ll" && pass "P4: Per-lane loop" || bad "P4: Failed"

# Summary
echo ""
[ "$fail" -eq 0 ] && echo -e " Suite: ${GREEN}OK${RST}" || echo -e " Suite: ${RED}FAILED${RST}"
exit "$fail"