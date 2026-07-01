#!/usr/bin/env bash
# run_packet_test.sh — Route B regression for the SPMD `fs_packet` packetizer.
#
#   Phase 1 — value model: straight-line + ternary -> per-lane <W x float> with
#             `select`; numeric equivalence vs the scalar shader on every lane.
#   Phase 2 — mask-CFG: `if/else` (incl. nested) via execution masks; writes
#             blend by mask; numeric equivalence across all control-flow paths.
#   BAIL    — a construct still outside the supported subset (a `for` loop)
#             emits NO fs_packet and leaves a valid scalar module.
#
# The numeric steps need clang-18 (run the width-W packet natively, compare to a
# scalar reference). They are skipped, not failed, if clang-18 is absent.
#
# Usage: ./test/script/run_packet_test.sh   (expects build/riscv/irgen_riscv)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
IRGEN="$ROOT/build/riscv/irgen_riscv"
LLVM_AS="${LLVM_AS:-llvm-as-18}"
CLANG="${CLANG:-clang-18}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
GREEN="\033[0;32m"; RED="\033[0;31m"; YEL="\033[0;33m"; RST="\033[0m"
fail=0
pass(){ echo -e "  ${GREEN}PASS${RST}  $*"; }
bad(){  echo -e "  ${RED}FAIL${RST}  $*"; fail=1; }
skip(){ echo -e "  ${YEL}SKIP${RST}  $*"; }

[ -x "$IRGEN" ] || { echo "irgen not found at $IRGEN"; exit 1; }
have_clang=0; command -v "$CLANG" >/dev/null 2>&1 && have_clang=1

# emit_packet <src> <out.ll>  -> 0 if fs_packet emitted
emit_packet(){ SHADER_EMIT_PACKET=1 "$IRGEN" "$2" < "$1" >/dev/null 2>&1; grep -q '@fs_packet' "$2"; }

# run_numeric <out.ll> <driver.cpp> <label>
run_numeric(){
    [ "$have_clang" -eq 1 ] || { skip "numeric: $CLANG not found ($3)"; return; }
    sed -E '/^target (triple|datalayout)/d; /^attributes #/d; s/ #0//g' "$1" > "$TMP/host.ll"
    if "$CLANG" -O2 "$TMP/host.ll" "$2" -lm -o "$TMP/t" 2>/dev/null && "$TMP/t"; then
        pass "numeric: $3 matches scalar reference on all lanes"
    else
        bad "numeric: $3 diverges from scalar reference"
    fi
}

# ── Phase 1: value model ─────────────────────────────────────────────────────
cat > "$TMP/p1.src" <<'EOF'
uniform float uGain;
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    float d = vUV.x * vUV.x + vUV.y * vUV.y;
    float c = d < 0.25 ? 1.0 : (d * uGain);
    FragColor = vec4(c, vUV.x, vUV.y, 1.0);
}
EOF
if emit_packet "$TMP/p1.src" "$TMP/p1.ll" \
   && grep -qE 'fmul <4 x float>' "$TMP/p1.ll" && grep -qE 'select <4 x i1>' "$TMP/p1.ll" \
   && "$LLVM_AS" "$TMP/p1.ll" -o /dev/null 2>/dev/null; then
    pass "phase1 emit: <4 x float> SIMD with per-lane select"
else bad "phase1 emit: missing/invalid"; fi
cat > "$TMP/p1.cpp" <<'EOF'
#include <cmath>
extern "C" void fs_packet(const float*,const float*,float*,int*); extern "C" float uGain;
int main(){const int W=4;uGain=2.0f;float ux[W]={.1f,.4f,.7f,0},uy[W]={.1f,.3f,.2f,.9f};
 float v[2*W];for(int i=0;i<W;i++){v[i]=ux[i];v[W+i]=uy[i];}float f[4*W]={0},o[4*W];int lv[W];fs_packet(v,f,o,lv);
 int b=0;for(int i=0;i<W;i++){float d=ux[i]*ux[i]+uy[i]*uy[i];float c=d<0.25f?1.0f:d*uGain;
 float r[4]={c,ux[i],uy[i],1};for(int k=0;k<4;k++)if(std::fabs(o[k*W+i]-r[k])>1e-5f)b++; if(lv[i]!=-1)b++;}return b?1:0;}
