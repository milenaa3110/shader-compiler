#!/usr/bin/env bash
# run_matrix_numeric.sh — Executable numeric tests for matrix arithmetic.
#
# FileCheck pins the *shape* of the emitted IR and llvm-as proves it is
# well-formed, but neither computes anything: a matrix multiply that transposed
# its operands, or read a mat3 column at the wrong stride, would pass both. This
# links the generated IR against a C++ driver, runs it, and compares against
# hand-computed column-major results.
#
# Reuses the retarget trick from run_packet_test.sh: strip the RISC-V triple,
# datalayout and RVV function attributes so host clang can compile the same IR
# natively. Skipped (not failed) when clang is unavailable.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/../.."
IRGEN="${IRGEN:-$ROOT/build/riscv/irgen_riscv}"
CLANG="${CLANG:-clang-18}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

GREEN="\033[0;32m"; RED="\033[0;31m"; YEL="\033[0;33m"; CYAN="\033[0;36m"; RST="\033[0m"
fail=0
pass(){ echo -e "  ${GREEN}PASS${RST}  $*"; }
bad(){  echo -e "  ${RED}FAIL${RST}  $*"; fail=1; }
skip(){ echo -e "  ${YEL}SKIP${RST}  $*"; }

[ -x "$IRGEN" ] || { echo "irgen not found at $IRGEN"; exit 1; }
if ! command -v "$CLANG" >/dev/null 2>&1; then
    skip "matrix numeric tests ($CLANG not installed)"
    exit 0
fi

# Compile a shader, retarget its IR to the host, link it with a driver, run it.
run_numeric(){
    local src="$1" drv="$2" name="$3"
    if ! "$IRGEN" "$TMP/m.ll" < "$src" >/dev/null 2>&1; then
        bad "$name (shader failed to compile)"; return
    fi
    sed -E '/^target (triple|datalayout)/d; /^attributes #/d; s/ #0//g' "$TMP/m.ll" > "$TMP/host.ll"
    if ! "$CLANG" -O2 "$TMP/host.ll" "$drv" -lm -o "$TMP/t" 2>/dev/null; then
        bad "$name (link failed)"; return
    fi
    if "$TMP/t"; then pass "$name"; else bad "$name (wrong result)"; fi
}

echo -e "\n${CYAN}Evaluating matrix numeric equivalence...${RST}"

#  Multiply forms: mat4*vec4, mat3*mat3, vec*mat, mat*scalar, mat+mat
cat > "$TMP/mul.src" <<'EOF'
uniform mat4 MVP;
uniform mat3 A;
uniform mat3 B;
out vec4 vC;
out vec4 vD;
@entry @stage(vertex)
fn void main() {
    vec4 p = MVP * vec4(1.0, 2.0, 3.0, 1.0);
    mat3 C = A * B;
    vec3 r = vec3(1.0, 0.0, 0.0) * A;
    mat3 S = A * 2.0;
    mat3 D = A + A;
    vC = vec4(C[0][0], C[1][2], C[2][1], r.y);
    vD = vec4(S[1][1], D[2][0], -A[0][1], A[2][2]);
    gl_Position = p;
}
EOF
cat > "$TMP/mul.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
// mat4 is [4 x <4 x float>] — 4 columns of 4 floats, tightly packed.
extern "C" float MVP[16];
// mat3 is [3 x <3 x float>], and LLVM rounds <3 x float> up to 16 bytes, so the
// column stride is 4 floats with the 4th slot padding. A host that declares
// float[9] here reads the wrong elements from the second column on.
extern "C" float A[12];
extern "C" float B[12];

