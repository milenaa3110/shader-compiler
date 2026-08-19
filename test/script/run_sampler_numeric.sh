#!/usr/bin/env bash
# run_sampler_numeric.sh — Executable numeric test that the sampler/image runtime
# reads REAL bound data (not a placeholder slice): cube-face selection, 3D volume
# slices, 2D-array layers, and storage-image texels.
#
# A vertex shader samples each type; the C++ driver defines the g_tex/g_img slot
# tables (matching tex_inline.h), fills them with distinct per-face/slice/layer/
# texel values, links the shader IR with tex_inline.bc, runs it, and checks the
# sampled values match. Uses the RISC-V IR path (texture() in a vertex shader is
# valid there — no derivatives needed). Skipped (not failed) if clang is absent.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
IRGEN="${IRGEN:-$ROOT/build/riscv/irgen_riscv}"
TEXBC="${TEXBC:-$ROOT/build/llvm/tex_inline.bc}"
CLANG="${CLANG:-clang-18}"
LLVM_DIS="${LLVM_DIS:-llvm-dis-18}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

GREEN="\033[0;32m"; RED="\033[0;31m"; YEL="\033[0;33m"; CYAN="\033[0;36m"; RST="\033[0m"

[ -x "$IRGEN" ] || { echo "irgen not found at $IRGEN"; exit 1; }
if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo -e "  ${YEL}SKIP${RST}  sampler numeric ($CLANG not installed)"; exit 0
fi
[ -f "$TEXBC" ] || { echo "tex_inline.bc not found at $TEXBC (build it first)"; exit 1; }

cat > "$TMP/s.src" <<'EOF'
layout(binding=0) uniform samplerCube uCube;
layout(binding=1) uniform sampler3D uVol;
layout(binding=2) uniform sampler2DArray uArr;
layout(binding=3) uniform image2D uImg;
layout(binding=4) uniform sampler2D uTex2;
out vec4 vCube;
out vec4 vVol;
out vec4 vArr;
out vec4 vImg;
@entry @stage(vertex)
fn void main() {
    vCube = vec4(texture(uCube, vec3(1.0,0.0,0.0)).x,
                 texture(uCube, vec3(0.0,1.0,0.0)).x,
                 texture(uCube, vec3(0.0,0.0,1.0)).x,
                 texture(uCube, vec3(-1.0,0.0,0.0)).x);
    vVol = vec4(texture(uVol, vec3(0.5,0.5,0.0)).x,
                texture(uVol, vec3(0.5,0.5,0.5)).x, 0.0, 0.0);
    vArr = vec4(texture(uArr, vec3(0.5,0.5,0.0)).x,
                texture(uArr, vec3(0.5,0.5,2.0)).x, 0.0, 0.0);
    imageStore(uImg, ivec2(0,1), vec4(7.0, 0.0, 0.0, 0.0));
    # textureLod: the runtime has no mip pyramid, so any LOD samples the base
    # level — lod 0.0 and lod 5.0 must return the same base texel. Packed into the
    # spare .z/.w of vImg to stay within the 16-float varying budget.
    vImg = vec4(imageLoad(uImg, ivec2(1,0)).x, imageLoad(uImg, ivec2(0,1)).x,
                textureLod(uTex2, vec2(0.0,0.0), 0.0).x,
                textureLod(uTex2, vec2(0.0,0.0), 5.0).x);
    gl_Position = vec4(0.0);
}
EOF

cat > "$TMP/d.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
// Must match tex_inline.h — the shader's __tex*/__image* (from tex_inline.bc)
// resolve g_tex/g_img against these definitions.
struct TexSlot { const float* data; int w, h, depth; };
struct ImgSlot { float* data; int w, h; };
TexSlot g_tex[8] = {};
ImgSlot g_img[8] = {};
extern "C" void vs_invoke(int, int, float*, double*, float*);
static int bad = 0;
static void eq(const char* w, float g, float want) {
    if (std::fabs(g - want) > 1e-3f) {
        std::printf("    MISMATCH %-8s got %.4f want %.4f\n", w, g, want);
        bad = 1;
    }
}
int main() {
    static float cube[6*4] = {0}; for (int f=0; f<6; ++f) cube[f*4] = f;      // 1x1x6, R=face
    static float vol[4*4]  = {0}; for (int s=0; s<4; ++s) vol[s*4] = s*10.f;  // 1x1x4, R=slice*10
    static float arr[3*4]  = {0}; for (int l=0; l<3; ++l) arr[l*4] = l*100.f; // 1x1x3, R=layer*100
    static float img[2*2*4] = {0}; img[(0*2+1)*4] = 42.f;                     // 2x2, texel(1,0).R=42
    static float tex2[4] = {55.f, 0, 0, 0};                                  // 1x1, R=55 (base level)
    g_tex[0]={cube,1,1,6}; g_tex[1]={vol,1,1,4}; g_tex[2]={arr,1,1,3}; g_tex[4]={tex2,1,1,1};
    g_img[3]={img,2,2};
    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    // Cube face selection: +X=face0, +Y=face2, +Z=face4, -X=face1.
    eq("cube.x",out[4],0.f); eq("cube.y",out[5],2.f); eq("cube.z",out[6],4.f); eq("cube.w",out[7],1.f);
    // 3D: z=0 → slice0 (0) ; z=0.5 → trilinear midpoint of slice1(10)/slice2(20)=15.
    eq("vol.x",out[8],0.f);  eq("vol.y",out[9],15.f);
    // Array: layer0 (0) ; layer2 (200).
    eq("arr.x",out[12],0.f); eq("arr.y",out[13],200.f);
    // Storage image: pre-bound texel(1,0).R = 42 ; imageStore→imageLoad round-trip
    // at (0,1) reads back 7.
    eq("img.load",out[16],42.f); eq("img.store",out[17],7.f);
    // textureLod: no mip pyramid → both LODs (0.0 and 5.0) sample base texel 55
    // (packed into vImg.z/.w = out[18]/out[19]).
    eq("lod0",out[18],55.f); eq("lod5",out[19],55.f);
    return bad;
}
EOF

echo -e "\n${CYAN}Evaluating sampler/image real-data equivalence...${RST}"
if ! "$IRGEN" "$TMP/m.ll" < "$TMP/s.src" >/dev/null 2>&1; then
    echo -e "  ${RED}FAIL${RST}  shader failed to compile"; exit 1
fi
sed -E '/^target (triple|datalayout)/d; /^attributes #/d; s/ #0//g' "$TMP/m.ll" > "$TMP/host.ll"
"$LLVM_DIS" "$TEXBC" -o "$TMP/tex.ll" 2>/dev/null
if ! "$CLANG" -O2 "$TMP/host.ll" "$TMP/tex.ll" "$TMP/d.cpp" -lm -o "$TMP/t" 2>/dev/null; then
    echo -e "  ${RED}FAIL${RST}  link failed"; exit 1
fi
if "$TMP/t"; then
    echo -e "  ${GREEN}PASS${RST}  cube faces / 3D slices / array layers / image texels / textureLod-clamps-to-base (real bound data)"
    echo -e " Sampler Numeric Suite: ${GREEN}PASS${RST}\n"; exit 0
else
    echo -e "  ${RED}FAIL${RST}  wrong sampled values"
    echo -e " Sampler Numeric Suite: ${RED}FAIL${RST}\n"; exit 1
fi
