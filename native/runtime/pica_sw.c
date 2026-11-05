/* Software PICA200 pipeline: vertex fetch -> vertex shader interpreter -> clip -> rasterize -> texture -> TEV ->
 * alpha/depth test -> blend -> tiled framebuffer in guest memory. Reference implementation; a Vulkan renderer can
 * replace it later. Register numbers are PICA200 register indices (word offsets).
 * Fragment lighting follows the DMP model (per-light LUT lookups, normal from the interpolated quaternion, optional
 * normal map), fog (mode 5) and stencil (D24S8). Not yet: shadows, gas, procedural texture, geometry shader, logic ops. */
#include "pica_int.h"
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <pthread.h>

#include "state.h"
#define R(i) pica_regs[i]

/* ---------------- shader state ---------------- */
uint32_t pica_vs_prog[4096 + 128], pica_vs_version;   /* program words, then the 128 swizzle descriptors */
#define prog pica_vs_prog
#define swz (pica_vs_prog + 4096)
static uint32_t prog_ptr, swz_ptr;
V4 pica_vs_cf[96];                      /* float uniforms */
#define cf pica_vs_cf
static uint32_t uni_idx, uni_buf[4], uni_n;
static V4 fixed_attr[16];
static uint32_t fixed_idx, fixed_buf[3], fixed_n;
/* immediate mode (0x232 = 0xF): vertices written through the attribute port, drawn as one non-indexed batch when the GPU
   goes back to configuration mode (0x245 bit 0) or the command list ends (pica_sw_imm_flush) */
#define IMM_MAX 4096
static V4 imm_v[IMM_MAX][12];
static uint32_t imm_n, imm_attr;
static int imm_active;   /* pica_fetch_attrs reads imm_v while the batch is drawn */
void pica_sw_draw(int indexed);
void pica_sw_imm_flush(void) {
    if (!imm_n) return;
    imm_active = 1; pica_sw_draw(0); imm_active = 0;
    imm_n = 0;
}
uint32_t light_lut[24][256], light_lut_version, fog_lut[128];
static uint32_t lut_idx, lut_type, fog_idx;

float f24(uint32_t v) {
    v &= 0xffffff;
    if (!(v & 0x7fffff)) return (v & 0x800000) ? -0.0f : 0.0f;
    uint32_t sign = v >> 23, exp = (v >> 16) & 0x7f, mant = v & 0xffff, bits;
    if (exp == 0x7f) bits = sign << 31 | 0xffu << 23 | mant << 7;
    else bits = sign << 31 | (exp + 64) << 23 | mant << 7;
    float f; memcpy(&f, &bits, 4); return f;
}
static float f32u(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static V4 unpack_f24x4(const uint32_t w[3]) {
    V4 r; r.w = f24(w[0] >> 8); r.z = f24((w[0] & 0xff) << 16 | w[1] >> 16); r.y = f24((w[1] & 0xffff) << 8 | w[2] >> 24); r.x = f24(w[2] & 0xffffff);
    return r;
}

/* register side effects of the shader/attribute upload ports */
void pica_sw_write(uint32_t id, uint32_t v) {
    switch (id) {
    case 0x2C0: uni_idx = v & 0xff; uni_n = 0; break;
    case 0x2C1: case 0x2C2: case 0x2C3: case 0x2C4: case 0x2C5: case 0x2C6: case 0x2C7: case 0x2C8:
        uni_buf[uni_n++] = v;
        static int sample_dbg = -1; if (sample_dbg < 0) sample_dbg = getenv("PWR_SW_SAMPLE") != NULL;
        if (sample_dbg) { static int nn; uint32_t ex = (v >> 23) & 0xff;
            if (nn < 6 && (R(0x2C0) >> 31) && ex == 0xff) { nn++; fprintf(stderr, "sw: NaN uniform word %08x idx=%u n=%u mode=%08x reg=%03x\n", v, uni_idx, uni_n, R(0x2C0), id); } }
        if (R(0x2C0) >> 31) {   /* f32 mode: w, z, y, x */
            if (uni_n == 4) { if (uni_idx < 96) cf[uni_idx] = (V4){ f32u(uni_buf[3]), f32u(uni_buf[2]), f32u(uni_buf[1]), f32u(uni_buf[0]) }; uni_idx++; uni_n = 0; }
        } else if (uni_n == 3) { if (uni_idx < 96) cf[uni_idx] = unpack_f24x4(uni_buf); uni_idx++; uni_n = 0; }
        break;
    case 0x2CB: prog_ptr = v & 0xfff; break;
    case 0x2CC: case 0x2CD: case 0x2CE: case 0x2CF: case 0x2D0: case 0x2D1: case 0x2D2: case 0x2D3:
        prog[prog_ptr++ & 4095] = v; pica_vs_version++; break;
    case 0x2D5: swz_ptr = v & 0x7f; break;
    case 0x2D6: case 0x2D7: case 0x2D8: case 0x2D9: case 0x2DA: case 0x2DB: case 0x2DC: case 0x2DD:
        swz[swz_ptr++ & 127] = v; pica_vs_version++; break;
    case 0x1C5: lut_idx = v & 0xff; lut_type = (v >> 8) & 0x1f; break;
    case 0x1C8: case 0x1C9: case 0x1CA: case 0x1CB: case 0x1CC: case 0x1CD: case 0x1CE: case 0x1CF:
        if (lut_type < 24) light_lut[lut_type][lut_idx & 255] = v;
        lut_idx++; light_lut_version++; break;
    case 0x0E6: fog_idx = v & 0x7f; break;
    case 0x0E8: case 0x0E9: case 0x0EA: case 0x0EB: case 0x0EC: case 0x0ED: case 0x0EE: case 0x0EF:
        fog_lut[fog_idx++ & 127] = v; light_lut_version++; break;
    case 0x232: fixed_idx = v & 0xf; fixed_n = 0; if (fixed_idx == 0xf) imm_attr = 0; break;
    case 0x233: case 0x234: case 0x235:
        fixed_buf[fixed_n++] = v;
        if (fixed_n == 3) {
            fixed_n = 0;
            if (fixed_idx == 0xf) {   /* immediate-mode vertex: attributes 0..max input index in order */
                if (imm_attr < 12) imm_v[imm_n][imm_attr] = unpack_f24x4(fixed_buf);
                if (++imm_attr > (R(0x242) & 0xf)) { imm_attr = 0; if (++imm_n == IMM_MAX) pica_sw_imm_flush(); }
            } else { if (fixed_idx < 12) fixed_attr[fixed_idx] = unpack_f24x4(fixed_buf); fixed_idx++; }
        }
        break;
    case 0x245: if (v & 1) pica_sw_imm_flush(); break;
    }
}

/* ---------------- vertex shader interpreter ---------------- */
typedef struct { V4 in[16], out[16], r[16]; int a0x, a0y, aL; int cx, cy; } VS;

static inline float pmul(float a, float b) { return (a == 0 || b == 0) ? 0.0f : a * b; }   /* PICA: 0 * inf = 0 */
static inline float comp(V4 v, int i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; }

static V4 src_reg(VS *s, uint32_t idx, uint32_t sel, int neg, int rel) {
    V4 v;
    if (idx < 0x10) v = s->in[idx];
    else if (idx < 0x20) v = s->r[idx - 0x10];
    else {
        int ci = (int)idx - 0x20 + (rel == 1 ? s->a0x : rel == 2 ? s->a0y : rel == 3 ? s->aL : 0);
        v = (ci >= 0 && ci < 96) ? cf[ci] : (V4){ 1, 1, 1, 1 };
    }
    V4 o = { comp(v, (sel >> 6) & 3), comp(v, (sel >> 4) & 3), comp(v, (sel >> 2) & 3), comp(v, sel & 3) };
    if (neg) { o.x = -o.x; o.y = -o.y; o.z = -o.z; o.w = -o.w; }
    return o;
}
static void dst_write(VS *s, uint32_t d, uint32_t mask, V4 v) {
    V4 *t = d < 0x10 ? &s->out[d] : &s->r[d - 0x10 & 15];
    if (mask & 8) t->x = v.x;
    if (mask & 4) t->y = v.y;
    if (mask & 2) t->z = v.z;
    if (mask & 1) t->w = v.w;
}
static int cond_ok(VS *s, uint32_t ins) {
    int refx = (ins >> 25) & 1, refy = (ins >> 24) & 1, op = (ins >> 22) & 3;
    int x = s->cx == refx, y = s->cy == refy;
    return op == 0 ? (x || y) : op == 1 ? (x && y) : op == 2 ? x : y;
}
static int cmp_op(float a, float b, int op) {
    switch (op) { case 0: return a == b; case 1: return a != b; case 2: return a < b; case 3: return a <= b; case 4: return a > b; case 5: return a >= b; }
    return 0;
}

/* run [pc, end) ; returns 1 when END reached */
static int vs_run(VS *s, uint32_t pc, uint32_t end, int depth) {
    if (depth > 8) return 1;
    while (pc < end && pc < 4096) {
        uint32_t ins = prog[pc], op = ins >> 26;
        if (op < 0x20 || (op >= 0x2e)) {
            /* arithmetic */
            if (op >= 0x30) {   /* MAD / MADI */
                uint32_t d = swz[ins & 0x1f], dest = (ins >> 24) & 0x1f, rel = (ins >> 22) & 3;
                int madi = op < 0x38;
                uint32_t s1 = (ins >> 17) & 0x1f, s2, s3;
                if (madi) { s2 = (ins >> 12) & 0x1f; s3 = (ins >> 5) & 0x7f; } else { s2 = (ins >> 10) & 0x7f; s3 = (ins >> 5) & 0x1f; }
                V4 a = src_reg(s, s1, (d >> 5) & 0xff, (d >> 4) & 1, 0);
                V4 b = src_reg(s, s2, (d >> 14) & 0xff, (d >> 13) & 1, madi ? 0 : (int)rel);
                V4 c = src_reg(s, s3, (d >> 23) & 0xff, (d >> 22) & 1, madi ? (int)rel : 0);
                V4 r = { pmul(a.x, b.x) + c.x, pmul(a.y, b.y) + c.y, pmul(a.z, b.z) + c.z, pmul(a.w, b.w) + c.w };
                dst_write(s, dest, d & 0xf, r); pc++; continue;
            }
            uint32_t d = swz[ins & 0x7f], dest = (ins >> 21) & 0x1f, rel = (ins >> 19) & 3;
            int inv = op >= 0x18 && op <= 0x1b;
            uint32_t s1 = inv ? (ins >> 14) & 0x1f : (ins >> 12) & 0x7f, s2 = inv ? (ins >> 7) & 0x7f : (ins >> 7) & 0x1f;
            V4 a = src_reg(s, s1, (d >> 5) & 0xff, (d >> 4) & 1, inv ? 0 : (int)rel);
            V4 b = src_reg(s, s2, (d >> 14) & 0xff, (d >> 13) & 1, inv ? (int)rel : 0);
            V4 r = a; float t;
            switch (op) {
            case 0x00: r = (V4){ a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; break;
            case 0x01: t = pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z); r = (V4){ t, t, t, t }; break;
            case 0x02: t = pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z) + pmul(a.w, b.w); r = (V4){ t, t, t, t }; break;
            case 0x03: case 0x18: t = pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z) + b.w; r = (V4){ t, t, t, t }; break;
            case 0x04: case 0x19: r = (V4){ 1, pmul(a.y, b.y), a.z, b.w }; break;
            case 0x05: t = exp2f(a.x); r = (V4){ t, t, t, t }; break;
            case 0x06: t = log2f(a.x); r = (V4){ t, t, t, t }; break;
            case 0x07: { /* LITP */
                float x = fmaxf(a.x, 0), y = fmaxf(a.y, 0), w = fminf(fmaxf(a.w, -127.9961f), 127.9961f);
                r = (V4){ x, y, a.z, w }; s->cx = a.x >= 0; s->cy = a.y >= 0; break; }
            case 0x08: r = (V4){ pmul(a.x, b.x), pmul(a.y, b.y), pmul(a.z, b.z), pmul(a.w, b.w) }; break;
            case 0x09: case 0x1a: r = (V4){ a.x >= b.x, a.y >= b.y, a.z >= b.z, a.w >= b.w }; break;
            case 0x0a: case 0x1b: r = (V4){ a.x < b.x, a.y < b.y, a.z < b.z, a.w < b.w }; break;
            case 0x0b: r = (V4){ floorf(a.x), floorf(a.y), floorf(a.z), floorf(a.w) }; break;
            case 0x0c: r = (V4){ a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w }; break;
            case 0x0d: r = (V4){ a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w }; break;
            case 0x0e: t = 1.0f / a.x; r = (V4){ t, t, t, t }; break;
            case 0x0f: t = 1.0f / sqrtf(a.x); r = (V4){ t, t, t, t }; break;
            case 0x12: if (d & 8) s->a0x = (int)a.x; if (d & 4) s->a0y = (int)a.y; pc++; continue;
            case 0x13: break;
            case 0x2e: case 0x2f: s->cx = cmp_op(a.x, b.x, (ins >> 24) & 7); s->cy = cmp_op(a.y, b.y, (ins >> 21) & 7); pc++; continue;
            default: pc++; continue;
            }
            dst_write(s, dest, d & 0xf, r); pc++; continue;
        }
        uint32_t dst = (ins >> 10) & 0xfff, num = ins & 0xff;
        switch (op) {
        case 0x21: pc++; break;                                    /* NOP */
        case 0x22: return 1;                                       /* END */
        case 0x20: return 2;                                       /* BREAK (handled by LOOP) */
        case 0x23: if (cond_ok(s, ins)) return 2; pc++; break;     /* BREAKC */
        case 0x24: case 0x25: case 0x26: {                         /* CALL / CALLC / CALLU */
            int take = op == 0x24 ? 1 : op == 0x25 ? cond_ok(s, ins) : (int)((R(0x2B0) >> ((ins >> 22) & 15)) & 1);
            if (take && vs_run(s, dst, dst + num, depth + 1) == 1) return 1;
            pc++; break;
        }
        case 0x27: case 0x28: {                                    /* IFU / IFC */
            int c = op == 0x27 ? (int)((R(0x2B0) >> ((ins >> 22) & 15)) & 1) : cond_ok(s, ins);
            int r = c ? vs_run(s, pc + 1, dst, depth + 1) : vs_run(s, dst, dst + num, depth + 1);
            if (r) return r;
            pc = dst + num; break;
        }
        case 0x29: {                                               /* LOOP i: aL = init; count+1 iterations of [pc+1, dst] */
            uint32_t iu = R(0x2B1 + ((ins >> 22) & 3));
            int count = (int)(iu & 0xff), step = (int8_t)(iu >> 16);
            s->aL = (int)((iu >> 8) & 0xff);
            for (int i = 0; i <= count; i++) {
                int r = vs_run(s, pc + 1, dst + 1, depth + 1);
                if (r == 1) return 1;
                if (r == 2) break;
                s->aL += step;
            }
            pc = dst + 1; break;
        }
        case 0x2c: if (cond_ok(s, ins)) pc = dst; else pc++; break;                        /* JMPC */
        case 0x2d: {                                                                       /* JMPU */
            int b = (int)((R(0x2B0) >> ((ins >> 22) & 15)) & 1);
            if (b != (int)(num & 1)) pc = dst; else pc++;
            break;
        }
        default: pc++; break;                                     /* EMIT / SETEMIT (geometry shader) */
        }
    }
    return 0;
}