static int bad = 0;
static void eq(const char* what, float got, float want) {
    if (std::fabs(got - want) > 1e-4f) {
        std::printf("    MISMATCH %-10s got %.4f want %.4f\n", what, got, want);
        bad = 1;
    }
}
int main() {
    for (int i = 0; i < 16; ++i) MVP[i] = float(i + 1);   // columns (1..4)(5..8)(9..12)(13..16)
    const float acol[3][3] = {{1,2,3},{4,5,6},{7,8,9}};
    const float bcol[3][3] = {{1,2,3},{0,1,0},{0,0,2}};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) { A[c*4+r] = acol[c][r]; B[c*4+r] = bcol[c][r]; }

    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);

    // gl_Position = MVP * (1,2,3,1) = 1*col0 + 2*col1 + 3*col2 + 1*col3
    eq("pos.x", out[0], 51.f); eq("pos.y", out[1], 58.f);
    eq("pos.z", out[2], 65.f); eq("pos.w", out[3], 72.f);
    // Varyings are emitted in declaration order after gl_Position.
    // C = A*B column-major: C[0]=A*(1,2,3)=(30,36,42), C[1]=A*(0,1,0)=(4,5,6),
    //                       C[2]=A*(0,0,2)=(14,16,18)
    eq("C[0][0]", out[4], 30.f);
    eq("C[1][2]", out[5],  6.f);
    eq("C[2][1]", out[6], 16.f);
    // vec*mat is a per-column dot: (1,0,0)*A = (A[0][0], A[1][0], A[2][0]) = (1,4,7)
    eq("r.y",     out[7],  4.f);
    eq("S[1][1]", out[8], 10.f);   // A*2 → column1 = (8,10,12)
    eq("D[2][0]", out[9], 14.f);   // A+A → column2 = (14,16,18)
    eq("-A[0][1]", out[10], -2.f);
    eq("A[2][2]", out[11], 9.f);
    if (!bad) std::printf("");
    return bad;
}
EOF
run_numeric "$TMP/mul.src" "$TMP/mul.cpp" "mat4*vec4, mat3*mat3, vec*mat, mat*scalar, mat+mat, -mat"

#  Non-square shapes, where a transposed lowering still type-checks
cat > "$TMP/ns.src" <<'EOF'
uniform mat4x2 P;
uniform mat2x4 Q;
out vec4 vC;
@entry @stage(vertex)
fn void main() {
    mat2 R = P * Q;
    vec2 v = P * vec4(1.0, 1.0, 1.0, 1.0);
    vC = vec4(R[0][0], R[1][1], v.x, v.y);
    gl_Position = vec4(0.0);
}
EOF
cat > "$TMP/ns.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
// mat4x2 = 4 columns of <2 x float>; <2 x float> is 8 bytes, so stride 2.
extern "C" float P[8];
// mat2x4 = 2 columns of <4 x float>; stride 4.
extern "C" float Q[8];
static int bad = 0;
static void eq(const char* what, float got, float want) {
    if (std::fabs(got - want) > 1e-4f) {
        std::printf("    MISMATCH %-10s got %.4f want %.4f\n", what, got, want);
        bad = 1;
    }
}
int main() {
    // P columns: (1,2) (3,4) (5,6) (7,8)
    for (int i = 0; i < 8; ++i) P[i] = float(i + 1);
    // Q columns: (1,0,0,0) and (0,1,0,0)
    for (int i = 0; i < 8; ++i) Q[i] = 0.f;
    Q[0] = 1.f; Q[5] = 1.f;

    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    // R = P*Q is mat2 (Q's 2 columns × P's 2 rows).
    // R col0 = P * Q col0 = P * (1,0,0,0) = P col0 = (1,2)
    // R col1 = P * Q col1 = P * (0,1,0,0) = P col1 = (3,4)
    eq("R[0][0]", out[4], 1.f);
    eq("R[1][1]", out[5], 4.f);
    // P * (1,1,1,1) = col0+col1+col2+col3 = (1+3+5+7, 2+4+6+8) = (16, 20)
    eq("v.x", out[6], 16.f);
    eq("v.y", out[7], 20.f);
    return bad;
}
EOF
run_numeric "$TMP/ns.src" "$TMP/ns.cpp" "mat4x2*mat2x4 → mat2, mat4x2*vec4 → vec2 (non-square)"

