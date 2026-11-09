#version 450
// PICA200 vertex shader interpreter on the GPU. The CPU fetches each vertex's attributes (pica_sw.c fetch_attrs);
// this shader runs the guest shader bytecode for one vertex and applies the output map, then hands the same
// varyings as pica.vert to pica.frag. Semantics mirror vs_run() in pica_sw.c; flow control uses an explicit stack
// (as the hardware does) instead of recursion.
// Also OpenGL's (pica_gl.c, GLSL 3.30): its first lines define VARYING empty and CLIP_Z (pica.vert), and the program and
// the attributes are texture buffers there.
#ifndef VARYING
#define VARYING(n) layout(location = n)
#endif
#ifdef VULKAN
layout(set = 0, binding = 5, std140) uniform VsDraw {
#else
layout(std140) uniform VsDraw {
#endif
    vec4 c[96];          // float uniforms
    uvec4 ib;            // x = bool uniforms (0x2B0), y = entry point, z = output mask (0x2BD), w = output count (0x04F)
    uvec4 iu;            // integer uniforms 0x2B1-0x2B4
    uvec4 perm;          // x/y = input permutation (0x2BB/0x2BC), z = number of attributes, w = first attribute vec4 of this draw
    uvec4 omap[2];       // output map registers 0x050-0x056
    uvec4 fa_[12];       // pica_vsgen.c's attribute fetch (unused here)
    vec4 fdef_[12];
    vec4 xf;             // clip-space scale of x and y (pica_vk.c: Samus Returns' widescreen and zoom)
} d;
#ifdef VULKAN
layout(set = 0, binding = 6, std430) readonly buffer Prog { uint prog[4096]; uint swz[128]; };
layout(set = 0, binding = 7, std430) readonly buffer Attrs { vec4 attrs[]; };
#define PROG(i) prog[i]
#define SWZ(i) swz[i]
#define ATTR(i) attrs[i]
#else
uniform usamplerBuffer prog_buf;   // pica_vs_prog: the program, then the swizzles
uniform samplerBuffer attr_buf;    // the draw's attributes, from vec4 d.perm.w on
#define PROG(i) texelFetch(prog_buf, int(i)).r
#define SWZ(i) texelFetch(prog_buf, 4096 + int(i)).r
#define ATTR(i) texelFetch(attr_buf, int(i))
#endif

VARYING(0) out vec4 v_col;
VARYING(1) out vec4 v_uv01;
VARYING(2) out vec3 v_uv2tw;
VARYING(3) out vec4 v_quat;
VARYING(4) out vec3 v_view;

vec4 vin[16], vout[16], tmp[16];
int a0x, a0y, aL;
bool cx, cy;

float pmul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }   // PICA: 0 * inf = 0
vec4 pmul4(vec4 a, vec4 b) { return vec4(pmul(a.x, b.x), pmul(a.y, b.y), pmul(a.z, b.z), pmul(a.w, b.w)); }

vec4 src_reg(uint idx, uint sel, bool neg, uint rel) {
    vec4 v;
    if (idx < 0x10u) v = vin[idx];
    else if (idx < 0x20u) v = tmp[idx - 0x10u];
    else {
        int ci = int(idx) - 0x20 + (rel == 1u ? a0x : rel == 2u ? a0y : rel == 3u ? aL : 0);
        v = (ci >= 0 && ci < 96) ? d.c[ci] : vec4(1);
    }
    vec4 o = vec4(v[(sel >> 6) & 3u], v[(sel >> 4) & 3u], v[(sel >> 2) & 3u], v[sel & 3u]);
    return neg ? -o : o;
}
void dst_write(uint dst, uint mask, vec4 v) {
    if (dst < 0x10u) {
        if ((mask & 8u) != 0u) vout[dst].x = v.x;
        if ((mask & 4u) != 0u) vout[dst].y = v.y;
        if ((mask & 2u) != 0u) vout[dst].z = v.z;
        if ((mask & 1u) != 0u) vout[dst].w = v.w;
    } else {
        uint t = (dst - 0x10u) & 15u;
        if ((mask & 8u) != 0u) tmp[t].x = v.x;
        if ((mask & 4u) != 0u) tmp[t].y = v.y;
        if ((mask & 2u) != 0u) tmp[t].z = v.z;
        if ((mask & 1u) != 0u) tmp[t].w = v.w;
    }
}
bool cond_ok(uint ins) {
    bool refx = ((ins >> 25) & 1u) != 0u, refy = ((ins >> 24) & 1u) != 0u;
    uint op = (ins >> 22) & 3u;
    bool x = cx == refx, y = cy == refy;
    return op == 0u ? (x || y) : op == 1u ? (x && y) : op == 2u ? x : y;
}
bool cmp_op(float a, float b, uint op) {
    if (op == 0u) return a == b; if (op == 1u) return a != b; if (op == 2u) return a < b;
    if (op == 3u) return a <= b; if (op == 4u) return a > b; if (op == 5u) return a >= b;
    return false;
}
bool bool_uniform(uint ins) { return ((d.ib.x >> ((ins >> 22) & 15u)) & 1u) != 0u; }