/* ---------------- vertex fetch ---------------- */

/* the vertex's attributes before the input permutation; returns their count */
/* a vertex or index buffer: offset from the attribute base (0x200), added in physical address space */
static uint32_t vtx_va(uint32_t off) { return pa_to_va(((R(0x200) & 0x1fffffff) << 3) + off); }

int pica_num_attrs(void) { return imm_active ? (int)(R(0x242) & 0xf) + 1 : (int)(R(0x202) >> 28) + 1; }
int pica_fetch_attrs(uint32_t index, V4 *attr) {
    if (imm_active) {
        int na = (int)(R(0x242) & 0xf) + 1; if (na > 12) na = 12;
        memcpy(attr, imm_v[index], (size_t)na * sizeof(V4));
        return na;
    }
    uint32_t fmt_lo = R(0x201), fmt_hi = R(0x202);
    int nattr = (int)(fmt_hi >> 28) + 1;
    for (int a = 0; a < nattr; a++) attr[a] = (V4){ 0, 0, 0, 1 };
    for (int a = 0; a < nattr && a < 12; a++) if ((fmt_hi >> (16 + a)) & 1) attr[a] = fixed_attr[a];
    for (int l = 0; l < 12; l++) {
        uint32_t off = R(0x203 + 3 * l), cl = R(0x204 + 3 * l), ch = R(0x205 + 3 * l);
        int ncomp = (int)(ch >> 28), stride = (int)((ch >> 16) & 0xff);
        if (!ncomp) continue;
        uint32_t p = vtx_va(off) + index * (uint32_t)stride;
        for (int k = 0; k < ncomp; k++) {
            uint32_t c = k < 8 ? (cl >> (4 * k)) & 15 : (ch >> (4 * (k - 8))) & 15;
            if (c >= 12) { p += (c - 11) * 4; continue; }
            uint32_t f = c < 8 ? (fmt_lo >> (4 * c)) & 15 : (fmt_hi >> (4 * (c - 8))) & 15;
            int type = f & 3, n = (int)(f >> 2) + 1, sz = type == 0 || type == 1 ? 1 : type == 2 ? 2 : 4;
            float v[4] = { 0, 0, 0, 1 };
            for (int i = 0; i < n; i++, p += (uint32_t)sz) {
                switch (type) {
                case 0: v[i] = (float)(int8_t)rd8(p); break;
                case 1: v[i] = (float)rd8(p); break;
                case 2: v[i] = (float)(int16_t)rd16(p); break;
                case 3: v[i] = f32u(rd32(p)); break;
                }
            }
            if (c < (uint32_t)nattr) attr[c] = (V4){ v[0], v[1], v[2], v[3] };
        }
    }
    return nattr;
}

int pica_fetch_ops(FetchOp *op, V4 *def) {
    if (imm_active) return -1;
    int nattr = pica_num_attrs();
    uint32_t fmt_lo = R(0x201), fmt_hi = R(0x202);
    for (int a = 0; a < nattr; a++) def[a] = (V4){ 0, 0, 0, 1 };
    for (int a = 0; a < nattr && a < 12; a++) if ((fmt_hi >> (16 + a)) & 1) def[a] = fixed_attr[a];
    int nop = 0;
    for (int l = 0; l < 12; l++) {
        uint32_t off = R(0x203 + 3 * l), cl = R(0x204 + 3 * l), ch = R(0x205 + 3 * l);
        int ncomp = (int)(ch >> 28), stride = (int)((ch >> 16) & 0xff);
        if (!ncomp) continue;
        uint32_t p = vtx_va(off);
        for (int k = 0; k < ncomp; k++) {
            uint32_t c = k < 8 ? (cl >> (4 * k)) & 15 : (ch >> (4 * (k - 8))) & 15;
            if (c >= 12) { p += (c - 11) * 4; continue; }
            uint32_t f = c < 8 ? (fmt_lo >> (4 * c)) & 15 : (fmt_hi >> (4 * (c - 8))) & 15;
            int type = f & 3, n = (int)(f >> 2) + 1, sz = type == 0 || type == 1 ? 1 : type == 2 ? 2 : 4;
            if (c < (uint32_t)nattr && nop < 48) op[nop++] = (FetchOp){ p, (uint32_t)stride, (uint8_t)type, (uint8_t)n, (uint8_t)c, (uint8_t)l };
            p += (uint32_t)(n * sz);
        }
    }
    return nop;
}

/* pica_fetch_attrs for many vertices: the loader registers are decoded once per draw instead of once per vertex
   (the per-vertex decode was ~25% of the GX thread's busy time). out gets nattr vec4 per vertex. */
void pica_fetch_attrs_n(const uint32_t *index, uint32_t count, V4 *out) {
    int nattr = pica_num_attrs();
    FetchOp op[48]; V4 def[16];
    int nop = pica_fetch_ops(op, def);
    if (nop < 0) { for (uint32_t i = 0; i < count; i++) pica_fetch_attrs(index[i], out + (size_t)i * nattr); return; }
    for (uint32_t i = 0; i < count; i++) {
        V4 *a = out + (size_t)i * nattr;
        memcpy(a, def, (size_t)nattr * sizeof(V4));
        for (int o = 0; o < nop; o++) {
            const uint8_t *p = g_mem + op[o].addr + index[i] * op[o].stride;
            float v[4] = { 0, 0, 0, 1 };
            switch (op[o].type) {
            case 0: for (int k = 0; k < op[o].n; k++) v[k] = (float)(int8_t)p[k]; break;
            case 1: for (int k = 0; k < op[o].n; k++) v[k] = (float)p[k]; break;
            case 2: for (int k = 0; k < op[o].n; k++) { int16_t s; memcpy(&s, p + 2 * k, 2); v[k] = (float)s; } break;
            case 3: memcpy(v, p, (size_t)op[o].n * 4); break;
            }
            memcpy(&a[op[o].dst], v, sizeof v);
        }
    }
}

static void load_vertex(uint32_t index, VS *s) {
    V4 attr[16];
    int nattr = pica_fetch_attrs(index, attr);
    /* attribute a -> input register given by the permutation */
    uint64_t perm = (uint64_t)R(0x2BB) | (uint64_t)R(0x2BC) << 32;
    for (int a = 0; a < nattr; a++) s->in[(perm >> (4 * a)) & 15] = attr[a];
}

static Vtx shade_vertex(uint32_t index) {
    VS s; memset(&s, 0, sizeof s);
    load_vertex(index, &s);
    vs_run(&s, R(0x2BA) & 0xffff, 4096, 0);
    /* output map: register 0x50+i holds the semantics of output register i's x,y,z,w */
    float sem[24]; for (int i = 0; i < 24; i++) sem[i] = 0;
    sem[3] = 1; sem[11] = 1;
    int nout = (int)(R(0x04F) & 7);
    for (int i = 0, o = 0; i < 16 && o < nout; i++) {
        if (!((R(0x2BD) >> i) & 1)) continue;
        uint32_t m = R(0x050 + o);
        for (int c = 0; c < 4; c++) { uint32_t se = (m >> (8 * c)) & 0x1f; if (se < 24) sem[se] = comp(s.out[i], c); }
        o++;
    }
    Vtx v;
    v.pos = (V4){ sem[0], sem[1], sem[2], sem[3] };
    static int nd; if (nd < 2 && v.pos.x != v.pos.x && getenv("PWR_SW_SAMPLE")) {
        nd++;
        fprintf(stderr, "sw: NaN vertex idx=%u entry=%x nattr=%u perm=%08x:%08x fmt=%08x:%08x base=%08x\n", index, R(0x2BA) & 0xffff, (R(0x202) >> 28) + 1, R(0x2BB), R(0x2BC), R(0x201), R(0x202), R(0x200));
        for (int i = 0; i < 8; i++) fprintf(stderr, "    v%d=(%g %g %g %g) r%d=(%g %g %g %g)\n", i, s.in[i].x, s.in[i].y, s.in[i].z, s.in[i].w, i, s.r[i].x, s.r[i].y, s.r[i].z, s.r[i].w);
        for (int i = 0; i < 8; i++) fprintf(stderr, "    c%d=(%g %g %g %g)\n", i, cf[i].x, cf[i].y, cf[i].z, cf[i].w);
        for (int l = 0; l < 3; l++) fprintf(stderr, "    loader%d off=%08x comps=%08x hi=%08x\n", l, R(0x203 + 3 * l), R(0x204 + 3 * l), R(0x205 + 3 * l));
        fprintf(stderr, "    uniform mode reg 0x2C0=%08x\n", R(0x2C0));
    }
    v.col = (V4){ fminf(fabsf(sem[8]), 1), fminf(fabsf(sem[9]), 1), fminf(fabsf(sem[10]), 1), fminf(fabsf(sem[11]), 1) };
    v.uv[0][0] = sem[12]; v.uv[0][1] = sem[13]; v.uv[1][0] = sem[14]; v.uv[1][1] = sem[15];
    v.uv[2][0] = sem[22]; v.uv[2][1] = sem[23]; v.tw = sem[16];
    v.quat = (V4){ sem[4], sem[5], sem[6], sem[7] };
    v.view[0] = sem[18]; v.view[1] = sem[19]; v.view[2] = sem[20];
    return v;
}