#  Constructors: narrowing, widening, mixed component lists
cat > "$TMP/ctor.src" <<'EOF'
uniform mat4 M;
out vec4 vA;
out vec4 vB;
out vec4 vC;
@entry @stage(vertex)
fn void main() {
    mat3 n = mat3(M);                              # narrowing: copy 3x3 block
    mat4 w = mat4(mat2(9.0, 8.0, 7.0, 6.0));       # widening: identity fill
    mat2 x = mat2(vec2(1.0, 2.0), 3.0, 4.0);       # mixed vector + scalars
    vA = vec4(n[0][0], n[1][2], n[2][0], n[2][2]);
    vB = vec4(w[0][0], w[1][1], w[2][2], w[3][3]);
    vC = vec4(x[0][0], x[0][1], x[1][0], x[1][1]);
    gl_Position = vec4(0.0);
}
EOF
cat > "$TMP/ctor.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
extern "C" float M[16];
static int bad = 0;
static void eq(const char* what, float got, float want) {
    if (std::fabs(got - want) > 1e-4f) {
        std::printf("    MISMATCH %-10s got %.4f want %.4f\n", what, got, want);
        bad = 1;
    }
}
int main() {
    for (int i = 0; i < 16; ++i) M[i] = float(i + 1);  // columns (1..4)(5..8)(9..12)(13..16)
    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    // mat3(mat4) copies the top-left 3x3: columns (1,2,3)(5,6,7)(9,10,11)
    eq("n[0][0]", out[4],  1.f);
    eq("n[1][2]", out[5],  7.f);
    eq("n[2][0]", out[6],  9.f);
    eq("n[2][2]", out[7], 11.f);
    // mat4(mat2) copies the 2x2 block and fills the rest from the identity,
    // so the diagonal is (9, 6, 1, 1) — not (9, 6, 0, 0).
    eq("w[0][0]", out[8],  9.f);
    eq("w[1][1]", out[9],  6.f);
    eq("w[2][2]", out[10], 1.f);
    eq("w[3][3]", out[11], 1.f);
    // mat2(vec2(1,2), 3, 4) fills column-major: col0=(1,2), col1=(3,4)
    eq("x[0][0]", out[12], 1.f);
    eq("x[0][1]", out[13], 2.f);
    eq("x[1][0]", out[14], 3.f);
    eq("x[1][1]", out[15], 4.f);
    return bad;
}
EOF
run_numeric "$TMP/ctor.src" "$TMP/ctor.cpp" "mat3(mat4) narrowing, mat4(mat2) widening, mixed component list"

# Numerical correctness test for component-wise matrix operations (+, -, /, ++).
cat > "$TMP/cw.src" <<'EOF'
uniform mat3 A;
out vec4 vA;
out vec4 vB;
out vec4 vC;
@entry @stage(vertex)
fn void main() {
    mat3 add = A + 10.0;       # component-wise +scalar
    mat3 sub = 100.0 - A;      # scalar - mat (non-commutative)
    mat3 dvd = A / A;          # component-wise mat/mat = all ones
    mat3 sdv = 36.0 / A;       # scalar / mat (non-commutative)
    mat3 inc = A;
    inc++;                     # component-wise ++ : +1.0 each
    vA = vec4(add[0][0], add[2][2], sub[0][0], sub[1][1]);
    vB = vec4(dvd[0][0], dvd[2][1], sdv[0][0], sdv[1][1]);
    vC = vec4(inc[0][0], inc[2][2], A[0][0], A[2][2]);
    gl_Position = vec4(0.0);
}
EOF
cat > "$TMP/cw.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
extern "C" float A[12];
static int bad = 0;
static void eq(const char* what, float got, float want){ if(std::fabs(got-want)>1e-4f){std::printf("    MISMATCH %-8s got %.4f want %.4f\n",what,got,want);bad=1;} }
int main(){
    const float acol[3][3]={{1,2,3},{4,5,6},{7,8,9}};
    for(int c=0;c<3;++c)for(int r=0;r<3;++r)A[c*4+r]=acol[c][r];
    float out[64]={0};
    vs_invoke(0,0,nullptr,nullptr,out);
    eq("add00",out[4],11.f); eq("add22",out[5],19.f);   // A+10
    eq("sub00",out[6],99.f); eq("sub11",out[7],95.f);   // 100-A (order matters)
    eq("dvd00",out[8],1.f);  eq("dvd21",out[9],1.f);    // A/A = 1
    eq("sdv00",out[10],36.f);eq("sdv11",out[11],7.2f);  // 36/A (order matters)
    eq("inc00",out[12],2.f); eq("inc22",out[13],10.f);  // A+1
    eq("A00",out[14],1.f);   eq("A22",out[15],9.f);     // A unchanged
    return bad;
}
EOF
run_numeric "$TMP/cw.src" "$TMP/cw.cpp" "mat+scalar, scalar-mat, mat/mat, scalar/mat, mat++ (component-wise)"

