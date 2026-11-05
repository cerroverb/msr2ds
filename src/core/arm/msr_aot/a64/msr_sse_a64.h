// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The SSE intrinsics aot_runtime.h and the runtime use, for AArch64 (the Android build): this folder
// is first on the include path there, so <emmintrin.h> and <xmmintrin.h> are these, and
// aot_runtime.h stays as it is (its hash, which the native code is checked against, too).
//
// MXCSR is mapped onto FPCR: the rounding mode (MXCSR RC 00 nearest, 01 down, 10 up, 11 toward
// zero; FPCR RMode 00 nearest, 01 up, 10 down, 11 toward zero, as the guest's FPSCR) and
// flush-to-zero (MXCSR FTZ|DAZ; FPCR FZ flushes inputs and outputs, as the guest's FZ). FPCR's
// traps stay off (all exceptions masked, as MXCSR's 0x1F80) and its default-NaN bit stays clear:
// aot_runtime.h applies the guest's DN itself (NanF, NanD). AArch64's NaN propagation follows the
// ARM rules, as the 3DS's VFP does. Square roots and conversions are the AArch64 instructions,
// which honour FPCR as the SSE ones honour MXCSR.
#pragma once

#if !defined(__aarch64__)
#error "msr_sse_a64.h is for AArch64 builds only"
#endif

#include <stdint.h>

typedef float __m128 __attribute__((vector_size(16), aligned(16)));
typedef double __m128d __attribute__((vector_size(16), aligned(16)));

#define MSR_SSE_A64 static inline __attribute__((always_inline, unused))

MSR_SSE_A64 uint64_t msr_a64_fpcr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}

MSR_SSE_A64 void msr_a64_set_fpcr(uint64_t v) {
    __asm__ volatile("msr fpcr, %0" : : "r"(v) : "memory");
}

MSR_SSE_A64 unsigned int _mm_getcsr(void) {
    static const unsigned int rc[4] = {0x0000u, 0x4000u, 0x2000u, 0x6000u}; // RMode RN, RP, RM, RZ
    const uint64_t f = msr_a64_fpcr();
    unsigned int m = 0x1F80u | rc[(f >> 22) & 3];
    if (f & (1u << 24)) m |= 0x8040u;
    return m;
}

MSR_SSE_A64 void _mm_setcsr(unsigned int m) {
    static const uint64_t rmode[4] = {0, 2, 1, 3}; // RC nearest, down, up, toward zero
    uint64_t f = msr_a64_fpcr() & ~((3ull << 22) | (1ull << 24) | (1ull << 25) | 0x9F00ull);
    f |= rmode[(m >> 13) & 3] << 22;
    if (m & 0x8000u) f |= 1ull << 24;
    msr_a64_set_fpcr(f);
}

MSR_SSE_A64 __m128 _mm_set_ss(float x) {
    return (__m128){x, 0.f, 0.f, 0.f};
}

MSR_SSE_A64 float _mm_cvtss_f32(__m128 a) {
    return a[0];
}

MSR_SSE_A64 __m128 _mm_sqrt_ss(__m128 a) {
    float r;
    __asm__ volatile("fsqrt %s0, %s1" : "=w"(r) : "w"(a[0]));
    a[0] = r;
    return a;
}

MSR_SSE_A64 __m128d _mm_set_sd(double x) {
    return (__m128d){x, 0.0};
}

MSR_SSE_A64 double _mm_cvtsd_f64(__m128d a) {
    return a[0];
}

/// Lower lane: the square root of b's; upper lane: a's.
MSR_SSE_A64 __m128d _mm_sqrt_sd(__m128d a, __m128d b) {
    double r;
    __asm__ volatile("fsqrt %d0, %d1" : "=w"(r) : "w"(b[0]));
    a[0] = r;
    return a;
}

/// Toward zero (FCVTZS saturates where CVTTSD2SI gives 0x8000000000000000; the callers range-check).
MSR_SSE_A64 int64_t _mm_cvttsd_si64(__m128d a) {
    int64_t r;
    __asm__ volatile("fcvtzs %x0, %d1" : "=r"(r) : "w"(a[0]));
    return r;
}

/// With the current rounding mode: FRINTI rounds by FPCR, then the exact conversion.
MSR_SSE_A64 int64_t _mm_cvtsd_si64(__m128d a) {
    double t;
    int64_t r;
    __asm__ volatile("frinti %d0, %d1" : "=w"(t) : "w"(a[0]));
    __asm__ volatile("fcvtzs %x0, %d1" : "=r"(r) : "w"(t));
    return r;
}