/* ---------------- textures ---------------- */
static const uint8_t mx[8] = { 0, 1, 4, 5, 16, 17, 20, 21 }, my[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
const int pica_tex_bpp[14] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8 };
static const int etc_mod[8][2] = { { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };

static uint32_t etc_texel(uint32_t blk, int x, int y, int alpha) {
    uint64_t v = (uint64_t)rd32(blk + (alpha ? 8 : 0)) | (uint64_t)rd32(blk + (alpha ? 12 : 4)) << 32;
    uint32_t hi = (uint32_t)(v >> 32), lo = (uint32_t)v;
    int flip = hi & 1, diff = (hi >> 1) & 1, sub = flip ? (y >= 2) : (x >= 2);
    int r, g, b;
    if (diff) {
        int r1 = hi >> 27 & 31, g1 = hi >> 19 & 31, b1 = hi >> 11 & 31;
        if (sub) { r1 += ((int)(hi >> 24 & 7) ^ 4) - 4; g1 += ((int)(hi >> 16 & 7) ^ 4) - 4; b1 += ((int)(hi >> 8 & 7) ^ 4) - 4; }
        r = (r1 << 3) | (r1 >> 2 & 7); g = (g1 << 3) | (g1 >> 2 & 7); b = (b1 << 3) | (b1 >> 2 & 7);
    } else {
        r = (int)(sub ? hi >> 24 & 15 : hi >> 28 & 15) * 17; g = (int)(sub ? hi >> 16 & 15 : hi >> 20 & 15) * 17; b = (int)(sub ? hi >> 8 & 15 : hi >> 12 & 15) * 17;
    }
    int tbl = (int)(sub ? hi >> 2 & 7 : hi >> 5 & 7), k = x * 4 + y;
    int idx = (int)((lo >> k) & 1) | (int)(((lo >> (k + 16)) & 1) << 1);
    int d = idx == 0 ? etc_mod[tbl][0] : idx == 1 ? etc_mod[tbl][1] : idx == 2 ? -etc_mod[tbl][0] : -etc_mod[tbl][1];
    r += d; g += d; b += d;
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    int a = 255;
    if (alpha) { uint64_t am = (uint64_t)rd32(blk) | (uint64_t)rd32(blk + 4) << 32; a = (int)((am >> (k * 4)) & 15) * 17; }
    return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
}

/* texel at memory coordinates (x, y), RGBA8 packed r | g<<8 | b<<16 | a<<24 */
uint32_t pica_texel(uint32_t addr, int fmt, int w, int x, int y) {
    if (fmt == 12 || fmt == 13) {
        int bs = fmt == 13 ? 16 : 8;
        uint32_t tile = (uint32_t)((y / 8) * (w / 8) + x / 8), sub = (uint32_t)(((y & 7) / 4) * 2 + (x & 7) / 4);
        return etc_texel(addr + (tile * 4 + sub) * (uint32_t)bs, x & 3, y & 3, fmt == 13);
    }
    uint32_t i = mx[x & 7] + my[y & 7] + (uint32_t)(x & ~7) * 8 + (uint32_t)(y & ~7) * (uint32_t)w;
    int bpp = pica_tex_bpp[fmt];
    uint32_t p = addr + i * (uint32_t)bpp / 8, v; int r, g, b, a = 255;
    switch (fmt) {
    case 0: return (uint32_t)rd8(p + 3) | rd8(p + 2) << 8 | rd8(p + 1) << 16 | rd8(p) << 24;
    case 1: return (uint32_t)rd8(p + 2) | rd8(p + 1) << 8 | rd8(p) << 16 | 0xff000000u;
    case 2: v = rd16(p); r = (v >> 11) * 255 / 31; g = (v >> 6 & 31) * 255 / 31; b = (v >> 1 & 31) * 255 / 31; a = (v & 1) * 255; break;
    case 3: v = rd16(p); r = (v >> 11) * 255 / 31; g = (v >> 5 & 63) * 255 / 63; b = (v & 31) * 255 / 31; break;
    case 4: v = rd16(p); r = (v >> 12) * 17; g = (v >> 8 & 15) * 17; b = (v >> 4 & 15) * 17; a = (v & 15) * 17; break;
    case 5: v = rd16(p); r = g = b = (int)(v >> 8); a = (int)(v & 0xff); break;
    case 6: v = rd16(p); r = (int)(v >> 8); g = (int)(v & 0xff); b = 0; break;
    case 7: r = g = b = (int)rd8(p); break;
    case 8: r = g = b = 0; a = (int)rd8(p); break;
    case 9: v = rd8(p); r = g = b = (int)(v >> 4) * 17; a = (int)(v & 15) * 17; break;
    case 10: v = rd8(p); r = g = b = (int)((i & 1) ? v >> 4 : v & 15) * 17; break;
    case 11: v = rd8(p); r = g = b = 0; a = (int)((i & 1) ? v >> 4 : v & 15) * 17; break;
    default: return 0xffff00ffu;
    }
    return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
}

/* One ETC1 block (8 bytes, or 16 with ETC1A4's alpha first), all 16 texels as etc_texel gives them, c[x * 4 + y]: what
   depends on the block alone (its words, base colours, tables) once instead of per texel */
static void etc_block(const uint8_t *blk, int alpha, uint32_t c[16]) {
    uint32_t w0, w1; memcpy(&w0, blk + (alpha ? 8 : 0), 4); memcpy(&w1, blk + (alpha ? 12 : 4), 4);
    uint32_t hi = w1, lo = w0;
    int flip = hi & 1, diff = (hi >> 1) & 1, base[2][3], tbl[2];
    for (int sub = 0; sub < 2; sub++) {
        int r, g, b;
        if (diff) {
            int r1 = hi >> 27 & 31, g1 = hi >> 19 & 31, b1 = hi >> 11 & 31;
            if (sub) { r1 += ((int)(hi >> 24 & 7) ^ 4) - 4; g1 += ((int)(hi >> 16 & 7) ^ 4) - 4; b1 += ((int)(hi >> 8 & 7) ^ 4) - 4; }
            r = (r1 << 3) | (r1 >> 2 & 7); g = (g1 << 3) | (g1 >> 2 & 7); b = (b1 << 3) | (b1 >> 2 & 7);
        } else {
            r = (int)(sub ? hi >> 24 & 15 : hi >> 28 & 15) * 17; g = (int)(sub ? hi >> 16 & 15 : hi >> 20 & 15) * 17; b = (int)(sub ? hi >> 8 & 15 : hi >> 12 & 15) * 17;
        }
        base[sub][0] = r; base[sub][1] = g; base[sub][2] = b; tbl[sub] = (int)(sub ? hi >> 2 & 7 : hi >> 5 & 7);
    }
    uint32_t pal[2][4];   /* each half's four colours (modifier and clamp done), alpha 255 */
    for (int sub = 0; sub < 2; sub++) for (int idx = 0; idx < 4; idx++) {
        const int *m = etc_mod[tbl[sub]];
        int d = idx == 0 ? m[0] : idx == 1 ? m[1] : idx == 2 ? -m[0] : -m[1];
        int r = base[sub][0] + d, g = base[sub][1] + d, b = base[sub][2] + d;
        r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
        pal[sub][idx] = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | 0xff000000u;
    }
    uint64_t am = 0;
    if (alpha) { uint32_t a0, a1; memcpy(&a0, blk, 4); memcpy(&a1, blk + 4, 4); am = (uint64_t)a0 | (uint64_t)a1 << 32; }
    for (int k = 0; k < 16; k++) {   /* k = x * 4 + y */
        int x = k >> 2, y = k & 3, sub = flip ? (y >= 2) : (x >= 2);
        uint32_t v = pal[sub][((lo >> k) & 1) | (((lo >> (k + 16)) & 1) << 1)];
        if (alpha) v = (v & 0xffffffu) | (uint32_t)((am >> (k * 4)) & 15) * 17u << 24;
        c[k] = v;
    }
}

/* Tile rows ty0, ty0 + step, ... of a texture level, as pica_texel gives them, into out (w * h texels; row j is memory
   row h - 1 - j, as pica_vk.c uploads it): by 8x8 tiles in memory order, ETC1 by 4x4 blocks (w and h multiples of 8) */
static uint8_t tx_of[64], ty_of[64], c5[32], c6[64];   /* Morton index in a tile -> (x, y); pica_texel's 5- and 6-bit expansions */
static void decode_tile_rows(const uint8_t *src, int fmt, uint32_t w, uint32_t h, uint32_t *out, uint32_t ty0, uint32_t step) {
    const uint32_t tw = w / 8, th = h / 8;
    if (fmt == 12 || fmt == 13) {
        const int bs = fmt == 13 ? 16 : 8; uint32_t c[16];
        for (uint32_t ty = ty0; ty < th; ty += step) for (uint32_t tx = 0; tx < tw; tx++) {
            const uint8_t *tile = src + ((size_t)ty * tw + tx) * 4 * (size_t)bs;
            for (int sub = 0; sub < 4; sub++) {
                etc_block(tile + sub * bs, fmt == 13, c);
                uint32_t bx = tx * 8 + (uint32_t)(sub & 1) * 4, by = ty * 8 + (uint32_t)(sub >> 1) * 4;
                for (uint32_t y = 0; y < 4; y++) {
                    uint32_t *row = out + (size_t)(h - 1 - (by + y)) * w + bx;
                    row[0] = c[0 + y]; row[1] = c[4 + y]; row[2] = c[8 + y]; row[3] = c[12 + y];
                }
            }
        }
        return;
    }
    const uint32_t bpp = (uint32_t)pica_tex_bpp[fmt];
    for (uint32_t ty = ty0; ty < th; ty += step) for (uint32_t tx = 0; tx < tw; tx++) {
        const uint32_t t0 = (ty * tw + tx) * 64;   /* the tile's first texel index */
        uint32_t *base = out + (size_t)(h - 1 - ty * 8) * w + tx * 8;   /* (x, y) of the tile -> base[x - y * w] */
        for (uint32_t i = 0; i < 64; i++) {
            const uint32_t idx = t0 + i; const uint8_t *p = src + (size_t)idx * bpp / 8;
            uint32_t v, r, a, o;
            switch (fmt) {
            case 0: o = (uint32_t)p[3] | p[2] << 8 | p[1] << 16 | (uint32_t)p[0] << 24; break;
            case 1: o = (uint32_t)p[2] | p[1] << 8 | p[0] << 16 | 0xff000000u; break;
            case 2: v = (uint32_t)p[0] | p[1] << 8; o = c5[v >> 11] | c5[v >> 6 & 31] << 8 | c5[v >> 1 & 31] << 16 | (v & 1) * 255u << 24; break;
            case 3: v = (uint32_t)p[0] | p[1] << 8; o = c5[v >> 11] | c6[v >> 5 & 63] << 8 | c5[v & 31] << 16 | 0xff000000u; break;
            case 4: v = (uint32_t)p[0] | p[1] << 8; o = (v >> 12) * 17 | (v >> 8 & 15) * 17 << 8 | (v >> 4 & 15) * 17 << 16 | (v & 15) * 17 << 24; break;
            case 5: v = (uint32_t)p[0] | p[1] << 8; r = v >> 8; o = r | r << 8 | r << 16 | (v & 0xff) << 24; break;
            case 6: v = (uint32_t)p[0] | p[1] << 8; o = (v >> 8) | (v & 0xff) << 8 | 0xff000000u; break;
            case 7: r = p[0]; o = r | r << 8 | r << 16 | 0xff000000u; break;
            case 8: o = (uint32_t)p[0] << 24; break;
            case 9: v = p[0]; r = (v >> 4) * 17; a = (v & 15) * 17; o = r | r << 8 | r << 16 | a << 24; break;
            case 10: v = p[0]; r = ((idx & 1) ? v >> 4 : v & 15) * 17; o = r | r << 8 | r << 16 | 0xff000000u; break;
            default: v = p[0]; a = ((idx & 1) ? v >> 4 : v & 15) * 17; o = a << 24; break;   /* 11: A4 */
            }
            base[(ptrdiff_t)tx_of[i] - (ptrdiff_t)ty_of[i] * (ptrdiff_t)w] = o;
        }
    }
}
static void pool_run(void (*job)(int id, int n));
static struct { const uint8_t *src; int fmt; uint32_t w, h; uint32_t *out; } dec;   /* the GX thread's decode, for the pool */
static void decode_job(int id, int n) { decode_tile_rows(dec.src, dec.fmt, dec.w, dec.h, dec.out, (uint32_t)id, (uint32_t)n); }
/* A whole texture level as pica_texel gives it, into out (w * h texels; row j is memory row h - 1 - j). Per texel
   through pica_texel, a 1024x1024 ETC1 texture with its mipmaps took 39 ms of the GX thread on a Steam Deck the first
   time it was drawn (2026-11-03); by blocks ~5 ms, and levels from 128x128 up are shared with the worker pool (its
   threads are idle while the GPU renders). GX thread only. */
void pica_decode_level(uint32_t addr, int fmt, uint32_t w, uint32_t h, uint32_t *out) {
    if (w < 8 || h < 8 || (w & 7) || (h & 7) || fmt < 0 || fmt > 13) {   /* not whole tiles: texel by texel */
        for (uint32_t j = 0; j < h; j++) for (uint32_t x = 0; x < w; x++) out[(size_t)j * w + x] = pica_texel(addr, fmt, (int)w, (int)x, (int)(h - 1 - j));
        return;
    }
    static int ready;
    if (!ready) {
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { tx_of[mx[x] + my[y]] = (uint8_t)x; ty_of[mx[x] + my[y]] = (uint8_t)y; }
        for (int i = 0; i < 32; i++) c5[i] = (uint8_t)(i * 255 / 31);
        for (int i = 0; i < 64; i++) c6[i] = (uint8_t)(i * 255 / 63);
        ready = 1;
    }
    if (w * h < 128 * 128) { decode_tile_rows(g_mem + addr, fmt, w, h, out, 0, 1); return; }
    dec.src = g_mem + addr; dec.fmt = fmt; dec.w = w; dec.h = h; dec.out = out;
    pool_run(decode_job);
}

typedef struct { float r, g, b, a; } C4;
static C4 unpack(uint32_t v) { return (C4){ (v & 255) / 255.f, (v >> 8 & 255) / 255.f, (v >> 16 & 255) / 255.f, (v >> 24) / 255.f }; }

static int wrap(int c, int size, int mode) {
    switch (mode) {
    case 0: return c < 0 ? 0 : c >= size ? size - 1 : c;                         /* clamp to edge */
    case 1: return (c < 0 || c >= size) ? -1 : c;                                  /* clamp to border */
    case 3: { int m = ((c % (2 * size)) + 2 * size) % (2 * size); return m < size ? m : 2 * size - 1 - m; }   /* mirror */
    default: return ((c % size) + size) % size;                                    /* repeat */
    }
}

static C4 sample(int unit, float u, float v) {
    static const int dimr[3] = { 0x082, 0x092, 0x09A }, parr[3] = { 0x083, 0x093, 0x09B }, addrr[3] = { 0x085, 0x095, 0x09D }, fmtr[3] = { 0x08E, 0x096, 0x09E }, bordr[3] = { 0x081, 0x091, 0x099 };
    uint32_t dim = R(dimr[unit]), par = R(parr[unit]);
    int h = (int)(dim & 0x7ff), w = (int)((dim >> 16) & 0x7ff), fmt = (int)(R(fmtr[unit]) & 15);
    uint32_t addr = pa_to_va(R(addrr[unit]) << 3);
    if (!w || !h || !addr || fmt > 13) return (C4){ 1, 1, 1, 1 };
    int ws = (int)((par >> 12) & 7), wt = (int)((par >> 8) & 7), lin = (par >> 1) & 1;
    float fs = u * (float)w, ft = v * (float)h;
    if (lin) { fs -= 0.5f; ft -= 0.5f; }
    int s0 = (int)floorf(fs), t0 = (int)floorf(ft);
    C4 out = { 0, 0, 0, 0 };
    int taps = lin ? 2 : 1;
    float fx = fs - (float)s0, fy = ft - (float)t0;
    for (int j = 0; j < taps; j++) for (int i = 0; i < taps; i++) {
        int x = wrap(s0 + i, w, ws), y = wrap(t0 + j, h, wt);
        C4 c = (x < 0 || y < 0) ? unpack(R(bordr[unit])) : unpack(pica_texel(addr, fmt, w, x, h - 1 - y));
        float wgt = lin ? (i ? fx : 1 - fx) * (j ? fy : 1 - fy) : 1;
        out.r += c.r * wgt; out.g += c.g * wgt; out.b += c.b * wgt; out.a += c.a * wgt;
    }
    return out;
}

/* ---------------- fragment pipeline ---------------- */
static const int tev_base[6] = { 0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8 };

static inline float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* ---------------- fragment lighting ---------------- */
typedef struct { float x, y, z; } V3;
static inline V3 v3(float x, float y, float z) { return (V3){ x, y, z }; }
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vscale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 vcross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline V3 vnorm(V3 a) { float l = sqrtf(vdot(a, a)); return l > 0 ? vscale(a, 1 / l) : a; }
float pica_f16(uint32_t v) {   /* PICA float16: 1.5.10, bias 15 */
    v &= 0xffff; uint32_t sign = v >> 15, exp = (v >> 10) & 31, mant = v & 0x3ff, bits;
    if (!exp && !mant) bits = sign << 31;
    else if (exp == 31) bits = sign << 31 | 0xffu << 23 | mant << 13;
    else bits = sign << 31 | (exp + 112) << 23 | mant << 13;
    float f; memcpy(&f, &bits, 4); return f;
}
float pica_f20(uint32_t v) {   /* 1.7.12, bias 63 */
    v &= 0xfffff; uint32_t sign = v >> 19, exp = (v >> 12) & 0x7f, mant = v & 0xfff, bits;
    if (!exp && !mant) bits = sign << 31;
    else if (exp == 0x7f) bits = sign << 31 | 0xffu << 23 | mant << 11;
    else bits = sign << 31 | (exp + 64) << 23 | mant << 11;
    float f; memcpy(&f, &bits, 4); return f;
}
static V3 light_color(uint32_t v) { return v3((float)((v >> 20) & 0xff) / 255.f, (float)((v >> 10) & 0xff) / 255.f, (float)(v & 0xff) / 255.f); }
static float lut_lookup(int lut, int index, float delta) {
    uint32_t e = light_lut[lut][index & 255];
    int diff = (int)((e >> 12) & 0xfff); if (diff & 0x800) diff -= 0x1000;
    return (float)(e & 0xfff) / 4095.f + (float)diff / 2047.f * delta;
}
/* LUT samplers that each lighting configuration (config0 bits 4-7) provides; bit n = sampler n */
static int sampler_ok(int cfg, int smp) {
    static const uint8_t ok[16] = { 0x41, 0x49, 0x43, 0x4a, 0, 0x73, 0x79, 0x4b, 0x7b };
    return (ok[cfg & 15] >> smp) & 1;
}
static const float lut_scales[8] = { 1, 2, 4, 8, 0, 0, 0.25f, 0.5f };

/* fills the fragment primary (diffuse) and secondary (specular) colours */
static void fragment_light(const float quat_in[4], const float view_in[3], const C4 tex[3], C4 *prim, C4 *sec) {
    uint32_t c0 = R(0x1C3), c1 = R(0x1C4), absr = R(0x1D0), inr = R(0x1D1), scr = R(0x1D2);
    int cfg = (int)((c0 >> 4) & 15), bump = (int)((c0 >> 28) & 3), bsel = (int)((c0 >> 22) & 3);
    V3 surf_n = v3(0, 0, 1), surf_t = v3(1, 0, 0);
    if (bump && bsel < 3) {
        V3 m = v3(tex[bsel].r * 2 - 1, tex[bsel].g * 2 - 1, tex[bsel].b * 2 - 1);
        if (bump == 1) { surf_n = m; if (!((c0 >> 30) & 1)) surf_n.z = sqrtf(fmaxf(1 - m.x * m.x - m.y * m.y, 0)); }
        else surf_t = m;
    }
    float ql = sqrtf(quat_in[0] * quat_in[0] + quat_in[1] * quat_in[1] + quat_in[2] * quat_in[2] + quat_in[3] * quat_in[3]);
    if (ql <= 0) ql = 1;
    V3 q = v3(quat_in[0] / ql, quat_in[1] / ql, quat_in[2] / ql); float qw = quat_in[3] / ql;
    V3 normal = vadd(surf_n, vscale(vcross(q, vadd(vcross(q, surf_n), vscale(surf_n, qw))), 2));
    V3 tangent = vadd(surf_t, vscale(vcross(q, vadd(vcross(q, surf_t), vscale(surf_t, qw))), 2));
    V3 view = v3(view_in[0], view_in[1], view_in[2]), nview = vnorm(view);
    V3 dsum = v3(0, 0, 0), ssum = v3(0, 0, 0);
    float dalpha = 1, salpha = 1;
    int nlights = (int)(R(0x1C2) & 7) + 1;
    for (int li = 0; li < nlights; li++) {
        int id = (int)((R(0x1D9) >> (4 * li)) & 7), b = 0x140 + 0x10 * id;
        uint32_t lcfg = R(b + 9);
        int directional = lcfg & 1, two_sided = (lcfg >> 1) & 1;
        V3 pos = v3(pica_f16(R(b + 4)), pica_f16(R(b + 4) >> 16), pica_f16(R(b + 5)));
        V3 lv = vnorm(directional ? pos : vadd(pos, view));
        V3 half = vadd(nview, lv);
        /* sampler n (D0 0, D1 1, FR 3, RB 4, RG 5, RR 6) is used when its config1 disable bit is clear and the
         * lighting configuration provides it; its input/abs/scale fields sit in nibble n of 0x1D0-0x1D2 */
        static const int dis_bit[7] = { 16, 17, 18, 19, 22, 21, 20 };
        int have[7];
        for (int smp = 0; smp < 7; smp++) have[smp] = smp != 2 && !((c1 >> dis_bit[smp]) & 1) && sampler_ok(cfg, smp);
        #define LUT_INPUT(nib, out_val, lut_id) do { \
            int input = (int)((inr >> (4 * (nib))) & 7), use_abs = !((absr >> (1 + 4 * (nib))) & 1); float r_; \
            switch (input) { \
            case 0: r_ = vdot(normal, vnorm(half)); break; \
            case 1: r_ = vdot(nview, vnorm(half)); break; \
            case 2: r_ = vdot(normal, nview); break; \
            case 3: r_ = vdot(lv, normal); break; \
            case 4: { int sx = (int)(R(b + 6) & 0x1fff), sy = (int)((R(b + 6) >> 16) & 0x1fff), sz = (int)(R(b + 7) & 0x1fff); \
                if (sx & 0x1000) sx -= 0x2000; if (sy & 0x1000) sy -= 0x2000; if (sz & 0x1000) sz -= 0x2000; \
                r_ = vdot(lv, v3((float)sx / 2047.f, (float)sy / 2047.f, (float)sz / 2047.f)); break; } \
            case 5: { V3 hp = vsub(half, vscale(normal, vdot(normal, half))); r_ = vdot(vnorm(hp), tangent); break; } \
            default: r_ = 0; \
            } \
            int idx_; float dl_; \
            if (use_abs) { r_ = two_sided ? fabsf(r_) : fmaxf(r_, 0); float fl = floorf(r_ * 256); fl = fl < 0 ? 0 : fl > 255 ? 255 : fl; idx_ = (int)fl; dl_ = r_ * 256 - fl; } \
            else { float fl = floorf(r_ * 128); fl = fl < -128 ? -128 : fl > 127 ? 127 : fl; idx_ = (int)fl & 0xff; dl_ = r_ * 128 - fl; } \
            out_val = lut_lookup(lut_id, idx_, dl_) * lut_scales[(scr >> (4 * (nib))) & 7]; \
        } while (0)
        float dist_att = 1, spot_att = 1;
        if (!((c1 >> (24 + id)) & 1)) {
            float d = sqrtf(vdot(vadd(view, pos), vadd(view, pos)));
            float sm = clampf(pica_f20(R(b + 0xB)) * d + pica_f20(R(b + 0xA)));
            float fl = floorf(sm * 256); if (fl > 255) fl = 255;
            dist_att = lut_lookup(16 + id, (int)fl, sm * 256 - fl);
        }
        if (!((c1 >> (8 + id)) & 1)) LUT_INPUT(2, spot_att, 8 + id);
        float d0 = 1, d1 = 1, fr = 1; V3 refl;
        if (have[0]) LUT_INPUT(0, d0, 0);
        if (have[6]) LUT_INPUT(6, refl.x, 6); else refl.x = 1;
        if (have[5]) LUT_INPUT(5, refl.y, 5); else refl.y = refl.x;
        if (have[4]) LUT_INPUT(4, refl.z, 4); else refl.z = refl.x;
        if (have[1]) LUT_INPUT(1, d1, 1);
        if (have[3]) { LUT_INPUT(3, fr, 3); if ((c0 >> 2) & 1) dalpha = fr; if ((c0 >> 3) & 1) salpha = fr; }
        #undef LUT_INPUT
        V3 spec0 = vscale(light_color(R(b + 0)), d0);
        V3 spec1 = light_color(R(b + 1)); spec1 = v3(spec1.x * refl.x * d1, spec1.y * refl.y * d1, spec1.z * refl.z * d1);
        float ndl = vdot(lv, normal); ndl = two_sided ? fabsf(ndl) : fmaxf(ndl, 0);
        float clamp_h = ((c0 >> 27) & 1) ? (ndl == 0 ? 0.f : 1.f) : 1.f;
        if ((lcfg >> 2) & 1 || (lcfg >> 3) & 1) {
            float g = vdot(half, half); g = g == 0 ? 0 : fminf(ndl / g, 1);
            if ((lcfg >> 2) & 1) spec0 = vscale(spec0, g);
            if ((lcfg >> 3) & 1) spec1 = vscale(spec1, g);
        }
        float att = dist_att * spot_att;
        dsum = vadd(dsum, vscale(vadd(vscale(light_color(R(b + 2)), ndl), light_color(R(b + 3))), att));
        ssum = vadd(ssum, vscale(vadd(spec0, spec1), clamp_h * att));
    }
    dsum = vadd(dsum, light_color(R(0x1C0)));
    *prim = (C4){ clampf(dsum.x), clampf(dsum.y), clampf(dsum.z), clampf(dalpha) };
    *sec = (C4){ clampf(ssum.x), clampf(ssum.y), clampf(ssum.z), clampf(salpha) };
}
static C4 tev_src(int src, C4 prim, C4 tex[3], C4 prev, C4 buf, C4 konst, const C4 *lp, const C4 *ls) {
    switch (src) {
    case 0: return prim;
    case 1: return *lp;                                              /* fragment primary (lighting diffuse) */
    case 2: return *ls;                                              /* fragment secondary (lighting specular) */
    case 3: return tex[0]; case 4: return tex[1]; case 5: return tex[2];
    case 0xD: return buf; case 0xE: return konst; case 0xF: return prev;
    default: return (C4){ 0, 0, 0, 0 };
    }
}
static float opa(int op, C4 c) {
    switch (op) { case 0: return c.a; case 1: return 1 - c.a; case 2: return c.r; case 3: return 1 - c.r; case 4: return c.g; case 5: return 1 - c.g; case 6: return c.b; default: return 1 - c.b; }
}
static C4 oprgb(int op, C4 c) {
    switch (op) {
    case 0: return c; case 1: return (C4){ 1 - c.r, 1 - c.g, 1 - c.b, c.a };
    case 2: return (C4){ c.a, c.a, c.a, c.a }; case 3: return (C4){ 1 - c.a, 1 - c.a, 1 - c.a, c.a };
    case 4: return (C4){ c.r, c.r, c.r, c.a }; case 5: return (C4){ 1 - c.r, 1 - c.r, 1 - c.r, c.a };
    case 8: return (C4){ c.g, c.g, c.g, c.a }; case 9: return (C4){ 1 - c.g, 1 - c.g, 1 - c.g, c.a };
    case 12: return (C4){ c.b, c.b, c.b, c.a }; case 13: return (C4){ 1 - c.b, 1 - c.b, 1 - c.b, c.a };
    default: return c;
    }
}
static float comb(int op, float a, float b, float c) {
    switch (op) {
    case 0: return a; case 1: return a * b; case 2: return a + b; case 3: return a + b - 0.5f;
    case 4: return a * c + b * (1 - c); case 5: return a - b; case 8: return a * b + c; case 9: return (a + b) * c;
    default: return a;
    }
}

