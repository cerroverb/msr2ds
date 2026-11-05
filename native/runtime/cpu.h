/* Guest CPU state for Samus Returns' recompiled code (src/core/arm/msr_aot/aot_runtime.h, MsrAot::Ctx), seen from C.
 * The layout is MsrAot::Ctx's field for field; dispatch.cpp checks it at compile time. The recompiled code itself comes
 * from the module the launcher builds from the player's dump (msr_aot.dll / libmsr_aot.so, tools/msr/recomp), or
 * one built by tools/msr/recomp/build_module.py.
 * Guest memory is one 4 GiB host reservation: guest address a lives at g_mem + a. */
#ifndef MSR_NATIVE_CPU_H
#define MSR_NATIVE_CPU_H
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define CPU_BOOL bool
#else
#define CPU_BOOL _Bool
#endif

typedef struct MemoryCallbacks {
    void *user;
    uint8_t (*read8)(void *, uint32_t);
    uint16_t (*read16)(void *, uint32_t);
    uint32_t (*read32)(void *, uint32_t);
    void (*write8)(void *, uint32_t, uint8_t);
    void (*write16)(void *, uint32_t, uint16_t);
    void (*write32)(void *, uint32_t, uint32_t);
    CPU_BOOL (*write_exclusive32)(void *, uint32_t, uint32_t, uint32_t);
    CPU_BOOL (*write_exclusive64)(void *, uint32_t, uint64_t, uint64_t);
} MemoryCallbacks;

typedef struct CPU {
    uint32_t r[16];
    uint32_t n, z, cf, v;     /* flags, 0 or 1 (Ctx::c is cf here) */
    uint32_t q;
    uint32_t ge;
    uint32_t cpsr_rest;       /* mode and mask bits (0x10: user) */
    uint32_t thumb;
    uint32_t s[32];           /* VFP S0-S31 */
    uint32_t fpscr;
    uint32_t tls;             /* CP15 c13 user read-only thread register: the TLS block (Ctx::tls_ro) */
    uint32_t tls_rw;
    uint64_t ticks;           /* cycles run in this slice */
    uint64_t budget;          /* yield at loop heads once ticks reach this */
    uint32_t exit;            /* MsrAot::Exit */
    uint32_t svc;
    uint32_t excl_valid, excl_addr;
    uint64_t excl_value;
    uint8_t *const *pages;    /* 4 KiB page -> host pointer */
    MemoryCallbacks mem;
} CPU;

enum { EXIT_BRANCH = 0, EXIT_SVC = 1, EXIT_INTERPRET = 2, EXIT_PATCHED = 3 };

extern uint8_t *g_mem;

static inline uint32_t rd8(uint32_t a)  { return g_mem[a]; }
static inline uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static inline void wr8(uint32_t a, uint32_t v)  { g_mem[a] = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(g_mem + a, &x, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }

/* dispatch.cpp: the recompiled code */
void rt_set_entries_file(const char *path);    /* where code the module lacks is listed (build_module.py --extra) */
void rt_quiet_range(uint32_t lo, uint32_t hi);
int rt_add_call_hook(uint32_t pc, void (*fn)(CPU *c));   /* another one: up to 32 (0 when full) */
void rt_set_call_hook(uint32_t pc, void (*fn)(CPU *c));   /* fn runs on entry to the ARM function at pc, before it */  /* code placed there on purpose: interpreted without being listed */
int rt_interpret_step(CPU *c);                 /* interp_full.c: one ARM/Thumb instruction; 1 = SVC, -1 = unknown */
int rt_module_load(const char *path);          /* 0 when loaded and it fits this runtime */
int rt_has_code(uint32_t pc);                  /* an entry point for pc (bit 0: Thumb) */
/* a game mod changed the code at base (as the dump had it, as the mod left it): the recompiled functions covering the
   changed words are turned off, so the fallback interpreter runs them */
void rt_code_changed(uint32_t base, const uint8_t *original, uint32_t original_size, const uint8_t *modded, uint32_t modded_size);
/* instructions rewritten while the game runs that the module does not check: their functions run on the interpreter */
void rt_code_patchable(const uint32_t *words, uint32_t count);
void rt_cpu_init(CPU *c, uint32_t entry);      /* user mode, RunFast FPSCR, memory, pc (bit 0: Thumb) */
void rt_run(CPU *c);                           /* runs until the guest thread returns to EXIT_MAGIC */
void rt_svc(CPU *c, uint32_t num, uint32_t pc);   /* kernel.c */
int rt_interpret_one(CPU *c);                  /* interp.c: the ARM instruction at r15; 0 when it ran */
void k_slice_end(void);                        /* kernel.c: a time slice ran out (let other threads run) */
int k_preempt_take(void);                      /* kernel.c: a higher-priority thread asked to run (end the slice now) */
int k_abandoned(void);                         /* kernel.c: a save state is loading: leave rt_run now */
void k_nest(int d);                            /* kernel.c: rt_run entered (+1) or left (-1) on this thread */
#define EXIT_MAGIC 0xFFFFFFF0u

#ifdef __cplusplus
}
#endif
#endif
