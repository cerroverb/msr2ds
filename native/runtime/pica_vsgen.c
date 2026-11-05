/* PICA200 vertex shader -> GLSL -> SPIR-V, one specialized Vulkan shader module per guest program configuration.
 * Registers become variables, swizzles and operand indices are fixed, the attribute permutation and the output map
 * are baked in. Flow control: structured where it can be (2026-11-03, generate_structured: each range vs_run runs is a
 * function, IF/LOOP/CALL are if/for/calls), else the reachable code is split into basic blocks run by a switch(pc)
 * dispatcher, with an explicit call/if/loop stack (only emitted when the program uses one). Semantics mirror vs_run()
 * in pica_sw.c and shaders/pica_vs.vert (the interpreter used when no compiler is available).
 * GLSL is compiled with libshaderc, loaded at run time (no build dependency); the module cache is keyed by a hash of
 * the program, swizzles, entry point, permutation and output map.
 * Compiles run on background threads (2026-11-02: one took 250-360 ms with the optimizer, 11-22 ms without, and the
 * game froze for it the first time an effect such as a shot appeared): the GLSL is generated on the GX thread (it reads
 * the registers), the draw uses the interpreter until the module is ready. Unoptimized SPIR-V (level 0): the driver
 * optimizes it anyway. PWR_VSGEN_SYNC=1 compiles on the spot as before, PWR_VSGEN_OPT=<0-2> sets the level. */
#include "pica_int.h"
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#define dlopen(name, flags) ((void *)LoadLibraryA(name))
#define dlsym(h, name) ((void *)GetProcAddress((HMODULE)(h), name))
#else
#include <dlfcn.h>
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define R(i) pica_regs[i]

/* ---- libshaderc C API (subset), resolved with dlsym ---- */
typedef void *sc_compiler, *sc_options, *sc_result;
static sc_compiler (*sc_compiler_initialize)(void);
static sc_options (*sc_compile_options_initialize)(void);
static void (*sc_set_optimization_level)(sc_options, int);
static void (*sc_set_target_env)(sc_options, int, uint32_t);
static sc_result (*sc_compile_into_spv)(sc_compiler, const char *, size_t, int, const char *, const char *, sc_options);
static size_t (*sc_result_get_length)(sc_result);
static const char *(*sc_result_get_bytes)(sc_result);
static int (*sc_result_get_compilation_status)(sc_result);
static const char *(*sc_result_get_error_message)(sc_result);
static void (*sc_result_release)(sc_result);
static sc_compiler compiler; static sc_options options;
static int sc_state = -1;
static int vsgen_opt(void) {   /* shaderc's optimization level: 0 (none) unless PWR_VSGEN_OPT says otherwise */
    static int lv = -1;
    if (lv < 0) { const char *o = getenv("PWR_VSGEN_OPT"); lv = o ? atoi(o) : 0; if (lv < 0 || lv > 2) lv = 0; }
    return lv;
}

static int shaderc_load(void) {
    if (sc_state >= 0) return sc_state;
    sc_state = 0;
    if (getenv("PWR_VK_NO_VSGEN")) return 0;
#ifdef __APPLE__
    /* the app's own (Contents/Frameworks), else one the library path finds (a development build) */
    const char *names[] = { "@executable_path/../Frameworks/libshaderc_shared.1.dylib", "libshaderc_shared.1.dylib", "libshaderc_shared.dylib" };
#else
    const char *names[] = { "libshaderc.so.1", "libshaderc_shared.so.1", "libshaderc_shared.so", "libshaderc.so", "shaderc_shared.dll", "libshaderc_shared.dll" };
#endif
    void *h = NULL;
    for (size_t i = 0; i < sizeof names / sizeof *names && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "vsgen: libshaderc not found; vertex shaders use the GPU interpreter\n"); return 0; }
    #define SYM(v, n) if (!(*(void **)&v = dlsym(h, n))) { fprintf(stderr, "vsgen: libshaderc lacks %s\n", n); return 0; }
    SYM(sc_compiler_initialize, "shaderc_compiler_initialize");
    SYM(sc_compile_options_initialize, "shaderc_compile_options_initialize");
    SYM(sc_set_optimization_level, "shaderc_compile_options_set_optimization_level");
    SYM(sc_set_target_env, "shaderc_compile_options_set_target_env");
    SYM(sc_compile_into_spv, "shaderc_compile_into_spv");
    SYM(sc_result_get_length, "shaderc_result_get_length");
    SYM(sc_result_get_bytes, "shaderc_result_get_bytes");
    SYM(sc_result_get_compilation_status, "shaderc_result_get_compilation_status");
    SYM(sc_result_get_error_message, "shaderc_result_get_error_message");
    SYM(sc_result_release, "shaderc_result_release");
    #undef SYM
    compiler = sc_compiler_initialize(); options = sc_compile_options_initialize();
    if (!compiler || !options) return 0;
    sc_set_target_env(options, 0 /* vulkan */, (1u << 22) | (3u << 12) /* 1.3 */);
    sc_set_optimization_level(options, vsgen_opt());
    sc_state = 1;
    return 1;
}

/* ---- text buffer ---- */
static char *out; static size_t olen, ocap;
static void emit(const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt); int n = vsnprintf(out + olen, ocap - olen, fmt, ap); va_end(ap);
        if (n >= 0 && olen + (size_t)n < ocap) { olen += (size_t)n; return; }
        ocap = ocap ? ocap * 2 : 1 << 16; out = realloc(out, ocap);
    }
}

#define prog pica_vs_prog
#define swz (pica_vs_prog + 4096)