#  Matrix builtins: transpose, determinant (2/3/4), inverse (verified via
#  A*inverse(A) == I), matrixCompMult, outerProduct (non-square result).
cat > "$TMP/bi.src" <<'EOF'
uniform mat3 A;
uniform mat4 M4;
uniform mat2 M2;
out vec4 vT;
out vec4 vD;
out vec4 vI3;
out vec4 vI4;
@entry @stage(vertex)
fn void main() {
    mat3 t = transpose(A);
    float d2 = determinant(M2);
    float d3 = determinant(A);
    float d4 = determinant(M4);
    mat3 i3 = A * inverse(A);
    mat4 i4 = M4 * inverse(M4);
    mat3 cm = matrixCompMult(A, A);
    mat2x3 op = outerProduct(vec3(1.0, 2.0, 3.0), vec2(10.0, 20.0));
    vT = vec4(t[0][1], t[1][0], t[2][0], t[0][2]);
    vD = vec4(d2, d3, d4, cm[1][1]);
    vI3 = vec4(i3[0][0], i3[1][1], i3[2][2], i3[0][1]);
    vI4 = vec4(i4[0][0], i4[1][1], i4[2][2], i4[3][3]);
    gl_Position = vec4(op[0][0], op[0][2], op[1][0], op[1][2]);
}
EOF
cat > "$TMP/bi.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
extern "C" float A[12], M4[16], M2[4];
static int bad = 0;
static void eq(const char* w, float g, float want) {
    if (std::fabs(g - want) > 1e-3f) {
        std::printf("    MISMATCH %-8s got %.4f want %.4f\n", w, g, want);
        bad = 1;
    }
}
int main() {
    // A = symmetric, invertible; columns col-major (stride 4, <3xf> padded to 16B)
    float ac[3][3] = {{2,1,0},{1,2,1},{0,1,2}};
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) A[c*4+r] = ac[c][r];
    M2[0]=4; M2[1]=2; M2[2]=1; M2[3]=3;               // det = 10
    for (int i = 0; i < 16; ++i) M4[i] = 0;
    M4[0]=2; M4[5]=3; M4[10]=4; M4[15]=5;             // det = 120
    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    // transpose: t[c][r] = A[r][c]
    eq("t01",out[4],1.f); eq("t10",out[5],1.f); eq("t20",out[6],0.f); eq("t02",out[7],0.f);
    eq("d2",out[8],10.f); eq("d3",out[9],4.f); eq("d4",out[10],120.f); eq("cm11",out[11],4.f);
    // A*inverse(A) == identity
    eq("i3_00",out[12],1.f); eq("i3_11",out[13],1.f); eq("i3_22",out[14],1.f); eq("i3_01",out[15],0.f);
    eq("i4_00",out[16],1.f); eq("i4_11",out[17],1.f); eq("i4_22",out[18],1.f); eq("i4_33",out[19],1.f);
    // outerProduct((1,2,3),(10,20)) -> mat2x3: col0=(10,20,30) col1=(20,40,60)
    eq("op00",out[0],10.f); eq("op02",out[1],30.f); eq("op10",out[2],20.f); eq("op12",out[3],60.f);
    return bad;
}
EOF
run_numeric "$TMP/bi.src" "$TMP/bi.cpp" "transpose, determinant(2/3/4), inverse (A*inv=I), matrixCompMult, outerProduct"