// flow-control stack: run [pc, final) then continue at ret; loops repeat 'repeat' more times from 'start'
const int STACK = 16;
uint st_final[STACK], st_ret[STACK], st_start[STACK];
int st_repeat[STACK], st_inc[STACK];
bool st_loop[STACK];
int sp = 0;
void push(uint start, uint final_pc, uint ret, int repeat, int inc, bool is_loop) {
    if (sp >= STACK) return;
    st_start[sp] = start; st_final[sp] = final_pc; st_ret[sp] = ret; st_repeat[sp] = repeat; st_inc[sp] = inc; st_loop[sp] = is_loop; sp++;
}

void run() {
    uint pc = d.ib.y & 0xffffu;
    for (int steps = 0; steps < 65536; steps++) {
        // returning from a call / if-branch / loop body
        bool popped = true;
        while (popped && sp > 0) {
            popped = false;
            if (pc == st_final[sp - 1]) {
                int t = sp - 1;
                if (st_loop[t]) {
                    aL += st_inc[t];
                    if (st_repeat[t] > 0) { st_repeat[t]--; pc = st_start[t]; }
                    else { pc = st_ret[t]; sp--; popped = true; }
                } else { pc = st_ret[t]; sp--; popped = true; }
            }
        }
        if (pc >= 4096u) return;
        uint ins = PROG(pc), op = ins >> 26;
        if (op < 0x20u || op >= 0x2eu) {
            if (op >= 0x30u) {   // MAD / MADI
                uint dsc = SWZ(ins & 0x1fu), dst = (ins >> 24) & 0x1fu, rel = (ins >> 22) & 3u;
                bool madi = op < 0x38u;
                uint s1 = (ins >> 17) & 0x1fu, s2, s3;
                if (madi) { s2 = (ins >> 12) & 0x1fu; s3 = (ins >> 5) & 0x7fu; } else { s2 = (ins >> 10) & 0x7fu; s3 = (ins >> 5) & 0x1fu; }
                vec4 a = src_reg(s1, (dsc >> 5) & 0xffu, ((dsc >> 4) & 1u) != 0u, 0u);
                vec4 b = src_reg(s2, (dsc >> 14) & 0xffu, ((dsc >> 13) & 1u) != 0u, madi ? 0u : rel);
                vec4 c = src_reg(s3, (dsc >> 23) & 0xffu, ((dsc >> 22) & 1u) != 0u, madi ? rel : 0u);
                dst_write(dst, dsc & 0xfu, pmul4(a, b) + c);
                pc++; continue;
            }
            uint dsc = SWZ(ins & 0x7fu), dst = (ins >> 21) & 0x1fu, rel = (ins >> 19) & 3u;
            bool inv = op >= 0x18u && op <= 0x1bu;
            uint s1 = inv ? (ins >> 14) & 0x1fu : (ins >> 12) & 0x7fu, s2 = inv ? (ins >> 7) & 0x7fu : (ins >> 7) & 0x1fu;
            vec4 a = src_reg(s1, (dsc >> 5) & 0xffu, ((dsc >> 4) & 1u) != 0u, inv ? 0u : rel);
            vec4 b = src_reg(s2, (dsc >> 14) & 0xffu, ((dsc >> 13) & 1u) != 0u, inv ? rel : 0u);
            vec4 r = a; float t;
            bool write = true;
            switch (op) {
            case 0x00u: r = a + b; break;
            case 0x01u: t = pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z); r = vec4(t); break;
            case 0x02u: t = pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z) + pmul(a.w, b.w); r = vec4(t); break;
            case 0x03u: case 0x18u: t = pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z) + b.w; r = vec4(t); break;
            case 0x04u: case 0x19u: r = vec4(1.0, pmul(a.y, b.y), a.z, b.w); break;
            case 0x05u: r = vec4(exp2(a.x)); break;
            case 0x06u: r = vec4(log2(a.x)); break;
            case 0x07u: r = vec4(max(a.x, 0.0), max(a.y, 0.0), a.z, clamp(a.w, -127.9961, 127.9961)); cx = a.x >= 0.0; cy = a.y >= 0.0; break;
            case 0x08u: r = pmul4(a, b); break;
            case 0x09u: case 0x1au: r = vec4(greaterThanEqual(a, b)); break;
            case 0x0au: case 0x1bu: r = vec4(lessThan(a, b)); break;
            case 0x0bu: r = floor(a); break;
            case 0x0cu: r = vec4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w); break;
            case 0x0du: r = vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); break;
            case 0x0eu: r = vec4(1.0 / a.x); break;
            case 0x0fu: r = vec4(inversesqrt(a.x)); break;
            case 0x12u: if ((dsc & 8u) != 0u) a0x = int(a.x); if ((dsc & 4u) != 0u) a0y = int(a.y); write = false; break;
            case 0x13u: break;
            case 0x2eu: case 0x2fu: cx = cmp_op(a.x, b.x, (ins >> 24) & 7u); cy = cmp_op(a.y, b.y, (ins >> 21) & 7u); write = false; break;
            default: write = false; break;
            }
            if (write) dst_write(dst, dsc & 0xfu, r);
            pc++; continue;
        }
        uint dst = (ins >> 10) & 0xfffu, num = ins & 0xffu;
        switch (op) {
        case 0x21u: pc++; break;                                      // NOP
        case 0x22u: return;                                           // END
        case 0x20u: case 0x23u: {                                     // BREAK / BREAKC: leave the innermost loop
            if (op == 0x23u && !cond_ok(ins)) { pc++; break; }
            while (sp > 0 && !st_loop[sp - 1]) sp--;
            if (sp == 0) return;
            pc = st_ret[sp - 1]; sp--; break;
        }
        case 0x24u: case 0x25u: case 0x26u: {                         // CALL / CALLC / CALLU
            bool take = op == 0x24u ? true : op == 0x25u ? cond_ok(ins) : bool_uniform(ins);
            if (take) { push(dst, dst + num, pc + 1u, 0, 0, false); pc = dst; } else pc++;
            break;
        }
        case 0x27u: case 0x28u: {                                     // IFU / IFC: then = [pc+1, dst), else = [dst, dst+num)
            bool c = op == 0x27u ? bool_uniform(ins) : cond_ok(ins);
            if (c) { push(pc + 1u, dst, dst + num, 0, 0, false); pc = pc + 1u; }
            else { push(dst, dst + num, dst + num, 0, 0, false); pc = dst; }
            break;
        }
        case 0x29u: {                                                 // LOOP: aL = init; count+1 iterations of [pc+1, dst]
            uint iu = d.iu[(ins >> 22) & 3u];
            int count = int(iu & 0xffu), step = int(iu << 8) >> 24;
            aL = int((iu >> 8) & 0xffu);
            push(pc + 1u, dst + 1u, dst + 1u, count, step, true); pc = pc + 1u;
            break;
        }
        case 0x2cu: if (cond_ok(ins)) pc = dst; else pc++; break;     // JMPC
        case 0x2du: if (bool_uniform(ins) != ((num & 1u) != 0u)) pc = dst; else pc++; break;   // JMPU
        default: pc++; break;                                         // EMIT / SETEMIT (geometry shader)
        }
    }
}