static const char *comp = "xyzw";
/* source operand expression */
static void src_expr(char *buf, size_t n, uint32_t idx, uint32_t sel, int neg, uint32_t rel) {
    char base[48];
    if (idx < 0x10) snprintf(base, sizeof base, "v%u", idx);
    else if (idx < 0x20) snprintf(base, sizeof base, "r%u", idx - 0x10);
    else if (rel) snprintf(base, sizeof base, "cr(%u + %s)", idx - 0x20, rel == 1 ? "a0x" : rel == 2 ? "a0y" : "aL");
    else snprintf(base, sizeof base, "d.c[%u]", idx - 0x20);
    snprintf(buf, n, "%s(%s.%c%c%c%c)", neg ? "-" : "", base, comp[(sel >> 6) & 3], comp[(sel >> 4) & 3], comp[(sel >> 2) & 3], comp[sel & 3]);
}
static void dst_assign(uint32_t dst, uint32_t mask, const char *val) {
    if (!(mask & 15)) return;
    char name[8]; if (dst < 0x10) snprintf(name, sizeof name, "o%u", dst); else snprintf(name, sizeof name, "r%u", (dst - 0x10) & 15);
    char m[5]; int k = 0;
    if (mask & 8) m[k++] = 'x'; if (mask & 4) m[k++] = 'y'; if (mask & 2) m[k++] = 'z'; if (mask & 1) m[k++] = 'w'; m[k] = 0;
    emit("    { vec4 t_ = %s; %s.%s = t_.%s; }\n", val, name, m, m);
}
static void cond_expr(char *buf, size_t n, uint32_t ins) {
    int refx = (ins >> 25) & 1, refy = (ins >> 24) & 1, op = (ins >> 22) & 3;
    char x[24], y[24]; snprintf(x, sizeof x, "(cx == %s)", refx ? "true" : "false"); snprintf(y, sizeof y, "(cy == %s)", refy ? "true" : "false");
    if (op == 0) snprintf(buf, n, "(%s || %s)", x, y);
    else if (op == 1) snprintf(buf, n, "(%s && %s)", x, y);
    else snprintf(buf, n, "%s", op == 2 ? x : y);
}
static const char *cmp_glsl[8] = { "==", "!=", "<", "<=", ">", ">=", "!=", "!=" };

/* one arithmetic instruction */
static void emit_arith(uint32_t ins) {
    uint32_t op = ins >> 26; char a[96], b[96], c[96], v[320];
    if (op >= 0x30) {
        uint32_t ds = swz[ins & 0x1f], dst = (ins >> 24) & 0x1f, rel = (ins >> 22) & 3; int madi = op < 0x38;
        uint32_t s1 = (ins >> 17) & 0x1f, s2, s3;
        if (madi) { s2 = (ins >> 12) & 0x1f; s3 = (ins >> 5) & 0x7f; } else { s2 = (ins >> 10) & 0x7f; s3 = (ins >> 5) & 0x1f; }
        src_expr(a, sizeof a, s1, (ds >> 5) & 0xff, (ds >> 4) & 1, 0);
        src_expr(b, sizeof b, s2, (ds >> 14) & 0xff, (ds >> 13) & 1, madi ? 0 : rel);
        src_expr(c, sizeof c, s3, (ds >> 23) & 0xff, (ds >> 22) & 1, madi ? rel : 0);
        snprintf(v, sizeof v, "pmul4(%s, %s) + %s", a, b, c);
        dst_assign(dst, ds & 0xf, v); return;
    }
    uint32_t ds = swz[ins & 0x7f], dst = (ins >> 21) & 0x1f, rel = (ins >> 19) & 3;
    int inv = op >= 0x18 && op <= 0x1b;
    uint32_t s1 = inv ? (ins >> 14) & 0x1f : (ins >> 12) & 0x7f, s2 = inv ? (ins >> 7) & 0x7f : (ins >> 7) & 0x1f;
    src_expr(a, sizeof a, s1, (ds >> 5) & 0xff, (ds >> 4) & 1, inv ? 0 : rel);
    src_expr(b, sizeof b, s2, (ds >> 14) & 0xff, (ds >> 13) & 1, inv ? rel : 0);
    switch (op) {
    case 0x00: snprintf(v, sizeof v, "(%s + %s)", a, b); break;
    case 0x01: snprintf(v, sizeof v, "vec4(dp3(%s, %s))", a, b); break;
    case 0x02: snprintf(v, sizeof v, "vec4(dp4(%s, %s))", a, b); break;
    case 0x03: case 0x18: snprintf(v, sizeof v, "vec4(dph(%s, %s))", a, b); break;
    case 0x04: case 0x19: snprintf(v, sizeof v, "dst_(%s, %s)", a, b); break;
    case 0x05: snprintf(v, sizeof v, "vec4(exp2((%s).x))", a); break;
    case 0x06: snprintf(v, sizeof v, "vec4(log2((%s).x))", a); break;
    case 0x07: emit("    { vec4 a_ = %s; cx = a_.x >= 0.0; cy = a_.y >= 0.0;\n", a); dst_assign(dst, ds & 0xf, "vec4(max(a_.x, 0.0), max(a_.y, 0.0), a_.z, clamp(a_.w, -127.9961, 127.9961))"); emit("    }\n"); return;
    case 0x08: snprintf(v, sizeof v, "pmul4(%s, %s)", a, b); break;
    case 0x09: case 0x1a: snprintf(v, sizeof v, "vec4(greaterThanEqual(%s, %s))", a, b); break;
    case 0x0a: case 0x1b: snprintf(v, sizeof v, "vec4(lessThan(%s, %s))", a, b); break;
    case 0x0b: snprintf(v, sizeof v, "floor(%s)", a); break;
    case 0x0c: snprintf(v, sizeof v, "max_(%s, %s)", a, b); break;
    case 0x0d: snprintf(v, sizeof v, "min_(%s, %s)", a, b); break;
    case 0x0e: snprintf(v, sizeof v, "vec4(1.0 / (%s).x)", a); break;
    case 0x0f: snprintf(v, sizeof v, "vec4(inversesqrt((%s).x))", a); break;
    case 0x12: emit("    { vec4 a_ = %s;%s%s }\n", a, (ds & 8) ? " a0x = int(a_.x);" : "", (ds & 4) ? " a0y = int(a_.y);" : ""); return;
    case 0x13: snprintf(v, sizeof v, "%s", a); break;
    case 0x2e: case 0x2f:
        emit("    { vec4 a_ = %s, b_ = %s; cx = a_.x %s b_.x; cy = a_.y %s b_.y; }\n", a, b, cmp_glsl[(ins >> 24) & 7], cmp_glsl[(ins >> 21) & 7]);
        if (((ins >> 24) & 7) >= 6) emit("    cx = false;\n");
        if (((ins >> 21) & 7) >= 6) emit("    cy = false;\n");
        return;
    default: return;
    }
    dst_assign(dst, ds & 0xf, v);
}