#  Mutation + interfacing: whole-matrix copy, column/element assignment (only
#  llvm-as-checked before), compound assignment, and a matrix-typed function
#  parameter + return through the RISC-V ABI.
cat > "$TMP/asg.src" <<'EOF'
uniform mat3 A;
out vec4 vCopy;
out vec4 vAssign;
out vec4 vCompound;
out vec4 vFunc;
fn mat3 dbl(mat3 x) {
    return x + x;
}
@entry @stage(vertex)
fn void main() {
    mat3 cp = A;
    mat3 m = A;
    m[0] = vec3(9.0, 8.0, 7.0);
    m[1][2] = 42.0;
    mat3 acc = A;
    acc += A;
    mat3 d = dbl(A);
    vCopy = vec4(cp[0][0], cp[2][2], A[1][1], 0.0);
    vAssign = vec4(m[0][0], m[0][2], m[1][2], m[2][2]);
    vCompound = vec4(acc[0][0], acc[1][1], acc[2][2], 0.0);
    vFunc = vec4(d[0][0], d[1][1], d[2][2], 0.0);
    gl_Position = vec4(0.0);
}
EOF
cat > "$TMP/asg.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
extern "C" float A[12];
static int bad = 0;
static void eq(const char* w, float g, float want) {
    if (std::fabs(g - want) > 1e-3f) {
        std::printf("    MISMATCH %-8s got %.4f want %.4f\n", w, g, want);
        bad = 1;
    }
}
int main() {
    float ac[3][3] = {{1,2,3},{4,5,6},{7,8,9}};
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) A[c*4+r] = ac[c][r];
    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    eq("cp00",out[4],1.f); eq("cp22",out[5],9.f); eq("A11",out[6],5.f);
    eq("m00",out[8],9.f); eq("m02",out[9],7.f); eq("m12",out[10],42.f); eq("m22",out[11],9.f);
    eq("acc00",out[12],2.f); eq("acc11",out[13],10.f); eq("acc22",out[14],18.f);   // A + A
    eq("d00",out[16],2.f); eq("d11",out[17],10.f); eq("d22",out[18],18.f);          // dbl(A)
    return bad;
}
EOF
run_numeric "$TMP/asg.src" "$TMP/asg.cpp" "copy, column/element assign, compound += , matrix fn param+return"

#  Matrix as a struct member: whole-matrix store/load, column and element access
#  through the member, member-matrix * vector, and element assignment.
cat > "$TMP/st.src" <<'EOF'
struct Mats { mat3 m; };
uniform mat3 A;
out vec4 vX;
out vec4 vY;
@entry @stage(vertex)
fn void main() {
    Mats s;
    s.m = A;
    mat3 got = s.m;
    vec3 col = s.m[1];
    float e = s.m[2][0];
    vec3 mv = s.m * vec3(1.0, 0.0, 0.0);
    s.m[0][0] = 99.0;
    vX = vec4(got[0][0], col.x, e, mv.y);
    vY = vec4(s.m[0][0], s.m[1][1], 0.0, 0.0);
    gl_Position = vec4(0.0);
}
EOF
cat > "$TMP/st.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
extern "C" float A[12];
static int bad = 0;
static void eq(const char* w, float g, float want) {
    if (std::fabs(g - want) > 1e-3f) {
        std::printf("    MISMATCH %-8s got %.4f want %.4f\n", w, g, want);
        bad = 1;
    }
}
int main() {
    float ac[3][3] = {{1,2,3},{4,5,6},{7,8,9}};
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) A[c*4+r] = ac[c][r];
    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    eq("got00",out[4],1.f); eq("colx",out[5],4.f); eq("e",out[6],7.f); eq("mvy",out[7],2.f);
    eq("m00",out[8],99.f); eq("m11",out[9],5.f);   // after s.m[0][0] = 99
    return bad;
}
EOF
run_numeric "$TMP/st.src" "$TMP/st.cpp" "matrix as struct member (store/load, column/element, s.m * v, element assign)"