EOF
run_numeric "$TMP/p1.ll" "$TMP/p1.cpp" "phase1 straight-line+ternary"

# ── Phase 2: mask-CFG (nested if/else) ───────────────────────────────────────
cat > "$TMP/p2.src" <<'EOF'
uniform float uK;
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    float c = 0.0;
    if (vUV.x > 0.5) {
        c = vUV.y;
        if (vUV.y > 0.5) { c = 1.0; }
    } else {
        c = vUV.x * uK;
    }
    FragColor = vec4(c, vUV.x, 0.0, 1.0);
}
EOF
if emit_packet "$TMP/p2.src" "$TMP/p2.ll" \
   && grep -qE 'then\.mask' "$TMP/p2.ll" && grep -qE 'else\.mask' "$TMP/p2.ll" \
   && "$LLVM_AS" "$TMP/p2.ll" -o /dev/null 2>/dev/null; then
    pass "phase2 emit: if/else lowered to execution masks"
else bad "phase2 emit: masks missing/invalid"; fi
cat > "$TMP/p2.cpp" <<'EOF'
#include <cmath>
extern "C" void fs_packet(const float*,const float*,float*,int*); extern "C" float uK;
int main(){const int W=4;uK=3.0f;float ux[W]={.7f,.8f,.2f,.9f},uy[W]={.9f,.3f,.4f,.55f};
 float v[2*W];for(int i=0;i<W;i++){v[i]=ux[i];v[W+i]=uy[i];}float f[4*W]={0},o[4*W];int lv[W];fs_packet(v,f,o,lv);
 int b=0;for(int i=0;i<W;i++){float c=0;if(ux[i]>.5f){c=uy[i];if(uy[i]>.5f)c=1;}else c=ux[i]*uK;
 float r[4]={c,ux[i],0,1};for(int k=0;k<4;k++)if(std::fabs(o[k*W+i]-r[k])>1e-5f)b++; if(lv[i]!=-1)b++;}return b?1:0;}
EOF
run_numeric "$TMP/p2.ll" "$TMP/p2.cpp" "phase2 nested if/else"

# ── Phase 3: discard (per-lane liveness side-channel) ────────────────────────
cat > "$TMP/p3.src" <<'EOF'
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    if (vUV.x > 0.5) { discard; }
    FragColor = vec4(vUV.x, vUV.y, 0.0, 1.0);
}
EOF
if emit_packet "$TMP/p3.src" "$TMP/p3.ll" \
   && grep -qE 'live\.i32|sext' "$TMP/p3.ll" \
   && "$LLVM_AS" "$TMP/p3.ll" -o /dev/null 2>/dev/null; then
    pass "phase3 emit: discard lowers to per-lane liveness mask"
else bad "phase3 emit: liveness mask missing/invalid"; fi
cat > "$TMP/p3.cpp" <<'EOF'
#include <cmath>
extern "C" void fs_packet(const float*,const float*,float*,int*);
extern "C" void __frag_discard(){}  // stub: the scalar fs_main in the module refs it
int main(){const int W=4;float ux[W]={.7f,.2f,.9f,.4f},uy[W]={.1f,.5f,.3f,.8f};
 float v[2*W];for(int i=0;i<W;i++){v[i]=ux[i];v[W+i]=uy[i];}float f[4*W]={0},o[4*W];int lv[W];fs_packet(v,f,o,lv);
 int b=0;for(int i=0;i<W;i++){int live = ux[i]>0.5f ? 0 : -1; if(lv[i]!=live)b++;}return b?1:0;}
EOF
run_numeric "$TMP/p3.ll" "$TMP/p3.cpp" "phase3 discard liveness"