static int is_flow(uint32_t op) { return op >= 0x20 && op < 0x2e; }

/* the GLSL before main(): uniforms, the attribute fetch, PICA arithmetic */
static void emit_header(void) {
    emit("#version 450\n"
         "layout(set = 0, binding = 5, std140) uniform VsDraw { vec4 c[96]; uvec4 ib; uvec4 iu; uvec4 perm; uvec4 omap[2]; uvec4 fa[12]; vec4 fdef[12]; vec4 xf; } d;\n"
         "layout(set = 0, binding = 7, std430) readonly buffer Raw { uint raw[]; };\n"
         /* attribute a: d.fa[a] = (byte offset of vertex 0, stride, type | count << 4 | 0x100 when read from raw); else
            the default d.fdef[a]. Types as the PICA loaders: 0 s8, 1 u8, 2 s16, 3 f32 (pica_vk.c fills these) */
         "uint rb(uint a) { return (raw[a >> 2] >> ((a & 3u) * 8u)) & 0xffu; }\n"
         "vec4 fetch(uint a) {\n"
         "  uvec4 f = d.fa[a]; if ((f.z & 0x100u) == 0u) return d.fdef[a];\n"
         "  uint p = f.x + uint(gl_VertexIndex) * f.y, t = f.z & 3u, n = (f.z >> 4) & 7u; vec4 v = vec4(0, 0, 0, 1);\n"
         "  for (uint k = 0u; k < n; k++) {\n"
         "    if (t == 3u) v[k] = uintBitsToFloat(raw[(p >> 2) + k]);\n"
         "    else if (t == 2u) { uint q = p + 2u * k; v[k] = float(int(((raw[q >> 2] >> ((q & 2u) * 8u)) & 0xffffu) << 16) >> 16); }\n"
         "    else { uint b = rb(p + k); v[k] = t == 0u ? float(int(b << 24) >> 24) : float(b); }\n"
         "  }\n"
         "  return v;\n"
         "}\n"
         "layout(location = 0) out vec4 v_col; layout(location = 1) out vec4 v_uv01; layout(location = 2) out vec3 v_uv2tw;\n"
         "layout(location = 3) out vec4 v_quat; layout(location = 4) out vec3 v_view;\n"
         "float pmul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n"
         "vec4 pmul4(vec4 a, vec4 b) { return vec4(pmul(a.x, b.x), pmul(a.y, b.y), pmul(a.z, b.z), pmul(a.w, b.w)); }\n"
         "float dp3(vec4 a, vec4 b) { return pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z); }\n"
         "float dp4(vec4 a, vec4 b) { return dp3(a, b) + pmul(a.w, b.w); }\n"
         "float dph(vec4 a, vec4 b) { return dp3(a, b) + b.w; }\n"
         "vec4 dst_(vec4 a, vec4 b) { return vec4(1.0, pmul(a.y, b.y), a.z, b.w); }\n"
         "vec4 max_(vec4 a, vec4 b) { return vec4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w); }\n"
         "vec4 min_(vec4 a, vec4 b) { return vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n"
         "vec4 cr(int i) { return (i >= 0 && i < 96) ? d.c[i] : vec4(1.0); }\n"
         "bool bu(uint i) { return ((d.ib.x >> i) & 1u) != 0u; }\n");
}

/* gl_Position and the varyings from the output registers by the output map */
static void emit_outputs(void) {
    char sem[24][16];
    for (int i = 0; i < 24; i++) snprintf(sem[i], sizeof sem[i], "%s", i == 3 || i == 11 ? "1.0" : "0.0");
    uint32_t nout = R(0x04F) & 7, mask = R(0x2BD);
    for (uint32_t i = 0, o = 0; i < 16 && o < nout; i++) {
        if (!((mask >> i) & 1)) continue;
        uint32_t m = R(0x050 + o);
        for (int c = 0; c < 4; c++) { uint32_t se = (m >> (8 * c)) & 0x1f; if (se < 24) snprintf(sem[se], sizeof sem[se], "o%u.%c", i, comp[c]); }
        o++;
    }
    emit("  gl_Position = vec4((%s) * d.xf.x, (%s) * d.xf.y, -(%s), %s);\n", sem[0], sem[1], sem[2], sem[3]);   /* xf: pica_vk.c */
    emit("  v_col = min(abs(vec4(%s, %s, %s, %s)), vec4(1.0));\n", sem[8], sem[9], sem[10], sem[11]);
    emit("  v_uv01 = vec4(%s, %s, %s, %s);\n  v_uv2tw = vec3(%s, %s, %s);\n", sem[12], sem[13], sem[14], sem[15], sem[22], sem[23], sem[16]);
    emit("  v_quat = vec4(%s, %s, %s, %s);\n  v_view = vec3(%s, %s, %s);\n}\n", sem[4], sem[5], sem[6], sem[7], sem[18], sem[19], sem[20]);
}