static C4 run_tev(C4 prim, C4 tex[3], const C4 *lp, const C4 *ls) {
    /* the combiner buffer lags a stage (as Citra models the hardware): stage 0 reads 0, stage 1 the buffer colour 0x0FD,
       stage n the buffer from before stage n-1's update */
    C4 prev = prim, buf = { 0, 0, 0, 0 }, next_buf = unpack(R(0x0FD));
    uint32_t bufcfg = R(0x0E0);
    for (int i = 0; i < 6; i++) {
        int b = tev_base[i];
        uint32_t srcs = R(b), ops = R(b + 1), cmb = R(b + 2), scl = R(b + 4);
        C4 konst = unpack(R(b + 3));
        C4 in[3]; float ia[3];
        for (int k = 0; k < 3; k++) {
            in[k] = oprgb((int)(ops >> (4 * k)) & 15, tev_src((int)(srcs >> (4 * k)) & 15, prim, tex, prev, buf, konst, lp, ls));
            ia[k] = opa((int)(ops >> (12 + 4 * k)) & 7, tev_src((int)(srcs >> (16 + 4 * k)) & 15, prim, tex, prev, buf, konst, lp, ls));
        }
        int crgb = (int)(cmb & 15), ca = (int)((cmb >> 16) & 15);
        C4 o;
        if (crgb == 6 || crgb == 7) {
            float d = 4 * ((in[0].r - .5f) * (in[1].r - .5f) + (in[0].g - .5f) * (in[1].g - .5f) + (in[0].b - .5f) * (in[1].b - .5f));
            o.r = o.g = o.b = d;
        } else {
            o.r = comb(crgb, in[0].r, in[1].r, in[2].r); o.g = comb(crgb, in[0].g, in[1].g, in[2].g); o.b = comb(crgb, in[0].b, in[1].b, in[2].b);
        }
        o.a = crgb == 7 ? o.r : comb(ca, ia[0], ia[1], ia[2]);
        float sr = (float)(1 << (scl & 3)), sa = (float)(1 << ((scl >> 16) & 3));
        o.r = clampf(o.r * sr); o.g = clampf(o.g * sr); o.b = clampf(o.b * sr); o.a = clampf(o.a * sa);
        buf = next_buf;
        if (i < 4) {
            if ((bufcfg >> (8 + i)) & 1) { next_buf.r = o.r; next_buf.g = o.g; next_buf.b = o.b; }
            if ((bufcfg >> (12 + i)) & 1) next_buf.a = o.a;
        }
        prev = o;
    }
    return prev;
}

