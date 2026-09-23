// vec_math.cpp — branchless vector sin/cos, no libm calls.

#include "vec_math.h"

namespace {

typedef int vint __attribute__((ext_vector_type(SHADER_PACKET_WIDTH)));

constexpr float TWO_OVER_PI = 0.63661977236758134308f;

// pi/2 split so the first product is exact for small |n|.
constexpr float PIO2_HI = 1.57079625129699707031f;
constexpr float PIO2_MID = 7.54978941586159635335e-8f;
constexpr float PIO2_LO = 5.39030252995776476554e-15f;

// sin(r) = r + r^3 * (S1 + r^2 * (S2 + r^2 * S3))
constexpr float S1 = -1.6666654611e-1f;
constexpr float S2 = 8.3321608736e-3f;
constexpr float S3 = -1.9515295891e-4f;

// cos(r) = 1 - r^2/2 + r^4 * (C1 + r^2 * (C2 + r^2 * C3))
constexpr float C1 = 4.166664568298827e-2f;
constexpr float C2 = -1.388731625493765e-3f;
constexpr float C3 = 2.443315711809948e-5f;

// Exact element-wise select for a mask already narrowed to 1.0f / 0.0f.
// Written as multiply-add rather than a vector ternary so it depends only on
// arithmetic and __builtin_convertvector, and exactly rather than as
// b + m * (a - b), which would round the difference.
inline vfloat sel(vfloat m, vfloat a, vfloat b) {
    return a * m + b * (1.0f - m);
}

struct Reduced {
    vfloat r;  // remainder in [-pi/4, pi/4]
    vint q;    // quadrant 0..3
};

inline Reduced reduce(vfloat x) {
    vfloat f = x * TWO_OVER_PI;

    // Round to nearest without llvm.rint: the conversion below truncates, so
    // bias by +-0.5 first. A vector compare yields -1 for true, so converting it
    // to float and adding turns 0.5 into -0.5 exactly where x is negative.
    vfloat negative = __builtin_convertvector(f < 0.0f, vfloat);  // -1.0f or 0.0f
    vint n = __builtin_convertvector(f + (0.5f + negative), vint);
    vfloat fn = __builtin_convertvector(n, vfloat);

    // Cody-Waite: subtract n*pi/2 in decreasing pieces so the leading product
    // stays exact and only the small corrections carry rounding.
    vfloat r = x - fn * PIO2_HI;
    r = r - fn * PIO2_MID;
    r = r - fn * PIO2_LO;

    // Two's complement makes `& 3` the correct mod-4 for negative n as well:
    // n = -1 gives 3, which is the quadrant for x = r - pi/2.
    return { r, n & 3 };
}

inline vfloat sinPoly(vfloat r, vfloat r2) {
    return r + r * r2 * (S1 + r2 * (S2 + r2 * S3));
}

inline vfloat cosPoly(vfloat r2) {
    return 1.0f - 0.5f * r2 + r2 * r2 * (C1 + r2 * (C2 + r2 * C3));
}

}  // namespace

namespace {

typedef unsigned int vuint __attribute__((ext_vector_type(SHADER_PACKET_WIDTH)));

constexpr float LOG2E = 1.44269504088896341f;
// ln2 split the same way pi/2 was, so n * LN2_HI stays exact.
constexpr float LN2_HI = 0.693359375f;
constexpr float LN2_LO = -2.12194440e-4f;

// exp(r) on [-ln2/2, ln2/2], cephes single precision.
constexpr float E5 = 1.9875691500e-4f;
constexpr float E4 = 1.3981999507e-3f;
constexpr float E3 = 8.3334519073e-3f;
constexpr float E2 = 4.1665795894e-2f;
constexpr float E1 = 1.6666665459e-1f;
constexpr float E0 = 5.0000001201e-1f;

// log(1+r) on the frexp mantissa range, cephes single precision.
constexpr float SQRTHF = 0.707106781186547524f;
constexpr float L8 = 7.0376836292e-2f;
constexpr float L7 = -1.1514610310e-1f;
constexpr float L6 = 1.1676998740e-1f;
constexpr float L5 = -1.2420140846e-1f;
constexpr float L4 = 1.4249322787e-1f;
constexpr float L3 = -1.6668057665e-1f;
constexpr float L2 = 2.0000714765e-1f;
constexpr float L1 = -2.4999993993e-1f;
constexpr float L0 = 3.3333331174e-1f;

}  // namespace