/* builds GLSL for the current registers as a dispatcher over basic blocks; returns 0 on success */
static int generate_flat(void) {
    static uint8_t reach[4096], leader[4096];
    memset(reach, 0, sizeof reach); memset(leader, 0, sizeof leader);
    uint32_t entry = R(0x2BA) & 0xffff, work[8192]; int nw = 0, uses_stack = 0;
    if (entry >= 4096) return -1;
    leader[entry] = 1; work[nw++] = entry;
    #define ADD(x) do { uint32_t x_ = (x); if (x_ < 4096) { leader[x_] = 1; if (nw < 8192) work[nw++] = x_; } } while (0)
    #define LEAD(x) do { uint32_t x_ = (x); if (x_ < 4096) leader[x_] = 1; } while (0)
    while (nw) {
        uint32_t pc = work[--nw];
        while (pc < 4096 && !reach[pc]) {
            reach[pc] = 1;
            uint32_t ins = prog[pc], op = ins >> 26, dst = (ins >> 10) & 0xfff, num = ins & 0xff;
            if (!is_flow(op)) { pc++; continue; }
            if (op == 0x22) break;                                                   /* END */
            if (op == 0x21) { pc++; continue; }                                      /* NOP */
            uses_stack |= op == 0x24 || op == 0x25 || op == 0x26 || op == 0x27 || op == 0x28 || op == 0x29 || op == 0x20 || op == 0x23;
            if (op == 0x20) break;                                                   /* BREAK */
            if (op == 0x23) { ADD(pc + 1); break; }                                  /* BREAKC */
            if (op >= 0x24 && op <= 0x26) { ADD(dst); LEAD(dst + num); ADD(pc + 1); break; }
            if (op == 0x27 || op == 0x28) { ADD(pc + 1); LEAD(dst); ADD(dst); ADD(dst + num); break; }
            if (op == 0x29) { ADD(pc + 1); ADD(dst + 1); break; }
            if (op == 0x2c || op == 0x2d) { ADD(dst); ADD(pc + 1); break; }
            pc++;                                                                    /* EMIT/SETEMIT */
        }
    }
    #undef ADD
    #undef LEAD

    olen = 0;
    emit_header();
    emit("void main() {\n");
    uint32_t nattr = (uint32_t)pica_num_attrs();
    uint64_t perm = (uint64_t)R(0x2BB) | (uint64_t)R(0x2BC) << 32;
    for (int i = 0; i < 16; i++) emit("  vec4 v%d = vec4(0, 0, 0, 1), r%d = vec4(0), o%d = vec4(0);\n", i, i, i);
    for (uint32_t a = 0; a < nattr && a < 12; a++) emit("  v%u = fetch(%uu);\n", (uint32_t)(perm >> (4 * a)) & 15, a);
    emit("  int a0x = 0, a0y = 0, aL = 0; bool cx = false, cy = false;\n");
    if (uses_stack) emit("  uint st_final[16], st_ret[16], st_start[16]; int st_rep[16], st_inc[16]; bool st_loop[16]; int sp = 0;\n");
    emit("  uint pc = %uu;\n  for (int steps = 0; steps < 65536; steps++) {\n", entry);
    if (uses_stack)
        emit("    bool popped = true;\n"
             "    while (popped && sp > 0) { popped = false; if (pc == st_final[sp - 1]) { int t = sp - 1;\n"
             "      if (st_loop[t]) { aL += st_inc[t]; if (st_rep[t] > 0) { st_rep[t]--; pc = st_start[t]; } else { pc = st_ret[t]; sp--; popped = true; } }\n"
             "      else { pc = st_ret[t]; sp--; popped = true; } } }\n");
    emit("    if (pc >= 4096u) break;\n    switch (pc) {\n");
    #define PUSH(start, fin, ret, rep, inc, loop) emit("      if (sp < 16) { st_start[sp] = %uu; st_final[sp] = %uu; st_ret[sp] = %uu; st_rep[sp] = %s; st_inc[sp] = %s; st_loop[sp] = %s; sp++; }\n", \
                                                       (uint32_t)(start), (uint32_t)(fin), (uint32_t)(ret), rep, inc, loop)
    for (uint32_t pc = 0; pc < 4096; pc++) {
        if (!reach[pc] || !leader[pc]) continue;
        emit("    case %uu: {\n", pc);
        uint32_t p = pc;
        for (;;) {
            uint32_t ins = prog[p], op = ins >> 26, dst = (ins >> 10) & 0xfff, num = ins & 0xff;
            if (!is_flow(op) || op == 0x21 || op >= 0x2a && op <= 0x2b) {   /* arithmetic, NOP, EMIT/SETEMIT */
                if (!is_flow(op)) emit_arith(ins);
                p++;
                if (p >= 4096) { emit("      pc = 4096u; break;\n"); break; }
                if (leader[p]) { emit("      pc = %uu; break;\n", p); break; }
                continue;
            }
            char cnd[96];
            switch (op) {
            case 0x22: emit("      pc = 4096u; break;\n"); break;                                         /* END */
            case 0x20: case 0x23:                                                                           /* BREAK / BREAKC */
                if (op == 0x23) { cond_expr(cnd, sizeof cnd, ins); emit("      if (!%s) { pc = %uu; break; }\n", cnd, p + 1); }
                emit("      while (sp > 0 && !st_loop[sp - 1]) sp--;\n      if (sp == 0) { pc = 4096u; break; }\n      pc = st_ret[sp - 1]; sp--; break;\n");
                break;
            case 0x24: case 0x25: case 0x26:                                                                /* CALL / CALLC / CALLU */
                if (op == 0x24) snprintf(cnd, sizeof cnd, "true");
                else if (op == 0x25) cond_expr(cnd, sizeof cnd, ins);
                else snprintf(cnd, sizeof cnd, "bu(%uu)", (ins >> 22) & 15);
                emit("      if (%s) {\n", cnd); PUSH(dst, dst + num, p + 1, "0", "0", "false"); emit("      pc = %uu; } else pc = %uu;\n      break;\n", dst, p + 1);
                break;
            case 0x27: case 0x28:                                                                           /* IFU / IFC */
                if (op == 0x27) snprintf(cnd, sizeof cnd, "bu(%uu)", (ins >> 22) & 15); else cond_expr(cnd, sizeof cnd, ins);
                emit("      if (%s) {\n", cnd); PUSH(p + 1, dst, dst + num, "0", "0", "false"); emit("      pc = %uu; } else {\n", p + 1);
                PUSH(dst, dst + num, dst + num, "0", "0", "false"); emit("      pc = %uu; }\n      break;\n", dst);
                break;
            case 0x29: {                                                                                    /* LOOP */
                char rep[64], inc[64]; uint32_t i = (ins >> 22) & 3;
                snprintf(rep, sizeof rep, "int(d.iu[%u] & 0xffu)", i); snprintf(inc, sizeof inc, "(int(d.iu[%u] << 8) >> 24)", i);
                emit("      aL = int((d.iu[%u] >> 8) & 0xffu);\n", i);
                PUSH(p + 1, dst + 1, dst + 1, rep, inc, "true"); emit("      pc = %uu; break;\n", p + 1);
                break;
            }
            case 0x2c: cond_expr(cnd, sizeof cnd, ins); emit("      pc = %s ? %uu : %uu; break;\n", cnd, dst, p + 1); break;          /* JMPC */
            case 0x2d: emit("      pc = (bu(%uu) != %s) ? %uu : %uu; break;\n", (ins >> 22) & 15, (num & 1) ? "true" : "false", dst, p + 1); break;   /* JMPU */
            default: emit("      pc = %uu; break;\n", p + 1); break;
            }
            break;
        }
        emit("    }\n");
    }
    #undef PUSH
    emit("    default: pc = 4096u; break;\n    }\n    if (pc >= 4096u) break;\n  }\n");
    emit_outputs();
    return 0;
}