#  Array of matrices: whole-matrix / column / element read and write across all
#  three index levels (arr[i], arr[i][j], arr[i][j][k]).
cat > "$TMP/arr.src" <<'EOF'
uniform mat3 A;
uniform mat3 B;
out vec4 vR;
out vec4 vW;
@entry @stage(vertex)
fn void main() {
    mat3 arr[2];
    arr[0] = A;
    arr[1] = B;
    mat3 whole = arr[1];
    vec3 col = arr[0][1];
    float e = arr[1][2][0];
    arr[0][1] = vec3(11.0, 22.0, 33.0);
    arr[1][2][0] = 99.0;
    vR = vec4(whole[0][0], col.x, e, 0.0);
    vW = vec4(arr[0][1][0], arr[0][1][2], arr[1][2][0], arr[1][0][0]);
    gl_Position = vec4(0.0);
}
EOF
cat > "$TMP/arr.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
extern "C" float A[12], B[12];
static int bad = 0;
static void eq(const char* w, float g, float want) {
    if (std::fabs(g - want) > 1e-3f) {
        std::printf("    MISMATCH %-8s got %.4f want %.4f\n", w, g, want);
        bad = 1;
    }
}
int main() {
    float ac[3][3] = {{1,2,3},{4,5,6},{7,8,9}};
    float bc[3][3] = {{10,20,30},{40,50,60},{70,80,90}};
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) { A[c*4+r]=ac[c][r]; B[c*4+r]=bc[c][r]; }
    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    // whole=arr[1]=B [0][0]=10 ; col=arr[0][1]=A col1=(4,5,6) col.x=4 ; e=arr[1][2][0]=B[2][0]=70
    eq("whole",out[4],10.f); eq("colx",out[5],4.f); eq("e",out[6],70.f);
    // after writes: arr[0][1]=(11,22,33) ; arr[1][2][0]=99 ; arr[1][0][0]=B[0][0]=10
    eq("w010",out[8],11.f); eq("w012",out[9],33.f); eq("w120",out[10],99.f); eq("w100",out[11],10.f);
    return bad;
}
EOF
run_numeric "$TMP/arr.src" "$TMP/arr.cpp" "array of matrices: arr[i] / arr[i][j] / arr[i][j][k] read + write"

#  Matrix equality in a bool context (mat==/!=mat and-reduction) and a direct
#  column index on a uniform matrix — both work on RISC-V; this locks it, and
#  matdemo.vert.spv locks the SPIR-V side (OpLogicalAnd + push-constant chain).
cat > "$TMP/cmp.src" <<'EOF'
uniform mat3 A;
uniform mat3 B;
out vec4 vEq;
@entry @stage(vertex)
fn void main() {
    float eq = 0.0;
    if (A == B) { eq = 1.0; }
    float ne = 0.0;
    if (A != B) { ne = 1.0; }
    vec3 col = A[1];
    vEq = vec4(eq, ne, col.x, col.z);
    gl_Position = vec4(0.0);
}
EOF
cat > "$TMP/cmp.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
extern "C" float A[12], B[12];
static int bad = 0;
static void eq(const char* w, float g, float want) {
    if (std::fabs(g - want) > 1e-3f) {
        std::printf("    MISMATCH %-8s got %.4f want %.4f\n", w, g, want);
        bad = 1;
    }
}
int main() {
    float ac[3][3] = {{1,2,3},{4,5,6},{7,8,9}};
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) { A[c*4+r]=ac[c][r]; B[c*4+r]=ac[c][r]; }
    B[5] = 99.0;  // make B differ from A in one element
    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    // A != B (one element differs): eq=0, ne=1 ; col=A[1]=(4,5,6) col.x=4 col.z=6
    eq("eq",out[4],0.f); eq("ne",out[5],1.f); eq("colx",out[6],4.f); eq("colz",out[7],6.f);
    return bad;
}
EOF
run_numeric "$TMP/cmp.src" "$TMP/cmp.cpp" "mat==/!=mat in bool context + uniform column index"