// exp(x) = 2^n * exp(r), r = x - n*ln2. The 2^n factor is built straight from
// the exponent field rather than by another call.
static inline vfloat expImpl(vfloat x) {
    // Clamp the ARGUMENT, not the resulting exponent. Clamping n after the fact
    // leaves r = x - n*ln2 far outside [-ln2/2, ln2/2], and the polynomial then
    // returns nonsense — exp(-100) came out as -7.67e-35 before this was moved.
    // These bounds are where float exp under/overflows, so the clamp only ever
    // saturates a result that had nowhere to go, and keeps n within [-126, 127].
    x = (x < -87.3f ? (vfloat)(-87.3f) : x);
    x = (x > 88.0f ? (vfloat)88.0f : x);

    vfloat p = x * LOG2E;
    vfloat negative = __builtin_convertvector(p < 0.0f, vfloat);  // -1.0f or 0.0f
    vint n = __builtin_convertvector(p + (0.5f + negative), vint);
    vfloat fn = __builtin_convertvector(n, vfloat);

    vfloat r = x - fn * LN2_HI;
    r = r - fn * LN2_LO;
    vfloat r2 = r * r;
    vfloat poly = ((((E5 * r + E4) * r + E3) * r + E2) * r + E1) * r + E0;
    vfloat er = poly * r2 + r + 1.0f;

    vfloat pow2 = __builtin_bit_cast(vfloat, (vuint)(n + 127) << 23);
    return er * pow2;
}

// log(x) = e*ln2 + log(m), with x = m * 2^e and m in [0.5, 1) from the raw
// exponent field — frexp without the call.
static inline vfloat logImpl(vfloat x) {
    vuint ix = __builtin_bit_cast(vuint, x);
    vfloat e = __builtin_convertvector((vint)((ix >> 23) & 0xFFu) - 126, vfloat);
    vfloat m = __builtin_bit_cast(vfloat, (ix & 0x807FFFFFu) | 0x3F000000u);

    // Fold the mantissa into [sqrt(0.5), sqrt(2)) so the series converges fast.
    vfloat small = -__builtin_convertvector(m < SQRTHF, vfloat);  // 1.0f or 0.0f
    e = e - small;
    vfloat r = sel(small, m + m - 1.0f, m - 1.0f);

    vfloat r2 = r * r;
    vfloat poly = ((((((((L8 * r + L7) * r + L6) * r + L5) * r + L4) * r + L3) * r + L2) * r + L1) *
                       r + L0) * r * r2;
    vfloat y = poly + e * LN2_LO - 0.5f * r2;
    vfloat result = r + y + e * LN2_HI;

    // Shaders only ever take the log of a positive radius; returning 0 keeps a
    // degenerate lane from poisoning the packet with -inf or NaN.
    vfloat positive = -__builtin_convertvector(x > 0.0f, vfloat);  // 1.0f or 0.0f
    return result * positive;
}

// q: 0 -> sin(r), 1 -> cos(r), 2 -> -sin(r), 3 -> -cos(r)
static inline vfloat sinImpl(vfloat x) {
    const Reduced d = reduce(x);
    const vfloat r2 = d.r * d.r;
    const vfloat swap = __builtin_convertvector(d.q & 1, vfloat);         // 1 or 0
    const vfloat neg = __builtin_convertvector((d.q >> 1) & 1, vfloat);   // 1 or 0
    // Sign as a multiply keeps the result exact; a subtract would not.
    return sel(swap, cosPoly(r2), sinPoly(d.r, r2)) * (1.0f - 2.0f * neg);
}

// q: 0 -> cos(r), 1 -> -sin(r), 2 -> -cos(r), 3 -> sin(r)
static inline vfloat cosImpl(vfloat x) {
    const Reduced d = reduce(x);
    const vfloat r2 = d.r * d.r;
    const vfloat swap = __builtin_convertvector(d.q & 1, vfloat);
    const vfloat neg = __builtin_convertvector(((d.q + 1) >> 1) & 1, vfloat);
    return sel(swap, sinPoly(d.r, r2), cosPoly(r2)) * (1.0f - 2.0f * neg);
}

// Pointer wrappers: see the ABI note in vec_math.h for why the vector does
// not cross the call boundary by value.
extern "C" __attribute__((always_inline)) void __vexpf(const vfloat* x, vfloat* out) {
    *out = expImpl(*x);
}

extern "C" __attribute__((always_inline)) void __vlogf(const vfloat* x, vfloat* out) {
    *out = logImpl(*x);
}

extern "C" __attribute__((always_inline)) void __vsinf(const vfloat* x, vfloat* out) {
    *out = sinImpl(*x);
}

extern "C" __attribute__((always_inline)) void __vcosf(const vfloat* x, vfloat* out) {
    *out = cosImpl(*x);
}