/* ---- the structured form (2026-11-03) ----
   The dispatcher (a loop over a switch of basic blocks, with an explicit stack) keeps every register live around one
   loop. On the Steam Deck's GPU (RADV, 8 CUs) the game's skinned and lit programs compiled to 20-46k instructions with
   all 256 VGPRs, 100 of them spilled to scratch: ~7 ms of a 60 Hz frame for ~20k vertices, on top of the ~4 ms the
   rest of the frame takes there (PWR_VK_CPU_VS=1 showed it). vs_run (pica_sw.c) is structured: each range it runs (a
   call, an IF's halves, a loop's body) is a frame that ends with 0 (the range ran out), 1 (END) or 2 (BREAK). Here each
   such range is a GLSL function with that result, IF/LOOP/CALL are if/for/calls and the registers are globals, which
   the driver inlines into straight code. Programs with jumps (JMPC/JMPU) or deeper nesting than vs_run allows keep
   the dispatcher. PWR_VSGEN_FLAT=1: the dispatcher for every program (A/B). */
#define MAX_REGIONS 512
static struct { uint16_t start, end; int8_t depth; } regions[MAX_REGIONS];
static int nregions;
static const char *sfail;   /* why a program keeps the dispatcher (PWR_VSGEN_TRACE) */
static int is_plain(uint32_t op) { return !is_flow(op) || op == 0x21 || op == 0x2a || op == 0x2b; }   /* arithmetic, NOP, EMIT/SETEMIT */
/* the ranges vs_run runs from [start, end) at this depth, into regions[]; -1 when the structured form does not apply */
static int region_scan(uint32_t start, uint32_t end, int depth) {
    if (start >= end || start >= 4096) return 0;   /* runs nothing: no function */
    if (depth > 8) { sfail = "nesting deeper than 8"; return -1; }   /* vs_run ends the program there */
    int i = 0;
    while (i < nregions && !(regions[i].start == start && regions[i].end == end)) i++;
    if (i < nregions) { if (regions[i].depth >= depth) return 0; regions[i].depth = (int8_t)depth; }   /* deeper: its calls too */
    else {
        if (nregions == MAX_REGIONS) { sfail = "too many ranges"; return -1; }
        nregions++; regions[i].start = (uint16_t)start; regions[i].end = (uint16_t)end; regions[i].depth = (int8_t)depth;
    }
    for (uint32_t pc = start; pc < end && pc < 4096; ) {
        uint32_t ins = prog[pc], op = ins >> 26, dst = (ins >> 10) & 0xfff, num = ins & 0xff;
        if (is_plain(op) || op == 0x23) { pc++; continue; }   /* BREAKC: a return */
        if (op == 0x22 || op == 0x20) break;                  /* END, BREAK: nothing after them runs */
        if (op >= 0x24 && op <= 0x26) { if (region_scan(dst, dst + num, depth + 1)) return -1; pc++; continue; }
        if (op == 0x27 || op == 0x28) {
            if (dst + num <= pc) { sfail = "an IF ending before itself"; return -1; }
            if (region_scan(pc + 1, dst, depth + 1) || region_scan(dst, dst + num, depth + 1)) return -1;
            pc = dst + num; continue;
        }
        if (op == 0x29) {
            if (dst + 1 <= pc) { sfail = "a LOOP ending before itself"; return -1; }
            if (region_scan(pc + 1, dst + 1, depth + 1)) return -1;
            pc = dst + 1; continue;
        }
        sfail = "jumps (JMPC/JMPU)"; return -1;
    }
    return 0;
}
/* the call of range [start, end) as an expression: its function, or 0 for an empty range */
static void region_call(char *buf, size_t n, uint32_t start, uint32_t end) {
    if (start >= end || start >= 4096) snprintf(buf, n, "0"); else snprintf(buf, n, "f%u_%u()", start, end);
}
/* one range's function body, as vs_run runs it */
static void region_body(uint32_t start, uint32_t end) {
    char cnd[96], a[32], b[32];
    for (uint32_t pc = start; pc < end && pc < 4096; ) {
        uint32_t ins = prog[pc], op = ins >> 26, dst = (ins >> 10) & 0xfff, num = ins & 0xff;
        if (is_plain(op)) { if (!is_flow(op)) emit_arith(ins); pc++; continue; }
        switch (op) {
        case 0x22: emit("    return 1;\n"); return;                                            /* END */
        case 0x20: emit("    return 2;\n"); return;                                            /* BREAK */
        case 0x23: cond_expr(cnd, sizeof cnd, ins); emit("    if (%s) return 2;\n", cnd); pc++; continue;   /* BREAKC */
        case 0x24: case 0x25: case 0x26:                         /* CALL / CALLC / CALLU: an END in it ends the program, a BREAK only it */
            if (op == 0x24) snprintf(cnd, sizeof cnd, "true");
            else if (op == 0x25) cond_expr(cnd, sizeof cnd, ins);
            else snprintf(cnd, sizeof cnd, "bu(%uu)", (ins >> 22) & 15);
            region_call(a, sizeof a, dst, dst + num);
            if (strcmp(a, "0")) emit("    if (%s) { if (%s == 1) return 1; }\n", cnd, a);
            pc++; continue;
        case 0x27: case 0x28:                                    /* IFU / IFC: [pc + 1, dst) or [dst, dst + num), whose END or BREAK goes up */
            if (op == 0x27) snprintf(cnd, sizeof cnd, "bu(%uu)", (ins >> 22) & 15); else cond_expr(cnd, sizeof cnd, ins);
            region_call(a, sizeof a, pc + 1, dst); region_call(b, sizeof b, dst, dst + num);
            emit("    { int r_ = 0; if (%s) r_ = %s; else r_ = %s; if (r_ != 0) return r_; }\n", cnd, a, b);
            pc = dst + num; continue;
        case 0x29:                                               /* LOOP i: aL = init; count + 1 runs of [pc + 1, dst] */
            region_call(a, sizeof a, pc + 1, dst + 1);
            emit("    { uint iu_ = d.iu[%u]; aL = int((iu_ >> 8) & 0xffu); int n_ = int(iu_ & 0xffu), s_ = int(iu_ << 8) >> 24;\n"
                 "      for (int k_ = 0; k_ <= n_; k_++) { int r_ = %s; if (r_ == 1) return 1; if (r_ == 2) break; aL += s_; } }\n",
                 (ins >> 22) & 3, a);
            pc = dst + 1; continue;
        default: return;   /* jumps: region_scan sent the program to the dispatcher */
        }
    }
    emit("    return 0;\n");
}
/* builds GLSL for the current registers in the structured form; returns 0 on success, -1 for the dispatcher */
static int generate_structured(void) {
    uint32_t entry = R(0x2BA) & 0xffff;
    if (entry >= 4096) { sfail = "entry point out of range"; return -1; }
    nregions = 0; sfail = NULL;
    if (region_scan(entry, 4096, 0)) return -1;
    olen = 0;
    emit_header();
    for (int i = 0; i < 16; i++) emit("vec4 v%d = vec4(0, 0, 0, 1), r%d = vec4(0), o%d = vec4(0);\n", i, i, i);
    emit("int a0x = 0, a0y = 0, aL = 0; bool cx = false, cy = false;\n");
    for (int i = 0; i < nregions; i++) emit("int f%u_%u();\n", regions[i].start, regions[i].end);
    for (int i = 0; i < nregions; i++) { emit("int f%u_%u() {\n", regions[i].start, regions[i].end); region_body(regions[i].start, regions[i].end); emit("}\n"); }
    emit("void main() {\n");
    uint32_t nattr = (uint32_t)pica_num_attrs();
    uint64_t perm = (uint64_t)R(0x2BB) | (uint64_t)R(0x2BC) << 32;
    for (uint32_t a = 0; a < nattr && a < 12; a++) emit("  v%u = fetch(%uu);\n", (uint32_t)(perm >> (4 * a)) & 15, a);
    emit("  f%u_4096();\n", entry);
    emit_outputs();
    return 0;
}
static int vsgen_flat(void) { static int f = -1; if (f < 0) f = getenv("PWR_VSGEN_FLAT") != NULL; return f; }
static const char *gen_form;   /* the form generate() chose (PWR_VSGEN_TRACE) */
static int generate(void) {
    if (!vsgen_flat() && !generate_structured()) { gen_form = "structured"; return 0; }
    gen_form = vsgen_flat() ? "dispatcher (PWR_VSGEN_FLAT)" : sfail ? sfail : "dispatcher";
    return generate_flat();
}