void main() {
    uint nattr = d.perm.z, base = d.perm.w + uint(gl_VertexIndex) * nattr;
    for (int i = 0; i < 16; i++) { vin[i] = vec4(0, 0, 0, 1); vout[i] = vec4(0); tmp[i] = vec4(0); }
    for (uint a = 0u; a < nattr && a < 12u; a++) {
        uint p = a < 8u ? (d.perm.x >> (4u * a)) & 15u : (d.perm.y >> (4u * (a - 8u))) & 15u;
        vin[p] = ATTR(base + a);
    }
    a0x = 0; a0y = 0; aL = 0; cx = false; cy = false;
    run();
    // output map: register 0x50+o holds the semantics of the o-th enabled output register's x, y, z, w
    float sem[24];
    for (int i = 0; i < 24; i++) sem[i] = 0.0;
    sem[3] = 1.0; sem[11] = 1.0;
    uint nout = d.ib.w & 7u, o = 0u;
    for (uint i = 0u; i < 16u && o < nout; i++) {
        if (((d.ib.z >> i) & 1u) == 0u) continue;
        uint m = d.omap[o >> 2][o & 3u];
        for (uint c = 0u; c < 4u; c++) { uint se = (m >> (8u * c)) & 0x1fu; if (se < 24u) sem[se] = vout[i][c]; }
        o++;
    }
#ifdef VULKAN
    gl_Position = vec4(sem[0] * d.xf.x, sem[1] * d.xf.y, -sem[2], sem[3]);
#else
    gl_Position = vec4(sem[0] * d.xf.x, sem[1] * d.xf.y, CLIP_Z(-sem[2], sem[3]), sem[3]);
#endif
    v_col = min(abs(vec4(sem[8], sem[9], sem[10], sem[11])), vec4(1.0));
    v_uv01 = vec4(sem[12], sem[13], sem[14], sem[15]);
    v_uv2tw = vec3(sem[22], sem[23], sem[16]);
    v_quat = vec4(sem[4], sem[5], sem[6], sem[7]);
    v_view = vec3(sem[18], sem[19], sem[20]);
}