# ── Phase 4: per-lane loops (data-dependent trip count, loop-carried local) ───
cat > "$TMP/p4.src" <<'EOF'
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    float sum = 0.0;
    float n = vUV.x * 8.0;
    for (float i = 0.0; i < n; i = i + 1.0) { sum = sum + vUV.y; }
    FragColor = vec4(sum, vUV.x, 0.0, 1.0);
}
EOF
if emit_packet "$TMP/p4.src" "$TMP/p4.ll" \
   && grep -qE 'loop\.header' "$TMP/p4.ll" && grep -qE 'vector\.reduce\.or' "$TMP/p4.ll" \
   && "$LLVM_AS" "$TMP/p4.ll" -o /dev/null 2>/dev/null; then
    pass "phase4 emit: per-lane loop CFG with active-mask reduction"
else bad "phase4 emit: loop CFG missing/invalid"; fi
cat > "$TMP/p4.cpp" <<'EOF'
#include <cmath>
extern "C" void fs_packet(const float*,const float*,float*,int*);
int main(){const int W=4;float ux[W]={.125f,.5f,.9f,0},uy[W]={1,2,.5f,3};
 float v[2*W];for(int i=0;i<W;i++){v[i]=ux[i];v[W+i]=uy[i];}float f[4*W]={0},o[4*W];int lv[W];fs_packet(v,f,o,lv);
 int b=0;for(int i=0;i<W;i++){float s=0,n=ux[i]*8.0f;for(float t=0;t<n;t=t+1)s=s+uy[i];
 float r[4]={s,ux[i],0,1};for(int k=0;k<4;k++)if(std::fabs(o[k*W+i]-r[k])>1e-4f)b++;}return b?1:0;}
EOF
run_numeric "$TMP/p4.ll" "$TMP/p4.cpp" "phase4 per-lane trip count + accumulator"

# ── Phase 4b: break (per-lane early exit) ────────────────────────────────────
cat > "$TMP/brk.src" <<'EOF'
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    float sum = 0.0;
    float cap = vUV.x * 5.0;
    for (float i = 0.0; i < 8.0; i = i + 1.0) {
        if (sum > cap) { break; }
        sum = sum + vUV.y;
    }
    FragColor = vec4(sum, cap, 0.0, 1.0);
}
EOF
emit_packet "$TMP/brk.src" "$TMP/brk.ll" && "$LLVM_AS" "$TMP/brk.ll" -o /dev/null 2>/dev/null \
    && pass "phase4b emit: loop with break is packetized" || bad "phase4b break: not packetized"
cat > "$TMP/brk.cpp" <<'EOF'
#include <cmath>
extern "C" void fs_packet(const float*,const float*,float*,int*);
int main(){int W=4;float ux[4]={.1f,.4f,.7f,1.0f},uy[4]={1.0f,.5f,2.0f,.3f};
 float v[8];for(int i=0;i<W;i++){v[i]=ux[i];v[W+i]=uy[i];}float f[16]={0},o[16];int lv[4];fs_packet(v,f,o,lv);
 int b=0;for(int i=0;i<W;i++){float s=0,cap=ux[i]*5.0f;for(float t=0;t<8.0f;t=t+1){if(s>cap)break;s=s+uy[i];}
 if(std::fabs(o[i]-s)>1e-4f)b++;}return b?1:0;}
EOF
run_numeric "$TMP/brk.ll" "$TMP/brk.cpp" "phase4b break (per-lane exit point)"

# ── Phase 4b: continue (per-lane skipped iterations) ─────────────────────────
cat > "$TMP/cont.src" <<'EOF'
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    float sum = 0.0;
    float skip = vUV.x * 4.0;
    for (float i = 0.0; i < 6.0; i = i + 1.0) {
        if (i < skip) { continue; }
        sum = sum + vUV.y;
    }
    FragColor = vec4(sum, skip, 0.0, 1.0);
}
EOF
emit_packet "$TMP/cont.src" "$TMP/cont.ll" && "$LLVM_AS" "$TMP/cont.ll" -o /dev/null 2>/dev/null \
    && pass "phase4b emit: loop with continue is packetized" || bad "phase4b continue: not packetized"