static int cmp_func(int f, float a, float b) {
    switch (f) { case 0: return 0; case 1: return 1; case 2: return a == b; case 3: return a != b; case 4: return a < b; case 5: return a <= b; case 6: return a > b; default: return a >= b; }
}
static float blend_factor(int f, C4 s, C4 d, C4 k, int ch) {
    float sc = ch == 0 ? s.r : ch == 1 ? s.g : ch == 2 ? s.b : s.a, dc = ch == 0 ? d.r : ch == 1 ? d.g : ch == 2 ? d.b : d.a, kc = ch == 0 ? k.r : ch == 1 ? k.g : ch == 2 ? k.b : k.a;
    switch (f) {
    case 0: return 0; case 1: return 1; case 2: return sc; case 3: return 1 - sc; case 4: return dc; case 5: return 1 - dc;
    case 6: return s.a; case 7: return 1 - s.a; case 8: return d.a; case 9: return 1 - d.a;
    case 10: return kc; case 11: return 1 - kc; case 12: return k.a; case 13: return 1 - k.a;
    case 14: return ch == 3 ? 1 : fminf(s.a, 1 - d.a);
    default: return 1;
    }
}
static float blend_eq(int eq, float s, float d, float sf, float df) {
    switch (eq) { case 1: return s * sf - d * df; case 2: return d * df - s * sf; case 3: return fminf(s, d); case 4: return fmaxf(s, d); default: return s * sf + d * df; }
}

