/* The fallback interpreter: ARMv6K (ARM and Thumb) with VFPv2, for code the recompiled module lacks. Static analysis
 * misses functions reached only through pointers computed at run time (a printf's character callback, for one); the
 * emulator-based build runs those on its JIT. Here dispatch.cpp interprets them from where the module has no entry
 * until the program counter reaches code it has (rt_interpret_until_known), and logs each such address once for the
 * next build of the module (build_module.py --extra).
 *
 * Covered: data processing (all shifter operands), multiplies (including long, DSP halfword and v6 media multiplies),
 * loads and stores (word, byte, halfword, signed, doubleword, exclusive, user-mode variants as ordinary ones), block
 * transfers, branches and interworking, MRS/MSR (flags and GE), CLZ, saturating arithmetic, v6 media (extends, REV,
 * SEL, PKH, SSAT/USAT, parallel add/subtract, USAD8), CP15's thread registers and barriers, and VFP (loads, stores,
 * transfers, arithmetic, compares, conversions) in single and double precision. Not covered (reported): exception
 * returns, mode changes, coprocessors other than 10/11/15, SETEND, BKPT.
 *
 * Floating point runs under the guest's MXCSR (dispatch.cpp's GuestMxcsr is in effect), as the recompiled code. */
#include "cpu.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { STEP_OK = 0, STEP_SVC = 1, STEP_BAD = -1 };