cat > "$TMP/cont.cpp" <<'EOF'
#include <cmath>
extern "C" void fs_packet(const float*,const float*,float*,int*);
int main(){int W=4;float ux[4]={.1f,.4f,.7f,1.0f},uy[4]={1.0f,.5f,2.0f,.3f};
 float v[8];for(int i=0;i<W;i++){v[i]=ux[i];v[W+i]=uy[i];}float f[16]={0},o[16];int lv[4];fs_packet(v,f,o,lv);
 int b=0;for(int i=0;i<W;i++){float s=0,sk=ux[i]*4.0f;for(float t=0;t<6.0f;t=t+1){if(t<sk)continue;s=s+uy[i];}
 if(std::fabs(o[i]-s)>1e-4f)b++;}return b?1:0;}
EOF
run_numeric "$TMP/cont.ll" "$TMP/cont.cpp" "phase4b continue (per-lane skip)"

# ── Phase 4b: texture gather (scalarized; packet must match scalar) ───────────
cat > "$TMP/tex.src" <<'EOF'
uniform sampler2D uTex;
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    vec4 t = texture(uTex, vUV);
    FragColor = vec4(t.r * 2.0, t.g + vUV.x, t.b, 1.0);
}
EOF
emit_packet "$TMP/tex.src" "$TMP/tex.ll" && grep -q '__tex2d_sample' "$TMP/tex.ll" \
   && "$LLVM_AS" "$TMP/tex.ll" -o /dev/null 2>/dev/null \
   && pass "phase4b emit: texture lowered to per-lane gather" || bad "phase4b texture: missing/invalid"
cat > "$TMP/tex.cpp" <<'EOF'
#include <cmath>
extern "C" void fs_packet(const float*,const float*,float*,int*);
extern "C" void fs_invoke(float*,float*,float*,double*);
extern "C" void __tex2d_sample(void*,float u,float v,float* o){o[0]=u;o[1]=v;o[2]=u*v;o[3]=1.0f;}
int main(){int W=4;float ux[4]={.1f,.4f,.7f,.9f},uy[4]={.2f,.6f,.3f,.8f};
 float v[8];for(int i=0;i<W;i++){v[i]=ux[i];v[W+i]=uy[i];}float f[16]={0},o[16];int lv[4];fs_packet(v,f,o,lv);
 int b=0;for(int i=0;i<W;i++){float so[4],fc[4]={0,0,0,1},vv[2]={ux[i],uy[i]};double od[1];fs_invoke(fc,vv,so,od);
 for(int k=0;k<4;k++)if(std::fabs(o[k*W+i]-so[k])>1e-4f)b++;}return b?1:0;}
EOF
run_numeric "$TMP/tex.ll" "$TMP/tex.cpp" "phase4b texture gather (vs scalar fs_invoke)"

# ── Phase 4c: math builtins (sin/cos/clamp/mix/length) — vectorized ───────────
cat > "$TMP/math.src" <<'EOF'
uniform float uT;
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    float x = vUV.x; float y = vUV.y;
    float s = sin(x*6.0 + uT);
    float c = cos(y*6.0 - uT);
    float r = clamp(s*0.5 + 0.5, 0.0, 1.0);
    float g = mix(s, c, x);
    float l = length(vec2(x - 0.5, y - 0.5));
    FragColor = vec4(r, g, l, 1.0);
}
EOF
emit_packet "$TMP/math.src" "$TMP/math.ll" && grep -q 'llvm.sin.v4f32' "$TMP/math.ll" \
   && "$LLVM_AS" "$TMP/math.ll" -o /dev/null 2>/dev/null \
   && pass "phase4c emit: math builtins -> vector intrinsics" || bad "phase4c math: missing/invalid"
cat > "$TMP/math.cpp" <<'EOF'
#include <cmath>
extern "C" void fs_packet(const float*,const float*,float*,int*);
extern "C" void fs_invoke(float*,float*,float*,double*);
extern "C" float uT;
int main(){int W=4;uT=0.7f;float ux[4]={.1f,.45f,.72f,.95f},uy[4]={.2f,.6f,.33f,.85f};
 float v[8];for(int i=0;i<W;i++){v[i]=ux[i];v[W+i]=uy[i];}float f[16]={0},o[16];int lv[4];fs_packet(v,f,o,lv);
 int b=0;for(int i=0;i<W;i++){float so[4],fc[4]={0,0,0,1},vv[2]={ux[i],uy[i]};double od[1];fs_invoke(fc,vv,so,od);
 for(int k=0;k<4;k++)if(std::fabs(o[k*W+i]-so[k])>1e-4f)b++;}return b?1:0;}