/* framebuffer access (8x8 Morton tiles, y = 0 at the bottom) */
static uint32_t fb_w, fb_h, cbuf, dbuf; static int cfmt, dfmt;
static uint32_t fb_off(uint32_t x, uint32_t y, uint32_t bpp) {
    y = fb_h - 1 - y;
    return (mx[x & 7] + my[y & 7] + (x & ~7u) * 8 + (y & ~7u) * fb_w) * bpp;
}
static C4 read_color(uint32_t x, uint32_t y) {
    static const int bpp[5] = { 4, 3, 2, 2, 2 };
    uint32_t p = cbuf + fb_off(x, y, (uint32_t)bpp[cfmt]), v;
    switch (cfmt) {
    case 0: return (C4){ rd8(p + 3) / 255.f, rd8(p + 2) / 255.f, rd8(p + 1) / 255.f, rd8(p) / 255.f };
    case 2: v = rd16(p); return (C4){ (v >> 11) / 31.f, (v >> 6 & 31) / 31.f, (v >> 1 & 31) / 31.f, (float)(v & 1) };
    case 3: v = rd16(p); return (C4){ (v >> 11) / 31.f, (v >> 5 & 63) / 63.f, (v & 31) / 31.f, 1 };
    case 4: v = rd16(p); return (C4){ (v >> 12) / 15.f, (v >> 8 & 15) / 15.f, (v >> 4 & 15) / 15.f, (v & 15) / 15.f };
    default: return (C4){ 0, 0, 0, 1 };
    }
}
static void write_color(uint32_t x, uint32_t y, C4 c, uint32_t mask) {
    static const int bpp[5] = { 4, 3, 2, 2, 2 };
    if (mask != 15) { C4 o = read_color(x, y); if (!(mask & 1)) c.r = o.r; if (!(mask & 2)) c.g = o.g; if (!(mask & 4)) c.b = o.b; if (!(mask & 8)) c.a = o.a; }
    uint32_t p = cbuf + fb_off(x, y, (uint32_t)bpp[cfmt]);
    uint32_t r = (uint32_t)(c.r * 255 + .5f), g = (uint32_t)(c.g * 255 + .5f), b = (uint32_t)(c.b * 255 + .5f), a = (uint32_t)(c.a * 255 + .5f);
    switch (cfmt) {
    case 0: wr8(p, a); wr8(p + 1, b); wr8(p + 2, g); wr8(p + 3, r); break;
    case 2: wr16(p, (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128)); break;
    case 3: wr16(p, (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); break;
    case 4: wr16(p, (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | (a >> 4)); break;
    }
}
static float read_depth(uint32_t x, uint32_t y) {
    if (dfmt == 0) return rd16(dbuf + fb_off(x, y, 2)) / 65535.f;
    uint32_t p = dbuf + fb_off(x, y, dfmt == 3 ? 4 : 3);
    return (float)(rd8(p) | rd8(p + 1) << 8 | rd8(p + 2) << 16) / 16777215.f;
}
static void write_depth(uint32_t x, uint32_t y, float z) {
    if (dfmt == 0) { wr16(dbuf + fb_off(x, y, 2), (uint32_t)(z * 65535 + .5f)); return; }
    uint32_t p = dbuf + fb_off(x, y, dfmt == 3 ? 4 : 3), v = (uint32_t)(z * 16777215 + .5f);
    wr8(p, v); wr8(p + 1, v >> 8); wr8(p + 2, v >> 16);
}
/* D24S8: the stencil byte is the top byte of each 32-bit depth word */
static uint8_t read_stencil(uint32_t x, uint32_t y) { return rd8(dbuf + fb_off(x, y, 4) + 3); }
static void stencil_op(uint32_t x, uint32_t y, int op, uint8_t old, uint8_t ref, uint8_t wmask) {
    int v = old;
    switch (op) {   /* same numbering as Vulkan's VkStencilOp */
    case 1: v = 0; break; case 2: v = ref; break; case 3: v = old < 255 ? old + 1 : 255; break; case 4: v = old > 0 ? old - 1 : 0; break;
    case 5: v = ~old & 255; break; case 6: v = (old + 1) & 255; break; case 7: v = (old - 1) & 255; break;
    }
    if (wmask) wr8(dbuf + fb_off(x, y, 4) + 3, (uint8_t)((v & wmask) | (old & ~wmask)));
}
/* fog (0x0E0 mode 5): factor from the fog LUT at depth * 128 (bit 16: 1 - depth), colour = factor * colour + (1 - factor) * 0x0E1 */
static float fog_factor(float depth) {
    float idx = (R(0x0E0) >> 16 & 1) ? (1 - depth) * 128 : depth * 128, i = floorf(idx);
    if (i < 0) i = 0; if (i > 127) i = 127;
    uint32_t e = fog_lut[(int)i]; int diff = (int)(e & 0x1fff); if (diff & 0x1000) diff -= 0x2000;
    return clampf((float)(e >> 13 & 0x7ff) / 2047.f + (float)diff / 2047.f * (idx - i));
}

/* ---------------- triangles ---------------- */
#define NA 18
typedef struct { float x, y, z, iw; float a[NA]; } SV;   /* screen vertex; a = col rgba, uv0, uv1, uv2, tw, quat, view (all /w) */

static int cull_mode;
unsigned long long g_sw_clip_plane[7];
unsigned long long g_sw_tri_in, g_sw_tri_drawn, g_sw_px_cov, g_sw_px_zfail, g_sw_px_afail, g_sw_px_written;
static void raster(const SV *v0, const SV *v1, const SV *v2, int band, int nbands) {
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0) return;
    if (cull_mode == 1 && area > 0) return;      /* same sense as the Vulkan renderer (checked on the Mii heads) */
    if (cull_mode == 2 && area < 0) return;
    int minx = (int)floorf(fminf(v0->x, fminf(v1->x, v2->x))), maxx = (int)ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    int miny = (int)floorf(fminf(v0->y, fminf(v1->y, v2->y))), maxy = (int)ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    int sx0 = 0, sy0 = 0, sx1 = (int)fb_w - 1, sy1 = (int)fb_h - 1;
    if (R(0x065) & 3) {
        int ax = (int)(R(0x066) & 0x3ff), ay = (int)(R(0x066) >> 16 & 0x3ff), bx = (int)(R(0x067) & 0x3ff), by = (int)(R(0x067) >> 16 & 0x3ff);
        if (ax > sx0) sx0 = ax; if (ay > sy0) sy0 = ay; if (bx < sx1) sx1 = bx; if (by < sy1) sy1 = by;
    }
    if (minx < sx0) minx = sx0; if (miny < sy0) miny = sy0; if (maxx > sx1) maxx = sx1; if (maxy > sy1) maxy = sy1;
    uint32_t tex_en = R(0x080), om = R(0x107), at = R(0x104), bl = R(0x101);
    int depth_test = om & 1, depth_func = (int)(om >> 4 & 7), depth_write = (om >> 12) & 1, cmask = (int)(om >> 8 & 15);
    int depth_on = R(0x114) & 3 || R(0x115) & 3, color_on = (R(0x113) & 15) != 0;
    int blend = (R(0x100) >> 8) & 1;
    /* stencil (0x105/0x106) only with a D24S8 buffer; the test then runs after the alpha test, like the hardware's order */
    int st_on = depth_on && (R(0x105) & 1) && dfmt == 3, fog_on = (R(0x0E0) & 7) == 5, wbuf = !(R(0x06D) & 1);   /* 0x06D bit 0: 1 = Z-buffer, 0 = W-buffer */
    uint32_t sf = R(0x105), so = R(0x106); uint8_t st_ref = (uint8_t)(sf >> 16), st_im = (uint8_t)(sf >> 24), st_wm = (R(0x115) & 3) ? (uint8_t)(sf >> 8) : 0;
    C4 fogc = { (R(0x0E1) & 255) / 255.f, (R(0x0E1) >> 8 & 255) / 255.f, (R(0x0E1) >> 16 & 255) / 255.f, 0 };
    int lit = (R(0x08F) & 1) && !(R(0x1C6) & 1);   /* 0x08F bit0 = enable, 0x1C6 bit0 = disable (seen in the game's lists) */
    C4 kblend = unpack(R(0x103));
    float inv_area = 1.0f / area;
    unsigned long long n_cov = 0, n_zfail = 0, n_afail = 0, n_written = 0;
    for (int y = miny; y <= maxy; y++) {
        if (((y >> 3) % nbands) != band) { y |= 7; continue; }   /* this worker owns every nbands-th 8-row band */
        for (int x = minx; x <= maxx; x++) {
        float px = (float)x + .5f, py = (float)y + .5f;
        float w0 = ((v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py)) * inv_area;
        float w1 = ((v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py)) * inv_area;
        float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        n_cov++;
        float z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
        float iw = w0 * v0->iw + w1 * v1->iw + w2 * v2->iw, rw = 1.0f / iw;
        if (wbuf) z *= rw;                 /* W-buffer: depth = (z/w * scale + offset) * w */
        z = clampf(z);                     /* also the fog's depth */
        if (!st_on && depth_on && depth_test && !cmp_func(depth_func, z, read_depth((uint32_t)x, (uint32_t)y))) { n_zfail++; continue; }
        float a[NA];
        for (int k = 0; k < (lit ? NA : 11); k++) a[k] = (w0 * v0->a[k] + w1 * v1->a[k] + w2 * v2->a[k]) * rw;
        C4 prim = { clampf(a[0]), clampf(a[1]), clampf(a[2]), clampf(a[3]) };
        C4 tex[3] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };
        if (tex_en & 1) tex[0] = sample(0, a[4], a[5]);
        if (tex_en & 2) tex[1] = sample(1, a[6], a[7]);
        if (tex_en & 4) tex[2] = (tex_en >> 13) & 1 ? sample(2, a[6], a[7]) : sample(2, a[8], a[9]);
        C4 lp = { 0, 0, 0, 0 }, ls = { 0, 0, 0, 0 };
        if (lit) fragment_light(a + 11, a + 15, tex, &lp, &ls);
        C4 c = run_tev(prim, tex, &lp, &ls);
        if (at & 1) { float ref = ((at >> 8) & 0xff) / 255.f; if (!cmp_func((int)(at >> 4 & 7), c.a, ref)) { n_afail++; continue; } }
        if (fog_on) { float f = fog_factor(z); c.r = f * c.r + (1 - f) * fogc.r; c.g = f * c.g + (1 - f) * fogc.g; c.b = f * c.b + (1 - f) * fogc.b; }
        if (st_on) {
            uint8_t old = read_stencil((uint32_t)x, (uint32_t)y);
            if (!cmp_func((int)(sf >> 4 & 7), (float)(st_ref & st_im), (float)(old & st_im))) { stencil_op((uint32_t)x, (uint32_t)y, (int)(so & 7), old, st_ref, st_wm); continue; }
            if (depth_test && !cmp_func(depth_func, z, read_depth((uint32_t)x, (uint32_t)y))) { stencil_op((uint32_t)x, (uint32_t)y, (int)(so >> 4 & 7), old, st_ref, st_wm); n_zfail++; continue; }
            stencil_op((uint32_t)x, (uint32_t)y, (int)(so >> 8 & 7), old, st_ref, st_wm);
        }
        if (color_on) {
            if (blend) {
                C4 d = read_color((uint32_t)x, (uint32_t)y);
                int eqr = (int)(bl & 7), eqa = (int)(bl >> 8 & 7), sr = (int)(bl >> 16 & 15), dr = (int)(bl >> 20 & 15), sa = (int)(bl >> 24 & 15), da = (int)(bl >> 28 & 15);
                C4 o;
                o.r = clampf(blend_eq(eqr, c.r, d.r, blend_factor(sr, c, d, kblend, 0), blend_factor(dr, c, d, kblend, 0)));
                o.g = clampf(blend_eq(eqr, c.g, d.g, blend_factor(sr, c, d, kblend, 1), blend_factor(dr, c, d, kblend, 1)));
                o.b = clampf(blend_eq(eqr, c.b, d.b, blend_factor(sr, c, d, kblend, 2), blend_factor(dr, c, d, kblend, 2)));
                o.a = clampf(blend_eq(eqa, c.a, d.a, blend_factor(sa, c, d, kblend, 3), blend_factor(da, c, d, kblend, 3)));
                c = o;
            }
            write_color((uint32_t)x, (uint32_t)y, c, (uint32_t)cmask); n_written++;
        }
        if (depth_on && depth_write) write_depth((uint32_t)x, (uint32_t)y, z);
        }
    }
    __atomic_fetch_add(&g_sw_px_cov, n_cov, __ATOMIC_RELAXED); __atomic_fetch_add(&g_sw_px_zfail, n_zfail, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_sw_px_afail, n_afail, __ATOMIC_RELAXED); __atomic_fetch_add(&g_sw_px_written, n_written, __ATOMIC_RELAXED);
}

