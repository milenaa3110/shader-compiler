#!/usr/bin/env bash
# bench_packet.sh — Route B Phase 5: measure the real SIMD speedup of the SPMD
# `fs_packet` path vs the scalar per-pixel `fs_invoke` path.
#
# WHY HOST (x86), NOT qemu/RVV: qemu-user emulates each RVV instruction with a
# scalar helper loop, so RVV wall-clock under qemu is meaningless (often slower).
# x86 has *real* SIMD (SSE/AVX) onto which the packet's <W x float> lowers
# directly, so a native host run gives a faithful speedup number. The same packet
# IR drives RVV on real RISC-V hardware.
#
# Method: emit a straight-line FS with both entry points (scalar fs_invoke +
# packetized fs_packet) into one module, retarget it to the host, and time both
# over the same fragments. Also asserts the two paths agree numerically.
#
# Needs clang++-18. Usage: ./test/script/bench_packet.sh
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
IRGEN="$ROOT/build/riscv/irgen_riscv"
CXX="${CLANGXX:-clang++-18}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

[ -x "$IRGEN" ] || { echo "irgen not found at $IRGEN"; exit 1; }
command -v "$CXX" >/dev/null 2>&1 || { echo "need $CXX for the host benchmark"; exit 1; }

cat > "$TMP/fs.src" <<'EOF'
uniform float uT;
in vec2 vUV;
out vec4 FragColor;
@entry @stage(fragment)
fn void main() {
    float x = vUV.x; float y = vUV.y;
    float a = x*x + y*y;
    float b = x*y*uT;
    float c = a*0.5 + b*0.25;
    float d = c*c + a*b - x*0.3;
    float e = d*0.7 + c*1.3 - b;
    float r = e*0.5 + 0.5;
    float g = (a + b + c)*0.2;
    float bl = (d - e)*0.1 + 0.5;
    FragColor = vec4(r, g, bl, 1.0);
}
EOF
SHADER_EMIT_PACKET=1 "$IRGEN" "$TMP/fs.ll" < "$TMP/fs.src" >/dev/null 2>&1
grep -q '@fs_packet' "$TMP/fs.ll" || { echo "fs_packet not emitted (shader bailed)"; exit 1; }
sed -E '/^target (triple|datalayout)/d; /^attributes #/d; s/ #0//g' "$TMP/fs.ll" > "$TMP/host.ll"

cat > "$TMP/bench.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
#include <chrono>
#include <vector>
#include <random>
extern "C" void fs_invoke(float*,float*,float*,double*);
extern "C" void fs_packet(const float*,const float*,float*,int*);
extern "C" float uT;
using clk=std::chrono::high_resolution_clock;
int main(){
    const int N=1<<12, REPS=4000; uT=1.7f;
    std::mt19937 rng(123); std::uniform_real_distribution<float> U(0,1);
    std::vector<float> ux(N), uy(N); for(int i=0;i<N;i++){ux[i]=U(rng);uy[i]=U(rng);}
    { float so[4],frag[4]={0,0,0,1},vs[2*4],fsa[4*4]={0},po[4*4]; int lv[4]; double od[1];
      for(int i=0;i<4;i++){vs[i]=ux[i];vs[4+i]=uy[i];for(int k=0;k<4;k++)fsa[k*4+i]=frag[k];}
      fs_packet(vs,fsa,po,lv); int bad=0;
      for(int i=0;i<4;i++){float vv[2]={ux[i],uy[i]};fs_invoke(frag,vv,so,od);
        for(int k=0;k<4;k++)if(std::fabs(so[k]-po[k*4+i])>1e-4f)bad++;}
      printf("equivalence: %s\n",bad?"MISMATCH":"OK"); if(bad)return 1; }
    volatile float sink=0;
    auto t0=clk::now();
    for(int r=0;r<REPS;r++){float frag[4]={0,0,0,1},out[4];double od[1];
      for(int i=0;i<N;i++){float vv[2]={ux[i],uy[i]};fs_invoke(frag,vv,out,od);sink+=out[0];}}
    double ms_s=std::chrono::duration<double,std::milli>(clk::now()-t0).count();
    auto t2=clk::now();
    for(int r=0;r<REPS;r++){float vs[2*4],fsa[4*4]={0},po[4*4];int lv[4];
      for(int i=0;i<4;i++)fsa[3*4+i]=1.0f;
      for(int i=0;i<N;i+=4){for(int l=0;l<4;l++){vs[l]=ux[i+l];vs[4+l]=uy[i+l];}
        fs_packet(vs,fsa,po,lv);sink+=po[0];}}
    double ms_p=std::chrono::duration<double,std::milli>(clk::now()-t2).count();
    printf("scalar : %8.1f ms\npacket : %8.1f ms\nspeedup: %.2fx (W=4 SIMD, sink=%.1f)\n",
           ms_s,ms_p,ms_s/ms_p,(float)sink);
    return 0;
}
EOF
"$CXX" -O3 -march=native "$TMP/host.ll" "$TMP/bench.cpp" -o "$TMP/bench" 2>/dev/null \
  || { echo "host build failed"; exit 1; }
"$TMP/bench"
