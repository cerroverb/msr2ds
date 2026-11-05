/* One ARM instruction, for the two cases the recompiled code hands back (dispatch.cpp): an instruction rewritten in
 * memory while the game runs (tools/msr/recomp/patchable.txt: the frame limit, the interface's time steps, cheats),
 * and the few the generator left to an interpreter. The emulator-based build runs these on its JIT; this runtime has
 * none, so it covers what those patches use: data processing (immediate, or a register shifted by an immediate),
 * B/BL, VMOV.F32 between single registers, and word or byte loads and stores with an immediate offset (the amiibo
 * extras store their flag: strb r0, [r3]). Anything else is reported by the caller. */
#include "cpu.h"

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
    case 0xE: return 1;
    default: return -1;   /* 0xF: unconditional encodings, not handled here */
    }
}

static uint32_t reg(const CPU *c, uint32_t r, uint32_t pc) { return r == 15 ? pc + 8 : c->r[r]; }

/* a + b + carry, with the flags of an addition */
static uint32_t add_flags(uint32_t a, uint32_t b, uint32_t carry, uint32_t *c_out, uint32_t *v_out) {
    uint64_t w = (uint64_t)a + b + carry;
    uint32_t r = (uint32_t)w;
    *c_out = (uint32_t)(w >> 32);
    *v_out = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}

int rt_interpret_one(CPU *c) {
    if (c->thumb) return -1;
    uint32_t pc = c->r[15], w = rd32(pc);
    int pass = cond_passed(c, w >> 28);
    if (pass < 0) return -1;
    if (!pass) { c->r[15] = pc + 4; return 0; }

    if ((w & 0x0E000000u) == 0x0A000000u) {            /* B, BL */
        int32_t off = (int32_t)(w << 8) >> 6;
        if (w & 0x01000000u) c->r[14] = pc + 4;
        c->r[15] = pc + 8 + (uint32_t)off;
        return 0;
    }
    if ((w & 0x0FBF0FD0u) == 0x0EB00A40u) {            /* VMOV.F32 Sd, Sm */
        uint32_t d = ((w >> 12) & 15) << 1 | ((w >> 22) & 1), m = (w & 15) << 1 | ((w >> 5) & 1);
        c->s[d] = c->s[m];
        c->r[15] = pc + 4;
        return 0;
    }
    if ((w & 0x0E000000u) == 0x04000000u) {            /* LDR, STR, LDRB, STRB with an immediate offset */
        uint32_t p = (w >> 24) & 1, u = (w >> 23) & 1, b = (w >> 22) & 1, wb = (w >> 21) & 1, l = (w >> 20) & 1;
        uint32_t rn = (w >> 16) & 15, rd = (w >> 12) & 15, off = w & 0xFFF;
        if (rd == 15 || ((wb || !p) && rn == 15)) return -1;
        uint32_t base = reg(c, rn, pc), addr = u ? base + off : base - off, at = p ? addr : base;
        if (l) c->r[rd] = b ? rd8(at) : rd32(at);
        else if (b) wr8(at, c->r[rd]);
        else wr32(at, c->r[rd]);
        if ((wb || !p) && !(l && rn == rd)) c->r[rn] = addr;
        c->r[15] = pc + 4;
        return 0;
    }
    if ((w & 0x0C000000u) != 0) return -1;
    uint32_t imm = (w >> 25) & 1, op = (w >> 21) & 15, s = (w >> 20) & 1, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
    if (!imm && (w & 0x90u) == 0x90u) return -1;        /* multiplies, extra loads and stores */
    if (!imm && (w & 0x10u)) return -1;                 /* shift by a register */
    if (op >= 8 && op <= 11 && !s) return -1;           /* MRS, MSR, BX and friends */
    if (rd == 15 && s) return -1;                       /* exception return */

    uint32_t op2, shifter_c = c->cf;
    if (imm) {
        uint32_t rot = ((w >> 8) & 15) * 2;
        op2 = ror32(w & 0xFF, rot);
        if (rot) shifter_c = op2 >> 31;
    } else {
        uint32_t m = reg(c, w & 15, pc), type = (w >> 5) & 3, amount = (w >> 7) & 31;
        switch (type) {
        case 0: op2 = amount ? m << amount : m; if (amount) shifter_c = (m >> (32 - amount)) & 1; break;
        case 1: if (!amount) amount = 32; op2 = amount == 32 ? 0 : m >> amount; shifter_c = (m >> (amount - 1)) & 1; break;
        case 2: if (!amount) amount = 32; op2 = amount == 32 ? (uint32_t)((int32_t)m >> 31) : (uint32_t)((int32_t)m >> amount);
                shifter_c = (m >> (amount - 1)) & 1; break;
        default:
            if (amount) { op2 = ror32(m, amount); shifter_c = op2 >> 31; }
            else { op2 = (c->cf << 31) | (m >> 1); shifter_c = m & 1; }   /* RRX */
        }
    }
    uint32_t a = reg(c, rn, pc), r = 0, cf = c->cf, vf = c->v, logical = 0;
    switch (op) {
    case 0x0: case 0x8: r = a & op2; logical = 1; break;               /* AND, TST */
    case 0x1: case 0x9: r = a ^ op2; logical = 1; break;               /* EOR, TEQ */
    case 0x2: case 0xA: r = add_flags(a, ~op2, 1, &cf, &vf); break;    /* SUB, CMP */
    case 0x3: r = add_flags(op2, ~a, 1, &cf, &vf); break;              /* RSB */
    case 0x4: case 0xB: r = add_flags(a, op2, 0, &cf, &vf); break;     /* ADD, CMN */
    case 0x5: r = add_flags(a, op2, c->cf, &cf, &vf); break;           /* ADC */
    case 0x6: r = add_flags(a, ~op2, c->cf, &cf, &vf); break;          /* SBC */
    case 0x7: r = add_flags(op2, ~a, c->cf, &cf, &vf); break;          /* RSC */
    case 0xC: r = a | op2; logical = 1; break;                         /* ORR */
    case 0xD: r = op2; logical = 1; break;                             /* MOV */
    case 0xE: r = a & ~op2; logical = 1; break;                        /* BIC */
    default: r = ~op2; logical = 1; break;                             /* MVN */
    }
    if (s) {
        c->n = r >> 31; c->z = r == 0;
        if (logical) c->cf = shifter_c; else { c->cf = cf; c->v = vf; }
    }
    if (op >= 8 && op <= 11) { c->r[15] = pc + 4; return 0; }       /* tests write no register */
    if (rd == 15) { c->r[15] = r & ~3u; return 0; }
    c->r[rd] = r;
    c->r[15] = pc + 4;
    return 0;
}