static uint32_t ror32(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

static int cond_passed(const CPU *c, uint32_t cond) {
    switch (cond) {
    case 0x0: return c->z;
    case 0x1: return !c->z;
    case 0x2: return c->cf;
    case 0x3: return !c->cf;
    case 0x4: return c->n;
    case 0x5: return !c->n;
    case 0x6: return c->v;
    case 0x7: return !c->v;
    case 0x8: return c->cf && !c->z;
    case 0x9: return !c->cf || c->z;
    case 0xA: return c->n == c->v;
    case 0xB: return c->n != c->v;
    case 0xC: return !c->z && c->n == c->v;
    case 0xD: return c->z || c->n != c->v;
    default: return 1;
    }
}

static uint32_t adc32(uint32_t a, uint32_t b, uint32_t carry, uint32_t *c_out, uint32_t *v_out) {
    uint64_t w = (uint64_t)a + b + carry;
    uint32_t r = (uint32_t)w;
    *c_out = (uint32_t)(w >> 32);
    *v_out = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
static void set_nz(CPU *c, uint32_t r) { c->n = r >> 31; c->z = r == 0; }

static uint32_t cpsr_of(const CPU *c) {
    return c->n << 31 | c->z << 30 | c->cf << 29 | c->v << 28 | c->q << 27 | (c->ge & 15) << 16 | c->thumb << 5 | c->cpsr_rest;
}

/* PC writes: interworking (BX, BLX, LDR/LDM/POP into the PC) and plain branches */
static void bx_write(CPU *c, uint32_t v) {
    c->thumb = v & 1;
    c->r[15] = c->thumb ? v & ~1u : v & ~3u;
}

static int32_t sat_signed(int64_t v, int bits, int *sat) {
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
    if (v > hi) { *sat = 1; return (int32_t)hi; }
    if (v < lo) { *sat = 1; return (int32_t)lo; }
    return (int32_t)v;
}
static uint32_t sat_unsigned(int64_t v, int bits, int *sat) {
    int64_t hi = bits >= 32 ? 0xFFFFFFFFll : ((int64_t)1 << bits) - 1;
    if (v > hi) { *sat = 1; return (uint32_t)hi; }
    if (v < 0) { *sat = 1; return 0; }
    return (uint32_t)v;
}

/* shift by an immediate (the encodings' amount 0 cases included); *carry gets the shifter carry */
static uint32_t shift_imm(uint32_t m, uint32_t type, uint32_t amount, uint32_t cin, uint32_t *carry) {
    switch (type) {
    case 0:
        if (!amount) { *carry = cin; return m; }
        *carry = (m >> (32 - amount)) & 1; return m << amount;
    case 1:
        if (!amount) amount = 32;
        *carry = (m >> (amount - 1)) & 1; return amount == 32 ? 0 : m >> amount;
    case 2:
        if (!amount) amount = 32;
        *carry = (m >> (amount - 1)) & 1;
        return amount == 32 ? (uint32_t)((int32_t)m >> 31) : (uint32_t)((int32_t)m >> amount);
    default:
        if (!amount) { *carry = m & 1; return (cin << 31) | (m >> 1); }   /* RRX */
        { uint32_t r = ror32(m, amount); *carry = r >> 31; return r; }
    }
}
/* shift by a register's bottom byte */
static uint32_t shift_reg(uint32_t m, uint32_t type, uint32_t amount, uint32_t cin, uint32_t *carry) {
    amount &= 0xFF;
    if (!amount) { *carry = cin; return m; }
    switch (type) {
    case 0:
        if (amount < 32) { *carry = (m >> (32 - amount)) & 1; return m << amount; }
        *carry = amount == 32 ? m & 1 : 0; return 0;
    case 1:
        if (amount < 32) { *carry = (m >> (amount - 1)) & 1; return m >> amount; }
        *carry = amount == 32 ? m >> 31 : 0; return 0;
    case 2:
        if (amount < 32) { *carry = (m >> (amount - 1)) & 1; return (uint32_t)((int32_t)m >> amount); }
        *carry = m >> 31; return (uint32_t)((int32_t)m >> 31);
    default:
        if (!(amount & 31)) { *carry = m >> 31; return m; }
        { uint32_t r = ror32(m, amount & 31); *carry = r >> 31; return r; }
    }
}

/* the sixteen data-processing operations; returns 0, or STEP_BAD for an exception return */
static int data_op(CPU *c, uint32_t op, uint32_t s, uint32_t rd, uint32_t a, uint32_t b, uint32_t shc) {
    uint32_t r = 0, cf = c->cf, vf = c->v, logical = 0;
    switch (op) {
    case 0x0: case 0x8: r = a & b; logical = 1; break;
    case 0x1: case 0x9: r = a ^ b; logical = 1; break;
    case 0x2: case 0xA: r = adc32(a, ~b, 1, &cf, &vf); break;
    case 0x3: r = adc32(b, ~a, 1, &cf, &vf); break;
    case 0x4: case 0xB: r = adc32(a, b, 0, &cf, &vf); break;
    case 0x5: r = adc32(a, b, c->cf, &cf, &vf); break;
    case 0x6: r = adc32(a, ~b, c->cf, &cf, &vf); break;
    case 0x7: r = adc32(b, ~a, c->cf, &cf, &vf); break;
    case 0xC: r = a | b; logical = 1; break;
    case 0xD: r = b; logical = 1; break;
    case 0xE: r = a & ~b; logical = 1; break;
    default: r = ~b; logical = 1; break;
    }
    if (rd == 15 && s && !(op >= 8 && op <= 11)) return STEP_BAD;   /* exception return */
    if (s) {
        set_nz(c, r);
        if (logical) c->cf = shc; else { c->cf = cf; c->v = vf; }
    }
    if (op >= 8 && op <= 11) return 0;
    if (rd == 15) { c->r[15] = c->thumb ? r & ~1u : r & ~3u; return 1; }   /* branch: no interworking on ARMv6 */
    c->r[rd] = r;
    return 0;
}

/* ---------------- VFP ---------------- */
static float  sf(const CPU *c, uint32_t i) { float f; memcpy(&f, &c->s[i], 4); return f; }
static void   sset(CPU *c, uint32_t i, float f) { memcpy(&c->s[i], &f, 4); }
static double df(const CPU *c, uint32_t i) { uint64_t u = (uint64_t)c->s[2 * i] | (uint64_t)c->s[2 * i + 1] << 32; double d; memcpy(&d, &u, 8); return d; }
static void   dset(CPU *c, uint32_t i, double d) { uint64_t u; memcpy(&u, &d, 8); c->s[2 * i] = (uint32_t)u; c->s[2 * i + 1] = (uint32_t)(u >> 32); }

static void vfp_compare(CPU *c, double a, double b) {
    uint32_t nzcv = (a != a || b != b) ? 0x3 : a == b ? 0x6 : a < b ? 0x8 : 0x2;
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | nzcv << 28;
}
static int64_t vfp_round(const CPU *c, double v, int toward_zero) {
    if (toward_zero) return (int64_t)trunc(v);
    switch ((c->fpscr >> 22) & 3) {
    case 1: return (int64_t)ceil(v);
    case 2: return (int64_t)floor(v);
    case 3: return (int64_t)trunc(v);
    default: return (int64_t)nearbyint(v);
    }
}

/* CDP on coprocessors 10/11 */
static int vfp_data(CPU *c, uint32_t w) {
    int dbl = (w >> 8) & 1;
    uint32_t p = (w >> 23) & 1, q = (w >> 21) & 1, r = (w >> 20) & 1, s = (w >> 6) & 1;
    uint32_t D = (w >> 22) & 1, N = (w >> 7) & 1, M = (w >> 5) & 1;
    uint32_t vd = (w >> 12) & 15, vn = (w >> 16) & 15, vm = w & 15;
    uint32_t sd = vd << 1 | D, sn = vn << 1 | N, sm = vm << 1 | M, dd = vd, dn = vn, dm = vm;   /* VFPv2: D0-D15 */
    uint32_t opc = p << 3 | q << 2 | r << 1 | s;
    if (opc != 0xF) {
        if (dbl) {
            double a = df(c, dn), b = df(c, dm), d = df(c, dd), x;
            switch (opc) {
            case 0x0: x = d + a * b; break;
            case 0x1: x = d - a * b; break;
            case 0x2: x = -d + a * b; break;
            case 0x3: x = -d - a * b; break;
            case 0x4: x = a * b; break;
            case 0x5: x = -(a * b); break;
            case 0x6: x = a + b; break;
            case 0x7: x = a - b; break;
            case 0x8: x = a / b; break;
            default: return STEP_BAD;
            }
            dset(c, dd, x);
        } else {
            float a = sf(c, sn), b = sf(c, sm), d = sf(c, sd), x;
            switch (opc) {
            case 0x0: x = d + a * b; break;
            case 0x1: x = d - a * b; break;
            case 0x2: x = -d + a * b; break;
            case 0x3: x = -d - a * b; break;
            case 0x4: x = a * b; break;
            case 0x5: x = -(a * b); break;
            case 0x6: x = a + b; break;
            case 0x7: x = a - b; break;
            case 0x8: x = a / b; break;
            default: return STEP_BAD;
            }
            sset(c, sd, x);
        }
        return 0;
    }
    uint32_t ext = vn << 1 | N;   /* the extension instructions */
    switch (ext) {
    case 0x00: if (dbl) dset(c, dd, df(c, dm)); else c->s[sd] = c->s[sm]; return 0;                  /* VMOV */
    case 0x01: if (dbl) dset(c, dd, fabs(df(c, dm))); else c->s[sd] = c->s[sm] & 0x7FFFFFFFu; return 0;   /* VABS */
    case 0x02: if (dbl) { dset(c, dd, df(c, dm)); c->s[2 * dd + 1] ^= 0x80000000u; } else c->s[sd] = c->s[sm] ^ 0x80000000u; return 0;   /* VNEG */
    case 0x03: if (dbl) dset(c, dd, sqrt(df(c, dm))); else sset(c, sd, sqrtf(sf(c, sm))); return 0;    /* VSQRT */
    case 0x08: case 0x09: vfp_compare(c, dbl ? df(c, dd) : sf(c, sd), dbl ? df(c, dm) : sf(c, sm)); return 0;   /* VCMP(E) */
    case 0x0A: case 0x0B: vfp_compare(c, dbl ? df(c, dd) : sf(c, sd), 0.0); return 0;                   /* VCMP(E) #0 */
    case 0x0F: if (dbl) sset(c, sd, (float)df(c, dm)); else dset(c, dd, (double)sf(c, sm)); return 0;  /* VCVT between precisions */
    case 0x10: if (dbl) dset(c, dd, (double)c->s[sm]); else sset(c, sd, (float)c->s[sm]); return 0;   /* VCVT from unsigned */
    case 0x11: if (dbl) dset(c, dd, (double)(int32_t)c->s[sm]); else sset(c, sd, (float)(int32_t)c->s[sm]); return 0;   /* from signed */
    case 0x18: case 0x19: case 0x1A: case 0x1B: {   /* VCVT(R) to unsigned / signed (R: per FPSCR; else toward zero) */
        double v = dbl ? df(c, dm) : sf(c, sm);
        int64_t i = v != v ? 0 : vfp_round(c, v, ext & 1);
        if (ext < 0x1A) c->s[sd] = i < 0 ? 0 : i > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)i;
        else c->s[sd] = (uint32_t)(i < INT32_MIN ? INT32_MIN : i > INT32_MAX ? INT32_MAX : (int32_t)i);
        return 0;
    }
    default: return STEP_BAD;
    }
}

/* VLDR/VSTR, VLDM/VSTM (VPUSH/VPOP), and VMOV between two core registers and a double or two singles */
static int vfp_transfer(CPU *c, uint32_t w, uint32_t base) {   /* base: Rn as the instruction reads it */
    int dbl = (w >> 8) & 1;
    uint32_t P = (w >> 24) & 1, U = (w >> 23) & 1, D = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
    uint32_t rn = (w >> 16) & 15, vd = (w >> 12) & 15, imm8 = w & 0xFF;
    if ((w & 0x0FE00000u) == 0x0C400000u) {   /* VMOV two core registers */
        uint32_t rt = (w >> 12) & 15, rt2 = (w >> 16) & 15, M = (w >> 5) & 1, vm = w & 15;
        if (rt == 15 || rt2 == 15) return STEP_BAD;
        uint32_t lo = dbl ? 2 * vm : (vm << 1 | M);
        if (lo + 1 >= 32) return STEP_BAD;
        if (L) { c->r[rt] = c->s[lo]; c->r[rt2] = c->s[lo + 1]; }
        else { c->s[lo] = c->r[rt]; c->s[lo + 1] = c->r[rt2]; }
        return 0;
    }
    if (P && !W) {   /* VLDR / VSTR */
        if (rn == 15) base &= ~3u;
        uint32_t a = U ? base + imm8 * 4 : base - imm8 * 4;
        uint32_t first = dbl ? 2 * vd : (vd << 1 | D), n = dbl ? 2 : 1;
        for (uint32_t i = 0; i < n; i++) { if (L) c->s[first + i] = rd32(a + 4 * i); else wr32(a + 4 * i, c->s[first + i]); }
        return 0;
    }
    if (P == U) return STEP_BAD;   /* (P=1, U=1) and (P=0, U=0) are not VLDM/VSTM */
    uint32_t first = dbl ? 2 * vd : (vd << 1 | D), words = dbl ? imm8 & ~1u : imm8;
    if (first + words > 32) return STEP_BAD;
    uint32_t a = U ? base : base - imm8 * 4;
    for (uint32_t i = 0; i < words; i++) { if (L) c->s[first + i] = rd32(a + 4 * i); else wr32(a + 4 * i, c->s[first + i]); }
    if (W) c->r[rn] = U ? base + imm8 * 4 : base - imm8 * 4;
    return 0;
}

/* MRC/MCR: VFP's single transfers and system registers, CP15's thread registers and barriers */
static int coproc_transfer(CPU *c, uint32_t w) {
    uint32_t cp = (w >> 8) & 15, L = (w >> 20) & 1, rt = (w >> 12) & 15, opc1 = (w >> 21) & 7;
    uint32_t crn = (w >> 16) & 15, crm = w & 15, opc2 = (w >> 5) & 7;
    if (cp == 15) {
        if (L && crn == 13 && opc1 == 0 && crm == 0 && (opc2 == 2 || opc2 == 3)) {
            if (rt == 15) return STEP_BAD;
            c->r[rt] = opc2 == 3 ? c->tls : c->tls_rw; return 0;
        }
        if (!L && crn == 13 && opc1 == 0 && crm == 0 && opc2 == 2) { c->tls_rw = c->r[rt]; return 0; }
        if (!L && crn == 7) return 0;   /* barriers, cache maintenance */
        return STEP_BAD;
    }
    if (cp == 10 && opc1 == 0) {   /* VMOV between a core register and a single */
        uint32_t sn = ((w >> 16) & 15) << 1 | ((w >> 7) & 1);
        if (rt == 15) return STEP_BAD;
        if (L) c->r[rt] = c->s[sn]; else c->s[sn] = c->r[rt];
        return 0;
    }
    if (cp == 10 && opc1 == 7) {   /* VMRS / VMSR */
        if (L) {
            uint32_t v = crn == 1 ? c->fpscr : crn == 8 ? 0x40000000u : 0;   /* FPSCR; FPEXC enabled; FPSID 0 */
            if (rt == 15) { c->n = v >> 31; c->z = (v >> 30) & 1; c->cf = (v >> 29) & 1; c->v = (v >> 28) & 1; }
            else c->r[rt] = v;
        } else if (crn == 1) c->fpscr = c->r[rt];
        return 0;
    }
    if (cp == 11 && (opc1 == 0 || opc1 == 1)) {   /* VMOV between a core register and half of a double */
        uint32_t dn = (w >> 16) & 15, half = opc1 & 1;
        if (rt == 15) return STEP_BAD;
        if (L) c->r[rt] = c->s[2 * dn + half]; else c->s[2 * dn + half] = c->r[rt];
        return 0;
    }
    return STEP_BAD;
}

/* ---------------- v6 media ---------------- */
static uint32_t extend_op(uint32_t op, uint32_t m, uint32_t rot, uint32_t n, int acc) {
    m = ror32(m, rot);
    uint32_t r;
    switch (op) {
    case 0: {   /* SXT(A)B16 */
        uint32_t lo = (uint32_t)(int32_t)(int8_t)m, hi = (uint32_t)(int32_t)(int8_t)(m >> 16);
        if (acc) { lo += n; hi += n >> 16; }
        return (lo & 0xFFFF) | hi << 16;
    }
    case 2: r = (uint32_t)(int32_t)(int8_t)m; break;    /* SXT(A)B */
    case 3: r = (uint32_t)(int32_t)(int16_t)m; break;   /* SXT(A)H */
    case 4: {   /* UXT(A)B16 */
        uint32_t lo = m & 0xFF, hi = (m >> 16) & 0xFF;
        if (acc) { lo += n; hi += n >> 16; }
        return (lo & 0xFFFF) | hi << 16;
    }
    case 6: r = m & 0xFF; break;     /* UXT(A)B */
    case 7: r = m & 0xFFFF; break;   /* UXT(A)H */
    default: return 0;
    }
    return acc ? r + n : r;
}

/* parallel add/subtract: op1 01 signed, 10 saturating signed, 11 halving signed, 101 unsigned, 110 saturating
   unsigned, 111 halving unsigned; op2 000 ADD16, 001 ASX, 010 SAX, 011 SUB16, 100 ADD8, 111 SUB8 */
static int parallel(CPU *c, uint32_t op1, uint32_t op2, uint32_t a, uint32_t b, uint32_t *out) {
    int sgn = !(op1 & 4), kind = op1 & 3;   /* 1 plain (sets GE), 2 saturating, 3 halving */
    if (!kind) return STEP_BAD;
    uint32_t r = 0, ge = 0;
    if (op2 == 4 || op2 == 7) {   /* 8-bit lanes */
        for (int i = 0; i < 4; i++) {
            int32_t x = sgn ? (int8_t)(a >> (8 * i)) : (int32_t)((a >> (8 * i)) & 0xFF);
            int32_t y = sgn ? (int8_t)(b >> (8 * i)) : (int32_t)((b >> (8 * i)) & 0xFF);
            int32_t s = op2 == 4 ? x + y : x - y, sat = 0;
            if (kind == 2) s = sgn ? sat_signed(s, 8, &sat) : (int32_t)sat_unsigned(s, 8, &sat);
            else if (kind == 3) s >>= 1;
            else if (sgn ? s >= 0 : (op2 == 4 ? s >= 0x100 : s >= 0)) ge |= 1u << i;
            r |= ((uint32_t)s & 0xFF) << (8 * i);
        }
    } else if (op2 <= 3) {   /* 16-bit lanes: ADD16, ASX, SAX, SUB16 */
        int32_t a0 = sgn ? (int16_t)a : (int32_t)(a & 0xFFFF), a1 = sgn ? (int16_t)(a >> 16) : (int32_t)(a >> 16);
        int32_t b0 = sgn ? (int16_t)b : (int32_t)(b & 0xFFFF), b1 = sgn ? (int16_t)(b >> 16) : (int32_t)(b >> 16);
        int32_t lo, hi; int add_lo, add_hi;
        switch (op2) {
        case 0: lo = a0 + b0; hi = a1 + b1; add_lo = add_hi = 1; break;
        case 1: lo = a0 - b1; hi = a1 + b0; add_lo = 0; add_hi = 1; break;
        case 2: lo = a0 + b1; hi = a1 - b0; add_lo = 1; add_hi = 0; break;
        default: lo = a0 - b0; hi = a1 - b1; add_lo = add_hi = 0; break;
        }
        int sat = 0;
        if (kind == 2) {
            lo = sgn ? sat_signed(lo, 16, &sat) : (int32_t)sat_unsigned(lo, 16, &sat);
            hi = sgn ? sat_signed(hi, 16, &sat) : (int32_t)sat_unsigned(hi, 16, &sat);
        } else if (kind == 3) { lo >>= 1; hi >>= 1; }
        else {
            if (sgn ? lo >= 0 : (add_lo ? lo >= 0x10000 : lo >= 0)) ge |= 3;
            if (sgn ? hi >= 0 : (add_hi ? hi >= 0x10000 : hi >= 0)) ge |= 12;
        }
        r = ((uint32_t)lo & 0xFFFF) | ((uint32_t)hi & 0xFFFF) << 16;
    } else return STEP_BAD;
    if (kind == 1) c->ge = ge;
    *out = r;
    return 0;
}

/* ---------------- ARM ---------------- */

static int arm_step(CPU *c) {
    uint32_t pc = c->r[15], w = rd32(pc), cond = w >> 28;
    c->r[15] = pc + 4;   /* the next instruction unless a branch changes it; reads of r15 use pc + 8 below */
#define RD(x) ((x) == 15 ? pc + 8 : c->r[x])
#define BAD() do { c->r[15] = pc; return STEP_BAD; } while (0)
    if (cond == 0xF) {
        if ((w & 0x0E000000u) == 0x0A000000u) {   /* BLX immediate */
            int32_t off = (int32_t)(w << 8) >> 6;
            c->r[14] = pc + 4;
            c->thumb = 1;
            c->r[15] = pc + 8 + (uint32_t)off + ((w >> 23) & 2);
            return 0;
        }
        if ((w & 0x0D70F000u) == 0x0550F000u) return 0;   /* PLD */
        if (w == 0xF57FF01Fu) { c->excl_valid = 0; return 0; }   /* CLREX */
        if ((w & 0xFFFFFF00u) == 0xF57FF000u) return 0;   /* DSB, DMB, ISB */
        BAD();
    }
    if (!cond_passed(c, cond)) return 0;
    uint32_t op1 = (w >> 25) & 7;
    switch (op1) {
    case 0: case 1: {
        if (op1 == 0 && (w & 0x90u) == 0x90u) {   /* multiplies, extra loads and stores, swaps, exclusives */
            uint32_t rn = (w >> 16) & 15, rd = (w >> 12) & 15, rs = (w >> 8) & 15, rm = w & 15;
            if ((w & 0x0FC000F0u) == 0x00000090u) {   /* MUL, MLA: Rd 19-16, Rn 15-12 */
                uint32_t r = c->r[rm] * c->r[rs] + (((w >> 21) & 1) ? c->r[rd] : 0);
                c->r[rn] = r;
                if ((w >> 20) & 1) set_nz(c, r);
                return 0;
            }
            if ((w & 0x0FF000F0u) == 0x00400090u) {   /* UMAAL */
                uint64_t r = (uint64_t)c->r[rm] * c->r[rs] + c->r[rd] + c->r[rn];
                c->r[rd] = (uint32_t)r; c->r[rn] = (uint32_t)(r >> 32);
                return 0;
            }
            if ((w & 0x0F8000F0u) == 0x00800090u) {   /* UMULL, UMLAL, SMULL, SMLAL: RdHi 19-16, RdLo 15-12 */
                int sgn = (w >> 22) & 1, acc = (w >> 21) & 1;
                uint64_t r = sgn ? (uint64_t)((int64_t)(int32_t)c->r[rm] * (int32_t)c->r[rs]) : (uint64_t)c->r[rm] * c->r[rs];
                if (acc) r += (uint64_t)c->r[rn] << 32 | c->r[rd];
                c->r[rd] = (uint32_t)r; c->r[rn] = (uint32_t)(r >> 32);
                if ((w >> 20) & 1) { c->n = (uint32_t)(r >> 63); c->z = r == 0; }
                return 0;
            }
            if ((w & 0x0FB00FF0u) == 0x01000090u) {   /* SWP, SWPB */
                uint32_t a = c->r[rn], v;
                if ((w >> 22) & 1) { v = rd8(a); wr8(a, c->r[rm]); } else { v = rd32(a); wr32(a, c->r[rm]); }
                c->r[rd] = v;
                return 0;
            }
            if ((w & 0x0F8000F0u) == 0x01800090u) {   /* LDREX(B/H/D), STREX(B/H/D) */
                uint32_t a = c->r[rn], size = (w >> 21) & 3;   /* 0 word, 1 doubleword, 2 byte, 3 halfword */
                if ((w >> 20) & 1) {
                    uint64_t v = size == 0 ? rd32(a) : size == 2 ? rd8(a) : size == 3 ? rd16(a) : ((uint64_t)rd32(a + 4) << 32 | rd32(a));
                    c->excl_valid = 1; c->excl_addr = a; c->excl_value = v;
                    c->r[rd] = (uint32_t)v;
                    if (size == 1) c->r[rd + 1] = (uint32_t)(v >> 32);
                } else {
                    int ok = c->excl_valid && c->excl_addr == a;
                    if (ok) {
                        if (size == 0) { uint32_t e = (uint32_t)c->excl_value; ok = __atomic_compare_exchange_n((uint32_t *)(g_mem + a), &e, c->r[rm], 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
                        else if (size == 1) { uint64_t e = c->excl_value, v = (uint64_t)c->r[rm + 1] << 32 | c->r[rm];
                                              ok = __atomic_compare_exchange_n((uint64_t *)(g_mem + a), &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
                        else if (size == 2) { uint8_t e = (uint8_t)c->excl_value; ok = __atomic_compare_exchange_n(g_mem + a, &e, (uint8_t)c->r[rm], 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
                        else { uint16_t e = (uint16_t)c->excl_value; ok = __atomic_compare_exchange_n((uint16_t *)(g_mem + a), &e, (uint16_t)c->r[rm], 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
                    }
                    c->excl_valid = 0;
                    c->r[rd] = ok ? 0 : 1;
                }
                return 0;
            }
            /* extra loads and stores: halfwords, signed bytes, doublewords */
            uint32_t P = (w >> 24) & 1, U = (w >> 23) & 1, I = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1, sh = (w >> 5) & 3;
            if (!sh) BAD();
            uint32_t off = I ? ((w >> 4) & 0xF0) | (w & 0xF) : c->r[rm];
            uint32_t base = RD(rn), addr = U ? base + off : base - off, at = P ? addr : base;
            if (L) {
                uint32_t v;
                if (sh == 1) v = rd16(at);
                else if (sh == 2) v = (uint32_t)(int32_t)(int8_t)rd8(at);
                else v = (uint32_t)(int32_t)(int16_t)rd16(at);
                if (!P || W) c->r[rn] = addr;
                c->r[rd] = v;
            } else if (sh == 1) {
                wr16(at, RD(rd));
                if (!P || W) c->r[rn] = addr;
            } else {   /* LDRD (sh 2), STRD (sh 3) */
                if (rd & 1) BAD();
                if (sh == 2) { uint32_t lo = rd32(at), hi = rd32(at + 4); if (!P || W) c->r[rn] = addr; c->r[rd] = lo; c->r[rd + 1] = hi; }
                else { wr32(at, c->r[rd]); wr32(at + 4, c->r[rd + 1]); if (!P || W) c->r[rn] = addr; }
            }
            return 0;
        }
        if ((w & 0x01900000u) == 0x01000000u) {   /* the miscellaneous space (TST/TEQ/CMP/CMN without S) */
            uint32_t rd = (w >> 12) & 15, rm = w & 15, rn = (w >> 16) & 15, rs = (w >> 8) & 15;
            if (op1 == 1) {   /* MSR immediate (op 10x10), MOVW/MOVT (not on ARMv6) */
                if ((w & 0x0FB00000u) != 0x03200000u) BAD();
                uint32_t v = ror32(w & 0xFF, ((w >> 8) & 15) * 2), mask = (w >> 16) & 15;
                if ((w >> 22) & 1) BAD();
                if (mask & 8) { c->n = v >> 31; c->z = (v >> 30) & 1; c->cf = (v >> 29) & 1; c->v = (v >> 28) & 1; c->q = (v >> 27) & 1; }
                if (mask & 4) c->ge = (v >> 16) & 15;
                return 0;
            }
            if ((w & 0x0FBF0FFFu) == 0x010F0000u) { if ((w >> 22) & 1) BAD(); c->r[rd] = cpsr_of(c); return 0; }   /* MRS CPSR */
            if ((w & 0x0FB0FFF0u) == 0x0120F000u) {   /* MSR register */
                uint32_t v = c->r[rm], mask = (w >> 16) & 15;
                if ((w >> 22) & 1) BAD();
                if (mask & 8) { c->n = v >> 31; c->z = (v >> 30) & 1; c->cf = (v >> 29) & 1; c->v = (v >> 28) & 1; c->q = (v >> 27) & 1; }
                if (mask & 4) c->ge = (v >> 16) & 15;
                return 0;
            }
            if ((w & 0x0FFFFFF0u) == 0x012FFF10u) { bx_write(c, c->r[rm]); return 0; }                                   /* BX */
            if ((w & 0x0FFFFFF0u) == 0x012FFF30u) { uint32_t t = c->r[rm]; c->r[14] = pc + 4; bx_write(c, t); return 0; }  /* BLX */
            if ((w & 0x0FFF0FF0u) == 0x016F0F10u) { uint32_t v = c->r[rm]; c->r[rd] = v ? (uint32_t)__builtin_clz(v) : 32; return 0; }   /* CLZ */
            if ((w & 0x0F900FF0u) == 0x01000050u) {   /* QADD, QSUB, QDADD, QDSUB */
                uint32_t op = (w >> 21) & 3; int sat = 0;
                int64_t b = (int32_t)c->r[rn];
                if (op & 2) b = sat_signed(2 * b, 32, &sat);
                int64_t r = (op & 1) ? (int64_t)(int32_t)c->r[rm] - b : (int64_t)(int32_t)c->r[rm] + b;
                c->r[rd] = (uint32_t)sat_signed(r, 32, &sat);
                if (sat) c->q = 1;
                return 0;
            }
            if ((w & 0x0F900090u) == 0x01000080u) {   /* SMLAxy, SMLAWy, SMULWy, SMLALxy, SMULxy: Rd 19-16, Rn 15-12 */
                uint32_t op = (w >> 21) & 3, x = (w >> 5) & 1, y = (w >> 6) & 1;
                int32_t a = (int16_t)(x ? c->r[rm] >> 16 : c->r[rm]), b = (int16_t)(y ? c->r[rs] >> 16 : c->r[rs]);
                if (op == 0) { int64_t r = (int64_t)a * b + (int32_t)c->r[rd]; c->r[rn] = (uint32_t)r; if (r != (int32_t)r) c->q = 1; }
                else if (op == 1) {
                    int64_t r = ((int64_t)(int32_t)c->r[rm] * b) >> 16;
                    if (!x) { int64_t s = r + (int32_t)c->r[rd]; c->r[rn] = (uint32_t)s; if (s != (int32_t)s) c->q = 1; }
                    else c->r[rn] = (uint32_t)r;
                } else if (op == 2) { int64_t r = (int64_t)((uint64_t)c->r[rn] << 32 | c->r[rd]) + (int64_t)a * b; c->r[rd] = (uint32_t)r; c->r[rn] = (uint32_t)((uint64_t)r >> 32); }
                else c->r[rn] = (uint32_t)(a * b);
                return 0;
            }
            BAD();
        }
        /* data processing */
        uint32_t op = (w >> 21) & 15, s = (w >> 20) & 1, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
        uint32_t b, shc = c->cf, a;
        if (op1 == 1) {
            uint32_t rot = ((w >> 8) & 15) * 2;
            b = ror32(w & 0xFF, rot);
            if (rot) shc = b >> 31;
            a = RD(rn);
        } else if (w & 0x10) {   /* register shifted by a register: the PC reads 12 ahead */
            uint32_t rm = w & 15, rs = (w >> 8) & 15;
            uint32_t m = rm == 15 ? pc + 12 : c->r[rm];
            b = shift_reg(m, (w >> 5) & 3, c->r[rs], c->cf, &shc);
            a = rn == 15 ? pc + 12 : c->r[rn];
        } else {
            b = shift_imm(RD(w & 15), (w >> 5) & 3, (w >> 7) & 31, c->cf, &shc);
            a = RD(rn);
        }
        int r = data_op(c, op, s, rd, a, b, shc);
        if (r < 0) BAD();
        return 0;
    }
    case 2: case 3: {
        if (op1 == 3 && (w & 0x10)) {   /* v6 media */
            uint32_t rn = (w >> 16) & 15, rd = (w >> 12) & 15, rs = (w >> 8) & 15, rm = w & 15;
            uint32_t op2 = (w >> 5) & 7, top = (w >> 20) & 0x1F;
            if ((top & 0x18) == 0x00) {   /* 0110 0: parallel add/subtract */
                uint32_t r;
                if (parallel(c, (w >> 20) & 7, op2, c->r[rn], c->r[rm], &r)) BAD();
                c->r[rd] = r;
                return 0;
            }
            if ((w & 0x0F8000F0u) == 0x06800070u && !(w & 0x300)) {   /* SXT/UXT(A)B/H/B16 */
                uint32_t eop = (w >> 20) & 7;
                if (eop == 1 || eop == 5) BAD();
                c->r[rd] = extend_op(eop, c->r[rm], ((w >> 10) & 3) * 8, rn == 15 ? 0 : c->r[rn], rn != 15);
                return 0;
            }
            if ((w & 0x0FFF0FF0u) == 0x06BF0F30u) { c->r[rd] = __builtin_bswap32(c->r[rm]); return 0; }   /* REV */
            if ((w & 0x0FFF0FF0u) == 0x06BF0FB0u) { uint32_t v = c->r[rm]; c->r[rd] = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u); return 0; }   /* REV16 */
            if ((w & 0x0FFF0FF0u) == 0x06FF0FB0u) { uint32_t v = c->r[rm]; c->r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); return 0; }   /* REVSH */
            if ((w & 0x0FF00FF0u) == 0x06800FB0u) {   /* SEL */
                uint32_t r = 0;
                for (int i = 0; i < 4; i++) r |= (((c->ge >> i) & 1) ? c->r[rn] : c->r[rm]) & (0xFFu << (8 * i));
                c->r[rd] = r;
                return 0;
            }
            if ((w & 0x0FF00030u) == 0x06800010u) {   /* PKHBT, PKHTB */
                uint32_t amount = (w >> 7) & 31, tb = (w >> 6) & 1, m = c->r[rm];
                if (tb) { m = amount ? (uint32_t)((int32_t)m >> amount) : (uint32_t)((int32_t)m >> 31); c->r[rd] = (c->r[rn] & 0xFFFF0000u) | (m & 0xFFFF); }
                else { m <<= amount; c->r[rd] = (c->r[rn] & 0xFFFF) | (m & 0xFFFF0000u); }
                return 0;
            }
            if ((w & 0x0FA00030u) == 0x06A00010u) {   /* SSAT, USAT */
                uint32_t sat_imm = (w >> 16) & 31, amount = (w >> 7) & 31, ty = (w >> 6) & 1, cy; int sat = 0;
                int32_t v = (int32_t)shift_imm(c->r[rm], ty ? 2 : 0, amount, 0, &cy);
                c->r[rd] = (w >> 22) & 1 ? sat_unsigned(v, (int)sat_imm, &sat) : (uint32_t)sat_signed(v, (int)sat_imm + 1, &sat);
                if (sat) c->q = 1;
                return 0;
            }
            if ((w & 0x0FB00FF0u) == 0x06A00F30u) {   /* SSAT16, USAT16 */
                uint32_t sat_imm = (w >> 16) & 15, m = c->r[rm]; int sat = 0, u = (w >> 22) & 1;
                int32_t lo = (int16_t)m, hi = (int16_t)(m >> 16);
                uint32_t a = u ? sat_unsigned(lo, (int)sat_imm, &sat) : (uint32_t)sat_signed(lo, (int)sat_imm + 1, &sat);
                uint32_t b = u ? sat_unsigned(hi, (int)sat_imm, &sat) : (uint32_t)sat_signed(hi, (int)sat_imm + 1, &sat);
                c->r[rd] = (a & 0xFFFF) | b << 16;
                if (sat) c->q = 1;
                return 0;
            }
            if ((w & 0x0F800010u) == 0x07000010u) {   /* SMLAD/SMUAD/SMLSD/SMUSD, SMLALD/SMLSLD, SMMLA/SMMUL/SMMLS: Rd 19-16, Ra 15-12 */
                uint32_t op = (w >> 20) & 7, m = c->r[rs], x = (w >> 5) & 1, sub = (w >> 6) & 1;
                if (x && op != 5) m = ror32(m, 16);
                int64_t p1 = (int64_t)(int16_t)c->r[rm] * (int16_t)m, p2 = (int64_t)(int16_t)(c->r[rm] >> 16) * (int16_t)(m >> 16);
                if (op == 0) {
                    int64_t r = sub ? p1 - p2 : p1 + p2;
                    if (rd != 15) r += (int32_t)c->r[rd];
                    c->r[rn] = (uint32_t)r;
                    if (r != (int32_t)r) c->q = 1;
                    return 0;
                }
                if (op == 4) { int64_t r = (int64_t)((uint64_t)c->r[rn] << 32 | c->r[rd]) + (sub ? p1 - p2 : p1 + p2); c->r[rd] = (uint32_t)r; c->r[rn] = (uint32_t)((uint64_t)r >> 32); return 0; }
                if (op == 5) {
                    int64_t r = (int64_t)(int32_t)c->r[rm] * (int32_t)c->r[rs];
                    int round = (w >> 5) & 1, msub = (w >> 6) & 1;
                    if (rd != 15) r = msub ? ((int64_t)(int32_t)c->r[rd] << 32) - r : ((int64_t)(int32_t)c->r[rd] << 32) + r;
                    if (round) r += 0x80000000ll;
                    c->r[rn] = (uint32_t)((uint64_t)r >> 32);
                    return 0;
                }
                BAD();
            }
            if ((w & 0x0FF000F0u) == 0x07800010u) {   /* USAD8, USADA8: Rd 19-16, Ra 15-12 */
                uint32_t a = c->r[rm], b = c->r[rs], sum = 0;
                for (int i = 0; i < 4; i++) { int d = (int)((a >> (8 * i)) & 0xFF) - (int)((b >> (8 * i)) & 0xFF); sum += (uint32_t)(d < 0 ? -d : d); }
                c->r[rn] = sum + (rd == 15 ? 0 : c->r[rd]);
                return 0;
            }
            BAD();
        }
        /* LDR, STR, LDRB, STRB (and the T variants as ordinary accesses) */
        uint32_t P = (w >> 24) & 1, U = (w >> 23) & 1, B = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
        uint32_t rn = (w >> 16) & 15, rd = (w >> 12) & 15, cy;
        uint32_t off = op1 == 2 ? w & 0xFFF : shift_imm(c->r[w & 15], (w >> 5) & 3, (w >> 7) & 31, c->cf, &cy);
        uint32_t base = RD(rn), addr = U ? base + off : base - off, at = P ? addr : base;
        if (L) {
            uint32_t v = B ? rd8(at) : rd32(at);
            if (!P || W) c->r[rn] = addr;
            if (rd == 15) bx_write(c, v); else c->r[rd] = v;
        } else {
            uint32_t v = RD(rd);
            if (B) wr8(at, v); else wr32(at, v);
            if (!P || W) c->r[rn] = addr;
        }
        return 0;
    }
    case 4: {   /* LDM, STM */
        uint32_t P = (w >> 24) & 1, U = (w >> 23) & 1, S = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
        uint32_t rn = (w >> 16) & 15, list = w & 0xFFFF, n = (uint32_t)__builtin_popcount(list);
        if (S || !list) BAD();   /* user registers, exception returns */
        uint32_t base = c->r[rn], start = U ? base + (P ? 4 : 0) : base - 4 * n + (P ? 0 : 4), a = start;
        uint32_t wb = U ? base + 4 * n : base - 4 * n;
        if (L) {
            uint32_t pcv = 0; int load_pc = 0;
            for (int i = 0; i < 16; i++) if (list >> i & 1) {
                uint32_t v = rd32(a); a += 4;
                if (i == 15) { pcv = v; load_pc = 1; } else c->r[i] = v;
            }
            if (W && !(list >> rn & 1)) c->r[rn] = wb;
            if (load_pc) bx_write(c, pcv);
        } else {
            for (int i = 0; i < 16; i++) if (list >> i & 1) { wr32(a, i == 15 ? pc + 8 : c->r[i]); a += 4; }
            if (W) c->r[rn] = wb;
        }
        return 0;
    }
    case 5: {   /* B, BL */
        int32_t off = (int32_t)(w << 8) >> 6;
        if ((w >> 24) & 1) c->r[14] = pc + 4;
        c->r[15] = pc + 8 + (uint32_t)off;
        return 0;
    }
    case 6: {   /* coprocessor loads and stores: VFP */
        uint32_t cp = (w >> 8) & 15;
        if (cp != 10 && cp != 11) BAD();
        if (vfp_transfer(c, w, RD((w >> 16) & 15))) BAD();
        return 0;
    }
    default: {   /* 7: SVC, CDP, MRC/MCR */
        if ((w >> 24) & 1) { c->exit = EXIT_SVC; c->svc = w & 0xFFFFFF; return STEP_SVC; }
        uint32_t cp = (w >> 8) & 15;
        if (!(w & 0x10)) { if ((cp != 10 && cp != 11) || vfp_data(c, w)) BAD(); return 0; }
        if (coproc_transfer(c, w)) BAD();
        return 0;
    }
    }
#undef RD
#undef BAD
}

/* ---------------- Thumb ---------------- */
static int thumb_step(CPU *c) {
    uint32_t pc = c->r[15], h = rd16(pc);
    c->r[15] = pc + 2;
#define TR(x) ((x) == 15 ? pc + 4 : c->r[x])
#define BAD() do { c->r[15] = pc; return STEP_BAD; } while (0)
    uint32_t top = h >> 11;
    if (top <= 2) {   /* LSL, LSR, ASR by an immediate */
        uint32_t rd = h & 7, rm = (h >> 3) & 7, amount = (h >> 6) & 31, cy = c->cf;
        uint32_t r = shift_imm(c->r[rm], top, amount, c->cf, &cy);
        c->r[rd] = r; set_nz(c, r); c->cf = cy;
        return 0;
    }
    if (top == 3) {   /* ADD, SUB: register or 3-bit immediate */
        uint32_t rd = h & 7, rn = (h >> 3) & 7, x = (h >> 6) & 7, b = (h >> 10) & 1 ? x : c->r[x], cf, vf;
        uint32_t r = (h >> 9) & 1 ? adc32(c->r[rn], ~b, 1, &cf, &vf) : adc32(c->r[rn], b, 0, &cf, &vf);
        c->r[rd] = r; set_nz(c, r); c->cf = cf; c->v = vf;
        return 0;
    }
    if (top <= 7) {   /* MOV, CMP, ADD, SUB with an 8-bit immediate */
        uint32_t rd = (h >> 8) & 7, imm = h & 0xFF, cf, vf, r;
        switch (top) {
        case 4: r = imm; c->r[rd] = r; set_nz(c, r); return 0;
        case 5: r = adc32(c->r[rd], ~imm, 1, &cf, &vf); set_nz(c, r); c->cf = cf; c->v = vf; return 0;
        case 6: r = adc32(c->r[rd], imm, 0, &cf, &vf); c->r[rd] = r; set_nz(c, r); c->cf = cf; c->v = vf; return 0;
        default: r = adc32(c->r[rd], ~imm, 1, &cf, &vf); c->r[rd] = r; set_nz(c, r); c->cf = cf; c->v = vf; return 0;
        }
    }
    if ((h & 0xFC00) == 0x4000) {   /* data processing */
        uint32_t op = (h >> 6) & 15, rd = h & 7, rm = (h >> 3) & 7, a = c->r[rd], b = c->r[rm], r, cy = c->cf, cf, vf;
        switch (op) {
        case 0x0: r = a & b; c->r[rd] = r; set_nz(c, r); return 0;
        case 0x1: r = a ^ b; c->r[rd] = r; set_nz(c, r); return 0;
        case 0x2: r = shift_reg(a, 0, b, c->cf, &cy); c->r[rd] = r; set_nz(c, r); c->cf = cy; return 0;
        case 0x3: r = shift_reg(a, 1, b, c->cf, &cy); c->r[rd] = r; set_nz(c, r); c->cf = cy; return 0;
        case 0x4: r = shift_reg(a, 2, b, c->cf, &cy); c->r[rd] = r; set_nz(c, r); c->cf = cy; return 0;
        case 0x5: r = adc32(a, b, c->cf, &cf, &vf); c->r[rd] = r; set_nz(c, r); c->cf = cf; c->v = vf; return 0;
        case 0x6: r = adc32(a, ~b, c->cf, &cf, &vf); c->r[rd] = r; set_nz(c, r); c->cf = cf; c->v = vf; return 0;
        case 0x7: r = shift_reg(a, 3, b, c->cf, &cy); c->r[rd] = r; set_nz(c, r); c->cf = cy; return 0;
        case 0x8: set_nz(c, a & b); return 0;
        case 0x9: r = adc32(0, ~b, 1, &cf, &vf); c->r[rd] = r; set_nz(c, r); c->cf = cf; c->v = vf; return 0;   /* NEG */
        case 0xA: r = adc32(a, ~b, 1, &cf, &vf); set_nz(c, r); c->cf = cf; c->v = vf; return 0;
        case 0xB: r = adc32(a, b, 0, &cf, &vf); set_nz(c, r); c->cf = cf; c->v = vf; return 0;
        case 0xC: r = a | b; c->r[rd] = r; set_nz(c, r); return 0;
        case 0xD: r = a * b; c->r[rd] = r; set_nz(c, r); return 0;
        case 0xE: r = a & ~b; c->r[rd] = r; set_nz(c, r); return 0;
        default: r = ~b; c->r[rd] = r; set_nz(c, r); return 0;
        }
    }
    if ((h & 0xFC00) == 0x4400) {   /* high registers: ADD, CMP, MOV, BX/BLX */
        uint32_t op = (h >> 8) & 3, rd = (h & 7) | ((h >> 4) & 8), rm = (h >> 3) & 15, cf, vf;
        switch (op) {
        case 0: { uint32_t r = TR(rd) + TR(rm); if (rd == 15) c->r[15] = r & ~1u; else c->r[rd] = r; return 0; }
        case 1: { uint32_t r = adc32(TR(rd), ~TR(rm), 1, &cf, &vf); set_nz(c, r); c->cf = cf; c->v = vf; return 0; }
        case 2: { uint32_t r = TR(rm); if (rd == 15) c->r[15] = r & ~1u; else c->r[rd] = r; return 0; }
        default: {
            uint32_t t = TR(rm);
            if ((h >> 7) & 1) c->r[14] = (pc + 2) | 1;
            bx_write(c, t);
            return 0;
        }
        }
    }
    if (top == 9) { c->r[(h >> 8) & 7] = rd32(((pc + 4) & ~3u) + (h & 0xFF) * 4); return 0; }   /* LDR literal */
    if ((h & 0xF000) == 0x5000) {   /* loads and stores with a register offset */
        uint32_t rd = h & 7, a = c->r[(h >> 3) & 7] + c->r[(h >> 6) & 7];
        switch ((h >> 9) & 7) {
        case 0: wr32(a, c->r[rd]); return 0;
        case 1: wr16(a, c->r[rd]); return 0;
        case 2: wr8(a, c->r[rd]); return 0;
        case 3: c->r[rd] = (uint32_t)(int32_t)(int8_t)rd8(a); return 0;
        case 4: c->r[rd] = rd32(a); return 0;
        case 5: c->r[rd] = rd16(a); return 0;
        case 6: c->r[rd] = rd8(a); return 0;
        default: c->r[rd] = (uint32_t)(int32_t)(int16_t)rd16(a); return 0;
        }
    }
    if ((h & 0xE000) == 0x6000) {   /* word and byte with a 5-bit immediate */
        uint32_t rd = h & 7, rn = (h >> 3) & 7, imm = (h >> 6) & 31, byte = (h >> 12) & 1, load = (h >> 11) & 1;
        uint32_t a = c->r[rn] + (byte ? imm : imm * 4);
        if (load) c->r[rd] = byte ? rd8(a) : rd32(a); else if (byte) wr8(a, c->r[rd]); else wr32(a, c->r[rd]);
        return 0;
    }
    if ((h & 0xF000) == 0x8000) {   /* halfword with a 5-bit immediate */
        uint32_t rd = h & 7, a = c->r[(h >> 3) & 7] + ((h >> 6) & 31) * 2;
        if ((h >> 11) & 1) c->r[rd] = rd16(a); else wr16(a, c->r[rd]);
        return 0;
    }
    if ((h & 0xF000) == 0x9000) {   /* SP-relative */
        uint32_t rd = (h >> 8) & 7, a = c->r[13] + (h & 0xFF) * 4;
        if ((h >> 11) & 1) c->r[rd] = rd32(a); else wr32(a, c->r[rd]);
        return 0;
    }
    if ((h & 0xF000) == 0xA000) {   /* ADR, ADD Rd, SP, #imm */
        uint32_t rd = (h >> 8) & 7, imm = (h & 0xFF) * 4;
        c->r[rd] = ((h >> 11) & 1) ? c->r[13] + imm : ((pc + 4) & ~3u) + imm;
        return 0;
    }
    if ((h & 0xF000) == 0xB000) {   /* miscellaneous */
        if ((h & 0xFF00) == 0xB000) { uint32_t imm = (h & 0x7F) * 4; c->r[13] = (h & 0x80) ? c->r[13] - imm : c->r[13] + imm; return 0; }
        if ((h & 0xFF00) == 0xB200) {   /* SXTH, SXTB, UXTH, UXTB */
            uint32_t rd = h & 7, m = c->r[(h >> 3) & 7];
            switch ((h >> 6) & 3) {
            case 0: c->r[rd] = (uint32_t)(int32_t)(int16_t)m; break;
            case 1: c->r[rd] = (uint32_t)(int32_t)(int8_t)m; break;
            case 2: c->r[rd] = m & 0xFFFF; break;
            default: c->r[rd] = m & 0xFF; break;
            }
            return 0;
        }
        if ((h & 0xFE00) == 0xB400) {   /* PUSH */
            uint32_t list = (h & 0xFF) | ((h >> 8) & 1) << 14, n = (uint32_t)__builtin_popcount(list), a = c->r[13] - 4 * n;
            c->r[13] = a;
            for (int i = 0; i < 15; i++) if (list >> i & 1) { wr32(a, c->r[i]); a += 4; }
            return 0;
        }
        if ((h & 0xFE00) == 0xBC00) {   /* POP */
            uint32_t list = (h & 0xFF) | ((h >> 8) & 1) << 15, a = c->r[13], pcv = 0;
            for (int i = 0; i < 16; i++) if (list >> i & 1) { uint32_t v = rd32(a); a += 4; if (i == 15) pcv = v; else c->r[i] = v; }
            c->r[13] = a;
            if (list >> 15 & 1) bx_write(c, pcv);
            return 0;
        }
        if ((h & 0xFFC0) == 0xBA00) { c->r[h & 7] = __builtin_bswap32(c->r[(h >> 3) & 7]); return 0; }   /* REV */
        if ((h & 0xFFC0) == 0xBA40) { uint32_t v = c->r[(h >> 3) & 7]; c->r[h & 7] = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u); return 0; }   /* REV16 */
        if ((h & 0xFFC0) == 0xBAC0) { uint32_t v = c->r[(h >> 3) & 7]; c->r[h & 7] = (uint32_t)(int32_t)(int16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); return 0; }   /* REVSH */
        BAD();
    }
    if ((h & 0xF000) == 0xC000) {   /* STMIA, LDMIA */
        uint32_t rn = (h >> 8) & 7, list = h & 0xFF, a = c->r[rn];
        if (!list) BAD();
        if ((h >> 11) & 1) {
            for (int i = 0; i < 8; i++) if (list >> i & 1) { c->r[i] = rd32(a); a += 4; }
            if (!(list >> rn & 1)) c->r[rn] = a;
        } else {
            for (int i = 0; i < 8; i++) if (list >> i & 1) { wr32(a, c->r[i]); a += 4; }
            c->r[rn] = a;
        }
        return 0;
    }
    if ((h & 0xF000) == 0xD000) {   /* conditional branch, SVC */
        uint32_t cond = (h >> 8) & 15;
        if (cond == 15) { c->exit = EXIT_SVC; c->svc = h & 0xFF; return STEP_SVC; }
        if (cond == 14) BAD();
        if (cond_passed(c, cond)) c->r[15] = pc + 4 + (uint32_t)((int32_t)(int8_t)(h & 0xFF) * 2);
        return 0;
    }
    if (top == 0x1C) { c->r[15] = pc + 4 + (uint32_t)(((int32_t)(h << 21) >> 20)); return 0; }   /* B */
    if (top == 0x1E) {   /* BL / BLX: the prefix and its suffix together */
        uint32_t h2 = rd16(pc + 2);
        uint32_t hi = pc + 4 + (uint32_t)(((int32_t)(h << 21)) >> 9);
        if ((h2 >> 11) == 0x1F) { c->r[14] = (pc + 4) | 1; c->r[15] = hi + (h2 & 0x7FF) * 2; return 0; }
        if ((h2 >> 11) == 0x1D) { c->r[14] = (pc + 4) | 1; c->thumb = 0; c->r[15] = (hi + (h2 & 0x7FF) * 2) & ~3u; return 0; }
        BAD();
    }
    BAD();
#undef TR
#undef BAD
}

int rt_interpret_step(CPU *c) { return c->thumb ? thumb_step(c) : arm_step(c); }
