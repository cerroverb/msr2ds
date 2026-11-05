/* Save states (state.c): the guest's memory and every part of the runtime that keeps state, in sections. The game's threads
 * are stopped at points where their whole state is their registers (kernel.c k_freeze); a thread blocked in a wait is
 * saved as the wait, issued again when the state is loaded. */
#ifndef MSR_NATIVE_STATE_H
#define MSR_NATIVE_STATE_H
#include <stddef.h>
#include <stdint.h>
#include "kernel.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t *p; size_t n, cap; } SBuf;            /* growing write buffer */
typedef struct { const uint8_t *p; size_t n, off; int bad; } SRd;   /* reader; past the end gives zeros and sets bad */
void sb_put(SBuf *b, const void *d, size_t n);
void sr_get(SRd *r, void *d, size_t n);
#define SB(b, v) sb_put((b), &(v), sizeof(v))
#define SR(r, v) sr_get((r), &(v), sizeof(v))
static inline void sb_u32(SBuf *b, uint32_t v) { sb_put(b, &v, 4); }
static inline uint32_t sr_u32(SRd *r) { uint32_t v; sr_get(r, &v, 4); return v; }

/* kernel.c */
int  k_freeze(int timeout_ms);       /* every guest thread stopped where its state is complete; 0 on success */
void k_thaw(void);                   /* after a save: they go on */
int  k_abandon_all(int timeout_ms);  /* for a load: every guest thread leaves what it was doing and waits for its new state */
uint32_t ks_id(KObject *o);          /* while saving: an object's number in the state (0 = none) */
KObject *ks_obj(uint32_t id);        /* while loading */
void k_state_save(SBuf *b);          /* handles, threads, timers, the memory map, the clock */
void k_state_save_objects(SBuf *b);  /* last: every object the other sections named */
int  k_state_mem_fixups(uint32_t *addr, int max);   /* words the memory copy gets 1 added to (decrement-and-wait arbiters) */
void k_state_load_objects(SRd *r);   /* first */
int  k_state_load(SRd *r);
void k_state_resume(void);           /* the threads take their saved states and go on */
const char *k_session_name(KObject *o);

/* the other parts: save under their own locks; load while every guest thread is parked */
int  gsp_quiesce(int timeout_ms);    /* the GX worker idle with nothing queued; G held until gsp_release */
void gsp_release(void);
void gsp_state_save(SBuf *b);
void gsp_state_load(SRd *r);
void pica_state_save(SBuf *b);       /* pica_sw.c: registers, shader program, uniforms, lookup tables */
void pica_state_load(SRd *r);
void pica_vk_state_reset(void);      /* pica_vk.c: every render target and copy re-made from guest memory */
void dsp_pause(int on);
void dsp_state_save(SBuf *b);
void dsp_state_load(SRd *r);
void services_state_save(SBuf *b);
void services_state_load(SRd *r);
void fs_state_save(SBuf *b);
void fs_state_load(SRd *r);
int  fs_session_save(KObject *o, SBuf *b);       /* an fs:file or fs:dir session's file: 1 if it was one */
void fs_session_load(KObject *o, const char *name, SRd *r);
void msr_cheats_reset(void);         /* msr/cheats.cpp: forget the game objects it found */

#ifdef __cplusplus
}
#endif
#endif