/* clip a polygon against the PICA clip volume, then fan-triangulate */
static float plane_d(const Vtx *v, int p) {
    switch (p) {
    case 0: return v->pos.w - v->pos.x; case 1: return v->pos.w + v->pos.x; case 2: return v->pos.w - v->pos.y; case 3: return v->pos.w + v->pos.y;
    case 4: return -v->pos.z; case 5: return v->pos.z + v->pos.w; default: return v->pos.w - 1e-5f;
    }
}
static Vtx lerpv(const Vtx *a, const Vtx *b, float t) {
    Vtx o; const float *pa = (const float *)a, *pb = (const float *)b; float *po = (float *)&o;
    for (size_t i = 0; i < sizeof(Vtx) / sizeof(float); i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
    return o;
}
static SV to_screen(const Vtx *v) {
    float vw = f24(R(0x041)), vh = f24(R(0x043)), ds = f24(R(0x04D)), dofs = f24(R(0x04E));
    int vx = (int)(R(0x068) & 0x3ff), vy = (int)(R(0x068) >> 16 & 0x3ff);
    if (vx & 0x200) vx -= 0x400; if (vy & 0x200) vy -= 0x400;
    SV s; float iw = 1.0f / v->pos.w;
    s.x = (v->pos.x * iw + 1) * vw + (float)vx;
    s.y = (v->pos.y * iw + 1) * vh + (float)vy;
    s.z = v->pos.z * iw * ds + dofs;
    s.iw = iw;
    float at[NA] = { v->col.x, v->col.y, v->col.z, v->col.w, v->uv[0][0], v->uv[0][1], v->uv[1][0], v->uv[1][1], v->uv[2][0], v->uv[2][1], v->tw,
                     v->quat.x, v->quat.y, v->quat.z, v->quat.w, v->view[0], v->view[1], v->view[2] };
    for (int k = 0; k < NA; k++) s.a[k] = at[k] * iw;
    return s;
}
/* screen-space triangles of the current draw, rasterized afterwards by the worker pool */
static SV *tris; static size_t ntris, cap_tris;
static void emit_tri(const SV *a, const SV *b, const SV *c) {
    if (ntris == cap_tris) { cap_tris = cap_tris ? cap_tris * 2 : 4096; tris = realloc(tris, cap_tris * 3 * sizeof *tris); }
    tris[3 * ntris] = *a; tris[3 * ntris + 1] = *b; tris[3 * ntris + 2] = *c; ntris++;
}

static void triangle(const Vtx *a, const Vtx *b, const Vtx *c) {
    static int ns; if (ns < 6 && getenv("PWR_SW_SAMPLE")) { ns++; fprintf(stderr, "sw: pos (%g %g %g %g) (%g %g %g %g)\n", a->pos.x, a->pos.y, a->pos.z, a->pos.w, b->pos.x, b->pos.y, b->pos.z, b->pos.w); }
    Vtx poly[2][12]; int n = 3, cur = 0;
    poly[0][0] = *a; poly[0][1] = *b; poly[0][2] = *c;
    for (int p = 0; p < 7 && n; p++) {
        int m = 0; Vtx *in = poly[cur], *out = poly[cur ^ 1];
        for (int i = 0; i < n && m < 11; i++) {
            const Vtx *s = &in[i], *e = &in[(i + 1) % n];
            float ds = plane_d(s, p), de = plane_d(e, p);
            if (ds >= 0) out[m++] = *s;
            if ((ds >= 0) != (de >= 0)) out[m++] = lerpv(s, e, ds / (ds - de));
        }
        n = m; cur ^= 1;
        if (!n) {
            g_sw_clip_plane[p]++;
            static int nr; if (nr < 8 && getenv("PWR_SW_SAMPLE")) { nr++; fprintf(stderr, "sw: rejected p%d (%g %g %g %g) (%g %g %g %g)\n", p, a->pos.x, a->pos.y, a->pos.z, a->pos.w, b->pos.x, b->pos.y, b->pos.z, b->pos.w); }
            break;
        }
    }
    g_sw_tri_in++;
    if (n < 3) return;
    g_sw_tri_drawn++;
    SV sv[12]; for (int i = 0; i < n; i++) sv[i] = to_screen(&poly[cur][i]);
    for (int i = 1; i + 1 < n; i++) emit_tri(&sv[0], &sv[i], &sv[i + 1]);
}

/* ---------------- worker pool ----------------
 * pica_sw_draw splits a draw's vertex shading and rasterization over workers. Rasterization is split by 8-row
 * bands, so every pixel is written by one worker in triangle order (depth test and blending stay ordered).
 * PWR_GPU_THREADS sets the worker count (default 4, counting the GX thread). */
typedef void (*JobFn)(int id, int n);
static int pool_n;
static JobFn pool_job;
static unsigned pool_gen, pool_left;
static pthread_mutex_t PM = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_start = PTHREAD_COND_INITIALIZER, pool_done = PTHREAD_COND_INITIALIZER;

static void *pool_worker(void *arg) {
    int id = (int)(intptr_t)arg; unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&PM);
        while (pool_gen == seen) pthread_cond_wait(&pool_start, &PM);
        seen = pool_gen; JobFn job = pool_job;
        pthread_mutex_unlock(&PM);
        job(id, pool_n);
        pthread_mutex_lock(&PM);
        if (--pool_left == 0) pthread_cond_signal(&pool_done);
        pthread_mutex_unlock(&PM);
    }
    return NULL;
}
static void pool_run(JobFn job) {
    if (!pool_n) {
        const char *e = getenv("PWR_GPU_THREADS"); pool_n = e ? atoi(e) : 4;
        if (pool_n < 1) pool_n = 1; if (pool_n > 32) pool_n = 32;
        for (int i = 1; i < pool_n; i++) { pthread_t t; pthread_create(&t, NULL, pool_worker, (void *)(intptr_t)i); pthread_detach(t); }
    }
    if (pool_n == 1) { job(0, 1); return; }
    pthread_mutex_lock(&PM);
    pool_job = job; pool_left = (unsigned)pool_n - 1; pool_gen++;
    pthread_cond_broadcast(&pool_start);
    pthread_mutex_unlock(&PM);
    job(0, pool_n);
    pthread_mutex_lock(&PM);
    while (pool_left) pthread_cond_wait(&pool_done, &PM);
    pthread_mutex_unlock(&PM);
}

/* ---------------- draw ---------------- */
/* vertex cache: slot = index (indexed draws) or position in the draw (arrays); each slot is shaded once per draw */
static Vtx vcache[65536];
static uint32_t vlist_index[65536], vlist_slot[65536], nvlist;   /* what to shade this draw */
static uint32_t vstamp[65536], draw_stamp;

static void job_shade(int id, int n) {
    for (uint32_t i = (uint32_t)id; i < nvlist; i += (uint32_t)n) vcache[vlist_slot[i]] = shade_vertex(vlist_index[i]);
}
static void job_raster(int id, int n) {
    for (size_t i = 0; i < ntris; i++) raster(&tris[3 * i], &tris[3 * i + 1], &tris[3 * i + 2], id, n);
}

/* PWR_DRAWDBG=<hex colour-buffer address>[,<hex vertex shader entry or ffffffff>[,<after seconds>[,<count>]]]: print the
   state of the first <count> (default 400) matching draws into that target (0 = any), and their first shaded vertices when they are
   shaded on the CPU (shaded != 0). PWR_DRAWDBG_BOX=1: one `box:` line per draw (needs PWR_VK_CPU_VS=1). PWR_DRAWDBG_TEX=<hex va>[-<hex va>]: only draws sampling a texture at that address (range) on unit 0, 1 or 2 */