/* ---- cache ---- */
typedef struct { uint64_t key; VkShaderModule mod; uint32_t *spv; size_t spv_size; int pending; } Entry;   /* spv: kept for the shader seed */
static Entry *cache; static int ncache, cap;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;   /* the compile threads and the seed (pica_vk.c) use the cache too */
unsigned g_vsgen_compiled, g_vsgen_failed;
static void cache_add_locked(uint64_t key, VkShaderModule mod, const void *spv, size_t size, int pending) {
    if (ncache == cap) { cap = cap ? cap * 2 : 64; cache = realloc(cache, (size_t)cap * sizeof *cache); }
    Entry *e = &cache[ncache++];
    e->key = key; e->mod = mod; e->spv = NULL; e->spv_size = 0; e->pending = pending;
    if (mod && spv && size && (e->spv = malloc(size))) { memcpy(e->spv, spv, size); e->spv_size = size; }
}
static void cache_add(uint64_t key, VkShaderModule mod, const void *spv, size_t size) {
    pthread_mutex_lock(&cache_lock); cache_add_locked(key, mod, spv, size, 0); pthread_mutex_unlock(&cache_lock);
}
/* the shader seed (pica_vk.c): a generated shader's key and SPIR-V by its module; a module from saved SPIR-V */
int pica_vsgen_lookup(VkShaderModule mod, uint64_t *key) {
    int found = 0;
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < ncache && !found; i++) if (cache[i].mod && cache[i].mod == mod) { *key = cache[i].key; found = 1; }
    pthread_mutex_unlock(&cache_lock);
    return found;
}
int pica_vsgen_count(void) { pthread_mutex_lock(&cache_lock); int n = ncache; pthread_mutex_unlock(&cache_lock); return n; }
/* the generator's source (CMake hashes pica_vsgen.c) and the optimization level: saved SPIR-V from another generator is
   dropped (pwr_shaders.bin), a rebuild of anything else keeps it */