EOF
run_numeric "$TMP/math.ll" "$TMP/math.cpp" "phase4c sin/cos/clamp/mix/length (vs scalar)"

# ── Phase 4c: step / smoothstep ──────────────────────────────────────────────
cat > "$TMP/ss.src" <<'EOF'
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    float a = step(0.5, vUV.x);
    float b = smoothstep(0.2, 0.8, vUV.y);
    FragColor = vec4(a, b, vUV.x * vUV.y, 1.0);
}
EOF
emit_packet "$TMP/ss.src" "$TMP/ss.ll" && "$LLVM_AS" "$TMP/ss.ll" -o /dev/null 2>/dev/null \
    && pass "phase4c emit: step/smoothstep packetized" || bad "phase4c step/smoothstep: not packetized"
cat > "$TMP/ss.cpp" <<'EOF'
#include <cmath>
extern "C" void fs_packet(const float*,const float*,float*,int*);
extern "C" void fs_invoke(float*,float*,float*,double*);
int main(){int W=4;float ux[4]={.1f,.5f,.7f,.9f},uy[4]={.15f,.5f,.83f,.6f};
 float v[8];for(int i=0;i<W;i++){v[i]=ux[i];v[W+i]=uy[i];}float f[16]={0},o[16];int lv[4];fs_packet(v,f,o,lv);
 int b=0;for(int i=0;i<W;i++){float so[4],fc[4]={0,0,0,1},vv[2]={ux[i],uy[i]};double od[1];fs_invoke(fc,vv,so,od);
 for(int k=0;k<4;k++)if(std::fabs(o[k*W+i]-so[k])>1e-4f)b++;}return b?1:0;}
EOF
run_numeric "$TMP/ss.ll" "$TMP/ss.cpp" "phase4c step/smoothstep (vs scalar)"

# ── VS: the same packetizer applied to a vertex shader (gl_VertexID-driven) ───
cat > "$TMP/vs.src" <<'EOF'
out vec2 vUV;
@entry @stage(vertex)
fn void main() {
    float vid = float(gl_VertexID);
    float px = -1.0; float py = -1.0;
    if (vid == 1.0) { px = 3.0; py = -1.0; }
    if (vid == 2.0) { px = -1.0; py = 3.0; }
    vUV = vec2((px + 1.0) * 0.5, (py + 1.0) * 0.5);
    gl_Position = vec4(px, py, 0.0, 1.0);
}
EOF
SHADER_EMIT_PACKET=1 "$IRGEN" "$TMP/vs.ll" < "$TMP/vs.src" >/dev/null 2>&1
if grep -q '@vs_packet' "$TMP/vs.ll" && grep -qE 'add <4 x i32>' "$TMP/vs.ll" \
   && "$LLVM_AS" "$TMP/vs.ll" -o /dev/null 2>/dev/null; then
    pass "vs emit: vs_packet with per-lane gl_VertexID"
else bad "vs emit: vs_packet missing/invalid"; fi
cat > "$TMP/vs.cpp" <<'EOF'
#include <cmath>
extern "C" void vs_packet(const float*,int,int,float*);
extern "C" void vs_invoke(int,int,float*,double*,float*);
extern "C" int vs_total_floats;
int main(){int W=4,vtot=vs_total_floats;float po[64*4];vs_packet(nullptr,0,0,po);
 int b=0;for(int l=0;l<W;l++){float so[64];vs_invoke(l,0,nullptr,nullptr,so);
 for(int c=0;c<vtot;c++)if(std::fabs(po[c*W+l]-so[c])>1e-4f)b++;}return b?1:0;}
EOF
run_numeric "$TMP/vs.ll" "$TMP/vs.cpp" "vs per-lane gl_VertexID (vs scalar vs_invoke)"