# ── case: mat-- postfix decrement (component-wise -1.0) ───────────────────────
cat > "$TMP/dec.src" <<'EOF'
uniform mat3 M;
out vec4 vDec;
@entry @stage(vertex)
fn void main() {
    mat3 m = M;
    m--;                       # component-wise -- : -1.0 each
    vDec = vec4(m[0][0], m[1][1], m[2][2], m[0][2]);
    gl_Position = vec4(0.0);
}
EOF
cat > "$TMP/dec.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
extern "C" float M[12];
static int bad = 0;
static void eq(const char* w, float g, float want) {
    if (std::fabs(g - want) > 1e-3f) {
        std::printf("    MISMATCH %-8s got %.4f want %.4f\n", w, g, want);
        bad = 1;
    }
}
int main() {
    float mc[3][3] = {{1,2,3},{4,5,6},{7,8,9}};   // columns 0,1,2
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) M[c*4+r] = mc[c][r];
    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    // each component -1: m[0][0]=0, m[1][1]=4, m[2][2]=8, m[0][2]=3-1=2
    eq("m00",out[4],0.f); eq("m11",out[5],4.f); eq("m22",out[6],8.f); eq("m02",out[7],2.f);
    return bad;
}
EOF
run_numeric "$TMP/dec.src" "$TMP/dec.cpp" "mat-- postfix decrement (component-wise)"

# ── case: dynamic column index on a uniform matrix (uMVP[i], runtime i) ───────
cat > "$TMP/dyn.src" <<'EOF'
uniform mat4 uMVP;
uniform float uIdx;
out vec4 vCol;
@entry @stage(vertex)
fn void main() {
    int i = int(uIdx);
    vCol = uMVP[i];            # dynamic (non-constant) uniform column index
    gl_Position = vec4(0.0);
}
EOF
cat > "$TMP/dyn.cpp" <<'EOF'
#include <cstdio>
#include <cmath>
extern "C" void vs_invoke(int, int, float*, double*, float*);
extern "C" float uMVP[16];   // mat4 = [4 x <4 x float>], tightly packed
extern "C" float uIdx;
static int bad = 0;
static void eq(const char* w, float g, float want) {
    if (std::fabs(g - want) > 1e-3f) {
        std::printf("    MISMATCH %-8s got %.4f want %.4f\n", w, g, want);
        bad = 1;
    }
}
int main() {
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) uMVP[c*4+r] = c*10.f + r;
    uIdx = 2.0f;               // select column 2 = (20,21,22,23)
    float out[64] = {0};
    vs_invoke(0, 0, nullptr, nullptr, out);
    eq("c2.x",out[4],20.f); eq("c2.y",out[5],21.f); eq("c2.z",out[6],22.f); eq("c2.w",out[7],23.f);
    return bad;
}
EOF
run_numeric "$TMP/dyn.src" "$TMP/dyn.cpp" "dynamic column index on uniform matrix (uMVP[i])"

echo ""
if [ "$fail" -eq 0 ]; then
    echo -e " Matrix Numeric Suite: ${GREEN}PASS${RST}"
else
    echo -e " Matrix Numeric Suite: ${RED}FAIL${RST}"
fi
echo ""
exit "$fail"