static int force_dd;   /* PWR_SKIP_DUMP: print the draws PWR_SKIP selects (once, in one frame) instead of dropping them */
static void draw_debug(uint32_t n, int topo, const uint32_t *idx, int shaded) {
    static char *dd = (char *)1; static uint32_t dd_addr, dd_entry = ~0u; static double dd_after, dd_t0; static int left = 400;
    if (dd == (char *)1) {
        dd = getenv("PWR_DRAWDBG");
        if (dd) {
            char *e; dd_addr = (uint32_t)strtoul(dd, &e, 16);
            if (*e == ',') { dd_entry = (uint32_t)strtoul(e + 1, &e, 16); if (*e == ',') dd_after = strtod(e + 1, &e); if (*e == ',') left = atoi(e + 1); }
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); dd_t0 = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
        }
    }
    static long dd_tex = -2, dd_tex_hi;
    if (dd_tex == -2) { const char *e = getenv("PWR_DRAWDBG_TEX"); dd_tex = -1;
                        if (e) { dd_tex = strtol(e, NULL, 16); const char *d = strchr(e, '-'); dd_tex_hi = d ? strtol(d + 1, NULL, 16) : dd_tex + 1; } }
    #define IN_TEX(r) ((long)pa_to_va(R(r) << 3) >= dd_tex && (long)pa_to_va(R(r) << 3) < dd_tex_hi)
    int tex_ok = dd_tex < 0 || IN_TEX(0x085) || IN_TEX(0x095) || IN_TEX(0x09D);
    #undef IN_TEX
    int dd_hit = force_dd || (dd && left > 0 && (dd_addr == 0 || dd_addr == cbuf) && tex_ok && (dd_entry == ~0u || dd_entry == (R(0x2BA) & 0xffff)));
    if (dd_hit && dd_after > 0) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        dd_hit = (double)ts.tv_sec + ts.tv_nsec * 1e-9 - dd_t0 >= dd_after;
    }
    if (dd_hit && shaded && getenv("PWR_DRAWDBG_BOX")) {   /* one line per draw: screen box (x/w, y/w) of the shaded vertices */
        left--;
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, w0 = 1e30f, w1 = -1e30f; int fin = 0;
        for (uint32_t i = 0; i < n; i++) { Vtx *v = &vcache[idx[i]]; if (!(fabsf(v->pos.w) > 1e-6f)) continue;
            float sx = v->pos.x / v->pos.w, sy = v->pos.y / v->pos.w; fin++;
            if (sx < x0) x0 = sx; if (sx > x1) x1 = sx; if (sy < y0) y0 = sy; if (sy > y1) y1 = sy; if (v->pos.w < w0) w0 = v->pos.w; if (v->pos.w > w1) w1 = v->pos.w; }
        fprintf(stderr, "box: tgt=%08x entry=%x n=%u tex0=%08x box=[%.2f %.2f]-[%.2f %.2f] w=[%.1f %.1f] blend=%x depth=%x\n", cbuf, R(0x2BA) & 0xffff, n,
                pa_to_va(R(0x085) << 3), x0, y0, x1, y1, w0, w1, R(0x100), R(0x107));
        (void)fin;
        if (getenv("PWR_DRAWDBG_UNI")) {   /* PWR_DRAWDBG_UNI=<first>,<last>: float uniforms c<first>..c<last> of this draw */
            int u0 = 0, u1 = 0; sscanf(getenv("PWR_DRAWDBG_UNI"), "%d,%d", &u0, &u1);
            for (int i = u0; i <= u1 && i < 96; i++) fprintf(stderr, "box:   c%d=(%g %g %g %g)\n", i, cf[i].x, cf[i].y, cf[i].z, cf[i].w);
        }
        {   /* model-space bounds of attribute 0 when it is 3 or 4 floats (PWR_DRAWDBG_BOX): raw size of the mesh */
            uint32_t afmt = R(0x201), nsz = (afmt >> 2) & 3; float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
            uint32_t stride = (R(0x205) >> 16) & 0xff, off = R(0x203), vbase = vtx_va(off); int okf = (afmt & 3) == 3 && nsz >= 2 && stride;
            if (okf) for (uint32_t i = 0; i < nvlist; i++) { uint32_t vi = vlist_index[i]; for (int k = 0; k < 3; k++) { uint32_t w = rd32(vbase + vi * stride + 4 * (uint32_t)k); float v; memcpy(&v, &w, 4); if (v < mn[k]) mn[k] = v; if (v > mx[k]) mx[k] = v; } }
            fprintf(stderr, "box:   model x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f] stride=%u fmt=%x\n", mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], stride, afmt & 0xf);
        }
        return;
    }
    if (dd_hit) {
        left--;
        fprintf(stderr, "drawdbg: n=%u topo=%d nattr=%u entry=%x idx[0..2]=%u,%u,%u\n", n, topo, pica_num_attrs(), R(0x2BA) & 0xffff, idx[0], n > 1 ? idx[1] : 0, n > 2 ? idx[2] : 0);
        fprintf(stderr, "    tex1 dim=%x addr=%08x fmt=%x param=%x lod=%x  tex2 dim=%x addr=%08x fmt=%x param=%x lod=%x  t0 param=%x lod=%x\n",
                R(0x092), pa_to_va(R(0x095) << 3), R(0x096), R(0x093), R(0x094), R(0x09A), pa_to_va(R(0x09D) << 3), R(0x09E), R(0x09B), R(0x09C),
                R(0x083), R(0x084));
        fprintf(stderr, "    regs cull=%x vp=%x,%x scissor=%x tex=%x t0 dim=%x addr=%x fmt=%x blend=%x/%x alpha=%x stencil=%x depth=%x lighting=%x tev0=%x,%x,%x,%x dscale=%x dofs=%x wbuf=%x\n",
                R(0x040), R(0x041), R(0x043), R(0x065), R(0x080), R(0x082), R(0x085), R(0x08E), R(0x100), R(0x101), R(0x104), R(0x105),
                R(0x107), R(0x08F), R(0x0C0), R(0x0C1), R(0x0C2), R(0x0C3), R(0x04D), R(0x04E), R(0x06D));
        {   static const uint16_t tev[6] = { 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8 };
            fprintf(stderr, "    tev");
            for (int t = 0; t < 6; t++) fprintf(stderr, " [%x %x %x %x %x]", R(tev[t]), R(tev[t] + 1), R(tev[t] + 2), R(tev[t] + 3), R(tev[t] + 4));
            fprintf(stderr, " buf=%x lightcfg=%x,%x,%x,%x lutsel=%x,%x,%x nlights=%x\n", R(0x0E0), R(0x1C3), R(0x1C4), R(0x1C6), R(0x1D0), R(0x1D1), R(0x1D2), R(0x1D3), R(0x1C2));
        }
        fprintf(stderr, "    attr base=%x fmt=%x,%x", R(0x200), R(0x201), R(0x202));
        for (int b = 0; b < 4; b++) fprintf(stderr, " buf%d=[%x %x %x]", b, R(0x203 + 3 * b), R(0x204 + 3 * b), R(0x205 + 3 * b));
        fprintf(stderr, " ib=%x first=%x vsin=%x perm=%x,%x scissor=%x,%x vpxy=%x imm_attrs=%x fb=%x,%x,%x,%x\n", R(0x227), R(0x22A), R(0x2B9), R(0x2BB), R(0x2BC), R(0x066), R(0x067), R(0x068), R(0x242), R(0x11C), R(0x11D), R(0x11E), R(0x117));
        if (getenv("PWR_DRAWDBG_UNIL")) {   /* PWR_DRAWDBG_UNIL=<first>,<last>: those float uniforms on one line, for diffing runs */
            int u0 = 0, u1 = 0; sscanf(getenv("PWR_DRAWDBG_UNIL"), "%d,%d", &u0, &u1);
            fprintf(stderr, "    uni:"); for (int i = u0; i <= u1 && i < 96; i++) fprintf(stderr, " c%d=(%g %g %g %g)", i, cf[i].x, cf[i].y, cf[i].z, cf[i].w); fputc('\n', stderr);
        }
        if (getenv("PWR_DRAWDBG_RAW")) {   /* raw vertex data of buffer 0: guest address, FNV-1a hash of every vertex, first PWR_DRAWDBG_RAW vertices as words */
            uint32_t stride = (R(0x205) >> 16) & 0xff, vbase = vtx_va(R(0x203)), cnt = (uint32_t)atoi(getenv("PWR_DRAWDBG_RAW")), h = 2166136261u;
            if (stride) for (uint32_t i = 0; i < nvlist; i++) for (uint32_t b = 0; b < stride; b++) h = (h ^ rd8(vbase + vlist_index[i] * stride + b)) * 16777619u;
            fprintf(stderr, "    raw: vbase=%08x stride=%u nv=%u hash=%08x\n", vbase, stride, nvlist, h);
            for (uint32_t i = 0; stride && i < cnt && i < nvlist; i++) { fprintf(stderr, "    raw[%u]:", vlist_index[i]);
                for (uint32_t b = 0; b + 4 <= stride; b += 4) fprintf(stderr, " %08x", rd32(vbase + vlist_index[i] * stride + b)); fputc('\n', stderr); }
        }
        if (getenv("PWR_DRAWDBG_VS")) {   /* vertex shader words from the entry point and the first float uniforms */
            uint32_t e = R(0x2BA) & 0xffff;
            fprintf(stderr, "    vs@%x:", e); for (uint32_t i = 0; i < 12; i++) fprintf(stderr, " %08x", pica_vs_prog[(e + i) & 4095]);
            fprintf(stderr, "\n    swz10-17:"); for (int i = 0x10; i < 0x18; i++) fprintf(stderr, " %08x", pica_vs_prog[4096 + i]);
            fprintf(stderr, "\n    cf8-9: (%g %g %g %g) (%g %g %g %g)", cf[8].x, cf[8].y, cf[8].z, cf[8].w, cf[9].x, cf[9].y, cf[9].z, cf[9].w);
            fprintf(stderr, "\n    cf0-7:"); for (int i = 0; i < 8; i++) fprintf(stderr, " (%g %g %g %g)", cf[i].x, cf[i].y, cf[i].z, cf[i].w);
            fprintf(stderr, " outmap=%x,%x,%x outmask=%x\n", R(0x050), R(0x051), R(0x052), R(0x2BD));
        }
        for (uint32_t i = 0; shaded && i < 3 && i < n; i++) { Vtx *v = &vcache[idx[i]];
            fprintf(stderr, "    pos %g %g %g %g col %g %g %g %g uv %g %g\n", v->pos.x, v->pos.y, v->pos.z, v->pos.w, v->col.x, v->col.y, v->col.z, v->col.w, v->uv[0][0], v->uv[0][1]); }
    }
}

unsigned long long g_gs_draws;   /* draws with the geometry shader stage enabled (not implemented) */
void pica_sw_draw(int indexed) {
    {   /* PWR_SKIP=<first>-<last>: drop those draws of every game frame (counted from each top-screen swap), to find
           which draw makes an artifact by bisection */
        static int lo = -2, hi; static unsigned frame, n;
        if (lo == -2) { const char *e = getenv("PWR_SKIP"); lo = -1; if (e) { lo = atoi(e); const char *d = strchr(e, '-'); hi = d ? atoi(d + 1) : lo; } }
        if (lo >= 0) {
            extern unsigned g_gsp_top_swaps;
            if (frame != g_gsp_top_swaps) { frame = g_gsp_top_swaps; n = 0; }
            int i = (int)n++;
            static int dump = -1, dumped_frame = -1; if (dump < 0) dump = getenv("PWR_SKIP_DUMP") ? atoi(getenv("PWR_SKIP_DUMP")) : 0;
            force_dd = 0;
            if (i >= lo && i <= hi) {
                if (!dump) return;
                /* dump: the frame numbered PWR_SKIP_DUMP (top-screen swaps since boot) */
                if ((int)frame == dump || dumped_frame == (int)frame) { dumped_frame = (int)frame; force_dd = 1; fprintf(stderr, "skipdump: frame %u draw %d\n", frame, i); }
            }
        }
    }
    if (R(0x229) & 3) { g_gs_draws++; static int once; if (!once && getenv("PWR_VK_TRACE")) { once = 1; fprintf(stderr, "sw: geometry-shader draw: 0x229=%08x topo=%u n=%u\n", R(0x229), (R(0x25E) >> 8) & 3, R(0x228)); } }
    fb_w = R(0x11E) & 0x7ff; fb_h = ((R(0x11E) >> 12) & 0x3ff) + 1;
    cbuf = pa_to_va((R(0x11D) & 0x0fffffff) << 3); dbuf = pa_to_va((R(0x11C) & 0x0fffffff) << 3);
    cfmt = (int)((R(0x117) >> 16) & 7); dfmt = (int)(R(0x116) & 3);
    if (cfmt > 4) cfmt = 0;
    if (!cbuf || !fb_w || !fb_h) {
        static int once; if (!once) { once = 1; fprintf(stderr, "pica: draw dropped, bad target 0x11D=%08x 0x11E=%08x\n", R(0x11D), R(0x11E)); }
        return;
    }
    cull_mode = (int)(R(0x040) & 3);
    uint32_t n = imm_active ? imm_n : R(0x228), first = imm_active ? 0 : R(0x22A), ib = R(0x227);
    uint32_t ibase = vtx_va(ib & 0x0fffffff);
    int idx16 = (int)(ib >> 31), topo = (int)((R(0x25E) >> 8) & 3);
    if (n > 65536) return;
    static uint32_t idx[65536];   /* cache slot of each vertex in draw order */
    if (++draw_stamp == 0) { memset(vstamp, 0, sizeof vstamp); draw_stamp = 1; }
    nvlist = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = indexed ? (idx16 ? rd16(ibase + 2 * i) : rd8(ibase + i)) : first + i, slot = indexed ? v : i;
        idx[i] = slot;
        if (vstamp[slot] != draw_stamp) { vstamp[slot] = draw_stamp; vlist_index[nvlist] = v; vlist_slot[nvlist++] = slot; }
    }
    if (pica_vk_enabled() && pica_vk_gpu_vs()) { draw_debug(n, topo, idx, 0); pica_vk_draw(NULL, vlist_index, vlist_slot, nvlist, idx, n, topo); return; }
    if (nvlist < 128) job_shade(0, 1); else pool_run(job_shade);   /* waking the pool costs more than a small draw */
    draw_debug(n, topo, idx, 1);
    if (pica_vk_enabled()) { pica_vk_draw(vcache, vlist_index, vlist_slot, nvlist, idx, n, topo); return; }
    Vtx *vb = vcache;
    ntris = 0;
    #define V(i) (&vb[idx[i]])
    if (topo == 1) { for (uint32_t i = 2; i < n; i++) { if (i & 1) triangle(V(i - 1), V(i - 2), V(i)); else triangle(V(i - 2), V(i - 1), V(i)); } }
    else if (topo == 2) { for (uint32_t i = 2; i < n; i++) triangle(V(0), V(i - 1), V(i)); }
    else { for (uint32_t i = 0; i + 2 < n; i += 3) triangle(V(i), V(i + 1), V(i + 2)); }
    #undef V
    if (ntris) pool_run(job_raster);
}

/* ---- save states: the GPU's registers and what was uploaded through them (between command lists) ---- */
void pica_state_save(SBuf *b) {
    sb_put(b, pica_regs, sizeof(uint32_t) * 0x300);
    sb_put(b, pica_vs_prog, sizeof pica_vs_prog); sb_u32(b, prog_ptr); sb_u32(b, swz_ptr);
    sb_put(b, pica_vs_cf, sizeof pica_vs_cf); sb_u32(b, uni_idx); sb_put(b, uni_buf, sizeof uni_buf); sb_u32(b, uni_n);
    sb_put(b, fixed_attr, sizeof fixed_attr); sb_u32(b, fixed_idx); sb_put(b, fixed_buf, sizeof fixed_buf); sb_u32(b, fixed_n);
    sb_put(b, light_lut, sizeof light_lut); sb_put(b, fog_lut, sizeof fog_lut); sb_u32(b, lut_idx); sb_u32(b, lut_type); sb_u32(b, fog_idx);
}
void pica_state_load(SRd *r) {
    sr_get(r, pica_regs, sizeof(uint32_t) * 0x300);
    sr_get(r, pica_vs_prog, sizeof pica_vs_prog); prog_ptr = sr_u32(r); swz_ptr = sr_u32(r);
    sr_get(r, pica_vs_cf, sizeof pica_vs_cf); uni_idx = sr_u32(r); sr_get(r, uni_buf, sizeof uni_buf); uni_n = sr_u32(r);
    sr_get(r, fixed_attr, sizeof fixed_attr); fixed_idx = sr_u32(r); sr_get(r, fixed_buf, sizeof fixed_buf); fixed_n = sr_u32(r);
    sr_get(r, light_lut, sizeof light_lut); sr_get(r, fog_lut, sizeof fog_lut); lut_idx = sr_u32(r); lut_type = sr_u32(r); fog_idx = sr_u32(r);
    imm_n = 0;
    pica_vs_version++; light_lut_version++;   /* uploaded again, and the vertex shaders looked up again */
}