# ── BAIL: an unsupported builtin (textureLod) stays outside the subset ────────
cat > "$TMP/bail.src" <<'EOF'
uniform sampler2D uTex;
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() { FragColor = textureLod(uTex, vUV, 0.0); }
EOF
SHADER_EMIT_PACKET=1 "$IRGEN" "$TMP/bail.ll" < "$TMP/bail.src" >/dev/null 2>&1
if ! grep -q '@fs_packet' "$TMP/bail.ll" && grep -q '@fs_main' "$TMP/bail.ll" \
   && "$LLVM_AS" "$TMP/bail.ll" -o /dev/null 2>/dev/null; then
    pass "bail: textureLod FS emits no fs_packet, scalar intact"
else bad "bail: fs_packet leaked or module invalid"; fi

# ── Phase 5b: runtime integration — packet vs scalar render is bit-identical ──
# Builds a VS+FS pipeline, links the real software rasterizer for the host, and
# renders the same frame both ways (SHADER_PACKET off vs on). Needs clang++-18.
CXX="${CLANGXX:-clang++-18}"
if command -v "$CXX" >/dev/null 2>&1; then
    cat > "$TMP/iv.src" <<'EOF'
out vec2 vUV;
@entry @stage(vertex)
fn void main() {
    float vid = float(gl_VertexID);
    float px = -1.0; float py = -1.0;
    if (vid == 1.0) { px = 3.0; py = -1.0; }
    if (vid == 2.0) { px = -1.0; py = 3.0; }
    vUV = vec2((px + 1.0) * 0.5, (py + 1.0) * 0.5);
    gl_Position = vec4(px, py, 0.0, 1.0);
}
EOF
    cat > "$TMP/if.src" <<'EOF'
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    float r = sin(vUV.x*6.0)*0.5 + 0.5;
    float g = cos(vUV.y*6.0)*0.5 + 0.5;
    float b = vUV.x * vUV.y;
    if (vUV.x > 0.8) { b = 1.0; }
    FragColor = vec4(r, g, b, 1.0);
}
EOF
    "$IRGEN" "$TMP/iv.ll" < "$TMP/iv.src" >/dev/null 2>&1
    "$IRGEN" "$TMP/if.ll" < "$TMP/if.src" >/dev/null 2>&1
    grep -q '@vs_packet' "$TMP/iv.ll" || bad "phase5b: VS did not packetize"
    grep -q '@fs_packet' "$TMP/if.ll" || bad "phase5b: FS did not packetize"
    for m in iv if; do sed -E '/^target (triple|datalayout)/d; /^attributes #/d; s/ #0//g' "$TMP/$m.ll" > "$TMP/${m}h.ll"; done
    cat > "$TMP/cmp.cpp" <<'EOF'
#include "pipeline_runtime.h"
#include <cstdlib>
#include <cstdio>
#include <vector>
int main(){
    const int W=128,H=128; std::vector<unsigned char> a(W*H*3), b(W*H*3);
    PipelineDesc desc{W,H,3,nullptr,nullptr,0}; desc.clear=true;
    unsetenv("SHADER_PACKET"); render_pipeline(desc, a.data());
    setenv("SHADER_PACKET","1",1); render_pipeline(desc, b.data());
    int diff=0; for(size_t i=0;i<a.size();i++) if(a[i]!=b[i]) diff++;
    return diff;  // 0 == bit-identical
}
EOF
    if "$CXX" -O2 -I "$ROOT/src/runtime" "$TMP/ivh.ll" "$TMP/ifh.ll" \
         "$ROOT/src/runtime/pipeline_runtime.cpp" "$TMP/cmp.cpp" -lm -o "$TMP/cmp" 2>/dev/null \
       && "$TMP/cmp"; then
        pass "phase5b integration: packet vs scalar render is bit-identical"
    else
        bad "phase5b integration: packet render differs from scalar"
    fi
else
    skip "phase5b integration: $CXX not found"
fi

echo "────────────────────────────────"
[ "$fail" -eq 0 ] && echo -e " packet test: ${GREEN}OK${RST}" || echo -e " packet test: ${RED}FAILED${RST}"
exit "$fail"
