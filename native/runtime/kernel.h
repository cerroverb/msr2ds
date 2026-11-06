/* HLE kernel for the recompiled game: handles, sync objects, threads, memory, IPC sessions. */
#ifndef PWR_KERNEL_H
#define PWR_KERNEL_H
#include "cpu.h"

/* guest memory map (old-3DS application layout) */
#define VA_HEAP        0x08000000u
#define VA_STACK_TOP   0x10000000u
#define VA_LINEAR      0x14000000u
#define VA_VRAM        0x1F000000u
#define VA_DSPRAM      0x1FF00000u
#define VA_CONFIG      0x1FF80000u   /* kernel config page (read-only on hardware) */
#define VA_SHARED      0x1FF81000u   /* shared page */
#define VA_TLS         0x1FF82000u   /* 0x200 per thread */
#define TLS_CMDBUF     0x80u

#define H_CUR_THREAD   0xFFFF8000u
#define H_CUR_PROCESS  0xFFFF8001u

#define RES_OK         0u
#define RES_TIMEOUT    0x09401BFEu
#define RES_BAD_HANDLE 0xD8E007F7u

typedef enum { KO_NONE, KO_EVENT, KO_SEMAPHORE, KO_MUTEX, KO_ARBITER, KO_THREAD, KO_SESSION, KO_SHMEM, KO_PROCESS, KO_RESLIMIT, KO_TIMER } KType;

struct Service;
typedef struct KObject {
    KType type;
    int refs;
    /* event / timer */
    int signaled, reset_type;               /* 0 one-shot, 1 sticky, 2 pulse */
    /* semaphore */
    int32_t count, max;
    /* mutex */
    struct KThread *owner; int lock_count;
    /* thread */
    struct KThread *thread;
    /* session */
    const struct Service *svc; void *svc_state;
    /* shared memory */
    uint32_t addr, size;
} KObject;

typedef struct KThread {
    CPU cpu;
    int index;
    uint32_t handle;      /* handle of its KO_THREAD object */
    KObject *obj;
    int finished;
    int prio;             /* 0 = highest, as on the 3DS */
    int ready;            /* runnable and waiting for the run token */
    int in_svc; uint32_t svc_num, svc_r[6], svc_lr, svc_cmd;   /* the svc it is in (k_hang_report) */
    volatile int preempt; /* a higher-priority thread became ready while this one ran guest code (k_preempt_take) */
    /* save states (state.h): abandon = a state is loading, leave rt_run and park (thread_body); dead = the state has no such
       thread, end; parked = waiting for the new state; spawn = a new host thread is needed; nest = rt_run depth (a nested
       call is never stopped in); arb = its ArbWaiter while in an arbiter wait; resume_svc = after a load, the wait to issue
       again (svc number + 1) */
    volatile int abandon; int dead, parked, spawn, nest; void *arb; uint32_t resume_svc;
} KThread;

typedef struct Service {
    const char *name;
    void (*handle)(KThread *t, uint32_t *cmd, KObject *session);   /* cmd = host pointer to the TLS command buffer */
} Service;

void k_init(void);
void k_add_region(uint32_t base, uint32_t size, uint32_t perm, uint32_t state);   /* for svcQueryMemory */
uint32_t k_region_state(uint32_t addr);                                            /* 0: not in a region */
int k_region_list(uint32_t *base, uint32_t *size, uint32_t *state, int max);
extern int g_main_prio;                        /* from the exheader (ARM11 local caps) */
uint32_t k_new_handle(KObject *o);
KObject *k_obj(uint32_t h);
KObject *k_alloc(KType t);
KThread *k_current(void);
void k_signal(KObject *o);                    /* event signal (wakes waiters) */
uint32_t k_session_for(const char *name);     /* new session handle to a service, 0 if unknown */
int k_run_main(uint32_t entry);               /* run the main guest thread on this host thread */
volatile int *k_start_main(uint32_t entry);   /* ...or on a new host thread; returns its done flag */
void k_trace(const char *fmt, ...);
void k_hang_report(void);                     /* every guest thread and what it waits on (stderr) */
uint64_t k_ticks(void);                       /* ARM11 system ticks (268 MHz) since boot */
void k_sleep_ns(int64_t ns);                   /* sleep to within microseconds (svcSleepThread, the VBlank tick) */
void k_set_power_saving(int on);               /* k_sleep_ns waits on the timer alone, without its last-millisecond loop */
int  k_power_saving(void);
int  k_cpu_threads(void);                      /* the host's processor threads */
void k_background_thread(void);                /* lower the calling thread's priority: work no frame waits for */
extern int g_trace;

/* services.c */
const Service *svc_find(const char *name);
#endif