#ifndef MSR_VSGEN_SHA256
#define MSR_VSGEN_SHA256 __DATE__ " " __TIME__
#endif
const char *pica_vsgen_build(void) { static char b[48]; snprintf(b, sizeof b, "vsgen %.24s O%d%s", MSR_VSGEN_SHA256, vsgen_opt(), vsgen_flat() ? " flat" : ""); return b; }
int pica_vsgen_get(int i, uint64_t *key, const uint32_t **spv, size_t *size) {
    int ok = 0;
    pthread_mutex_lock(&cache_lock);
    if (i >= 0 && i < ncache && cache[i].spv) { *key = cache[i].key; *spv = cache[i].spv; *size = cache[i].spv_size; ok = 1; }   /* (spv is never freed) */
    pthread_mutex_unlock(&cache_lock);
    return ok;
}
VkShaderModule pica_vsgen_preload(VkDevice dev, uint64_t key, const uint32_t *spv, size_t size) {
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < ncache; i++) if (cache[i].key == key) { VkShaderModule m = cache[i].pending ? VK_NULL_HANDLE : cache[i].mod; pthread_mutex_unlock(&cache_lock); return m; }
    pthread_mutex_unlock(&cache_lock);
    if (!spv || !size) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = spv };
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, NULL, &mod)) return VK_NULL_HANDLE;
    cache_add(key, mod, spv, size);
    return mod;
}

/* ---- background compiles ---- */
typedef struct Job { uint64_t key; char *glsl; size_t len; struct Job *next; } Job;
static Job *jq_head, *jq_tail;
static pthread_mutex_t jq_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t jq_cond = PTHREAD_COND_INITIALIZER;
static VkDevice jq_dev;
static double vs_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6; }
static void *vs_worker(void *arg) {
    (void)arg;
    sc_compiler c = sc_compiler_initialize(); sc_options o = sc_compile_options_initialize();   /* this thread's own */
    if (!c || !o) return NULL;
    sc_set_target_env(o, 0 /* vulkan */, (1u << 22) | (3u << 12) /* 1.3 */);
    sc_set_optimization_level(o, vsgen_opt());
    static int trace = -1; if (trace < 0) trace = getenv("PWR_VSGEN_TRACE") != NULL;
    for (;;) {
        pthread_mutex_lock(&jq_lock);
        while (!jq_head) pthread_cond_wait(&jq_cond, &jq_lock);
        Job *j = jq_head; jq_head = j->next; if (!jq_head) jq_tail = NULL;
        pthread_mutex_unlock(&jq_lock);
        double t0 = vs_now_ms();
        sc_result r = sc_compile_into_spv(c, j->glsl, j->len, 0 /* vertex */, "pica_vs_gen.vert", "main", o);
        VkShaderModule mod = VK_NULL_HANDLE; const void *spv = NULL; size_t n = 0;
        if (r && sc_result_get_compilation_status(r) == 0) {
            spv = sc_result_get_bytes(r); n = sc_result_get_length(r);
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = (const uint32_t *)spv };
            if (vkCreateShaderModule(jq_dev, &ci, NULL, &mod)) mod = VK_NULL_HANDLE;
        } else {
            fprintf(stderr, "vsgen: compile failed: %s\n", r ? sc_result_get_error_message(r) : "(no result)");
            if (getenv("PWR_VK_TRACE")) fprintf(stderr, "%.*s\n", (int)j->len, j->glsl);
        }
        if (trace) fprintf(stderr, "vsgen: %016llx compiled in %.1f ms (background)\n", (unsigned long long)j->key, vs_now_ms() - t0);
        pthread_mutex_lock(&cache_lock);
        for (int i = 0; i < ncache; i++) if (cache[i].key == j->key && cache[i].pending) {
            Entry *e = &cache[i];
            e->mod = mod; e->pending = 0;
            if (mod && spv && n && (e->spv = malloc(n))) { memcpy(e->spv, spv, n); e->spv_size = n; }
            break;
        }
        if (mod) g_vsgen_compiled++; else g_vsgen_failed++;   /* failures stay: the interpreter */
        pthread_mutex_unlock(&cache_lock);
        if (mod) { extern void pica_vk_seed_dirty(void); pica_vk_seed_dirty(); }
        if (r) sc_result_release(r);
        free(j->glsl); free(j);
    }
    return NULL;
}
static void vs_enqueue(VkDevice dev, uint64_t key, const char *glsl, size_t len) {
    static int started;
    Job *j = malloc(sizeof *j); char *g = malloc(len ? len : 1);
    if (!j || !g) { free(j); free(g); return; }
    memcpy(g, glsl, len); j->key = key; j->glsl = g; j->len = len; j->next = NULL;
    pthread_mutex_lock(&jq_lock);
    if (!started) {   /* two threads: a new effect often brings several programs at once */
        jq_dev = dev; started = 1;
        for (int i = 0; i < 2; i++) { pthread_t th; if (!pthread_create(&th, NULL, vs_worker, NULL)) pthread_detach(th); }
    }
    if (jq_tail) jq_tail->next = j; else jq_head = j;
    jq_tail = j;
    pthread_cond_signal(&jq_cond); pthread_mutex_unlock(&jq_lock);
}

static uint32_t prog_hash_version = UINT32_MAX; static uint64_t prog_hash;
static uint64_t mix(uint64_t h, uint64_t v) { h ^= v; h *= 0x100000001B3ull; h ^= h >> 31; return h; }

/* specialized module for the current registers, VK_NULL_HANDLE when unavailable or still compiling (the caller uses
   the interpreter) */
VkShaderModule pica_vsgen_module(VkDevice dev) {
    if (!shaderc_load()) return VK_NULL_HANDLE;
    if (prog_hash_version != pica_vs_version) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (int i = 0; i < 4096 + 128; i++) h = mix(h, pica_vs_prog[i]);
        prog_hash = h; prog_hash_version = pica_vs_version;
    }
    uint64_t key = mix(mix(mix(mix(mix(prog_hash, R(0x2BA) & 0xffff), R(0x2BB)), R(0x2BC)), R(0x2BD)), (uint32_t)(pica_num_attrs() - 1) | (R(0x04F) & 7) << 8);
    for (int i = 0; i < 7; i++) key = mix(key, R(0x050 + i));
    static uint64_t last_key; static VkShaderModule last_mod; static int last_ok;   /* consecutive draws mostly share one */
    if (last_ok && key == last_key) return last_mod;
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < ncache; i++) if (cache[i].key == key) {
        VkShaderModule m = cache[i].mod; int pending = cache[i].pending;
        pthread_mutex_unlock(&cache_lock);
        if (pending) return VK_NULL_HANDLE;
        last_key = key; last_mod = m; last_ok = 1;
        return m;
    }
    pthread_mutex_unlock(&cache_lock);
    int gen_failed = generate();
    { static int trace = -1; if (trace < 0) trace = getenv("PWR_VSGEN_TRACE") != NULL;
      if (trace) fprintf(stderr, "vsgen: %016llx %s\n", (unsigned long long)key, gen_failed ? "not generated" : gen_form); }
    if (gen_failed) {
        pthread_mutex_lock(&cache_lock); g_vsgen_failed++; cache_add_locked(key, VK_NULL_HANDLE, NULL, 0, 0); pthread_mutex_unlock(&cache_lock);
        return VK_NULL_HANDLE;
    }
    static int sync = -1; if (sync < 0) sync = getenv("PWR_VSGEN_SYNC") != NULL;
    if (!sync) {
        pthread_mutex_lock(&cache_lock); cache_add_locked(key, VK_NULL_HANDLE, NULL, 0, 1); pthread_mutex_unlock(&cache_lock);
        vs_enqueue(dev, key, out, olen);
        return VK_NULL_HANDLE;
    }
    VkShaderModule mod = VK_NULL_HANDLE;
    sc_result r = sc_compile_into_spv(compiler, out, olen, 0 /* vertex */, "pica_vs_gen.vert", "main", options);
    if (r && sc_result_get_compilation_status(r) == 0) {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sc_result_get_length(r), .pCode = (const uint32_t *)sc_result_get_bytes(r) };
        if (vkCreateShaderModule(dev, &ci, NULL, &mod)) mod = VK_NULL_HANDLE;
        else { cache_add(key, mod, sc_result_get_bytes(r), sc_result_get_length(r)); g_vsgen_compiled++; sc_result_release(r); extern void pica_vk_seed_dirty(void); pica_vk_seed_dirty(); return mod; }
    } else {
        fprintf(stderr, "vsgen: compile failed: %s\n", r ? sc_result_get_error_message(r) : "(no result)");
        if (getenv("PWR_VK_TRACE")) fprintf(stderr, "%.*s\n", (int)olen, out);
    }
    if (r) sc_result_release(r);
    g_vsgen_failed++;
    cache_add(key, VK_NULL_HANDLE, NULL, 0);   /* failures are cached too (interpreter fallback) */
    return mod;
}
