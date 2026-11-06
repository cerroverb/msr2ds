/* HLE kernel (from Woohoo! Flight School, adapted to Samus Returns' dispatcher: each guest thread is a host thread
 * running rt_run). All kernel state is guarded by one lock. A blocked guest thread sleeps on its own condition variable
 * and is woken only by what it waits for (wake_obj, wake_ready, its arbiter address): with one shared condition variable
 * every wake-up woke all ~13 threads, which went back to sleep one after the other under the lock (~35 us each time,
 * dozens of times a frame, which kept 144 FPS near 100). Guest threads are host pthreads. */
#define _GNU_SOURCE
#include "kernel.h"
#include "runtime.h"
#include "state.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#endif

#define MAX_HANDLES 4096
#define MAX_THREADS 32
#define TICKS_PER_SEC 268111856ull

static pthread_mutex_t K = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  KC = PTHREAD_COND_INITIALIZER;
static KObject *handles[MAX_HANDLES];
static KThread *threads[MAX_THREADS];
static __thread KThread *cur;
static uint32_t heap_next = VA_HEAP, linear_next = VA_LINEAR;
static struct timespec t0;
int g_trace;

typedef struct { uint32_t addr; int wake; KThread *t; } ArbWaiter;
#define MAX_ARB 64
static ArbWaiter *arb_waiters[MAX_ARB];

/* per guest thread (by index): its condition variable, whether it sleeps on it, the objects it waits for */
static pthread_cond_t tcv[MAX_THREADS];
static uint8_t t_blocked[MAX_THREADS];
static KObject **t_wo[MAX_THREADS]; static int t_nwo[MAX_THREADS];
static pthread_cond_t TC = PTHREAD_COND_INITIALIZER;   /* the timer thread's */
static void wake_index(int i) { if (t_blocked[i]) pthread_cond_signal(&tcv[i]); }
static void wake_obj(KObject *o);
static void wake_ready(void);

void k_trace(const char *fmt, ...) {
    if (!g_trace) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[k%d] ", cur ? cur->index : -1); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

KThread *k_current(void) { return cur; }
int k_abandoned(void) { return cur && cur->abandon; }
void k_nest(int d) { if (cur) cur->nest += d; }

/* PWR_EVTRACE=<seconds>: from that run time on, the next 256 Ki events (svc in/out, token, GX) to evtrace.txt, for
   finding where a frame's time goes; recording one costs ~20 ns, unlike the PWR_TRACE_* lines */
#define EV_CAP (1u << 18)
static struct { uint64_t t; int16_t who; uint16_t code; uint32_t a, b; } *evs;
static uint32_t ev_n; static int ev_on = -1; static uint64_t ev_from;
static uint64_t now_ns(void);
void k_ev(int code, uint32_t a, uint32_t b) {
    if (ev_on == 0) return;
    if (ev_on < 0) { const char *e = getenv("PWR_EVTRACE"); ev_on = e != NULL; if (!e) return;
                     ev_from = (uint64_t)(atof(e) * 1e9); evs = calloc(EV_CAP, sizeof *evs); }
    uint64_t t = now_ns();
    if (t < ev_from) return;
    uint32_t i = __atomic_fetch_add(&ev_n, 1, __ATOMIC_RELAXED);
    if (i < EV_CAP) { evs[i].t = t; evs[i].who = (int16_t)(cur ? cur->index : -1); evs[i].code = (uint16_t)code; evs[i].a = a; evs[i].b = b; }
    if (i == EV_CAP - 1) {
        FILE *f = fopen("evtrace.txt", "w");
        for (uint32_t k = 0; f && k < EV_CAP; k++) fprintf(f, "%.6f %d %x %x %x\n", evs[k].t / 1e9, evs[k].who, evs[k].code, evs[k].a, evs[k].b);
        if (f) fclose(f);
        fprintf(stderr, "kernel: evtrace.txt written\n");
        ev_on = 0;
    }
}
int k_preempt_take(void) { return cur && __atomic_exchange_n(&cur->preempt, 0, __ATOMIC_ACQ_REL); }
int k_current_index(void) { return cur ? cur->index : -1; }

KObject *k_alloc(KType t) { KObject *o = calloc(1, sizeof *o); o->type = t; o->refs = 1; return o; }

uint32_t k_new_handle(KObject *o) {
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!handles[i]) { handles[i] = o; return 0x1000u + (uint32_t)i; }
    fprintf(stderr, "kernel: out of handles\n"); abort();
}

KObject *k_obj(uint32_t h) {
    if (h == H_CUR_THREAD) return cur ? cur->obj : NULL;
    if (h < 0x1001u || h >= 0x1000u + MAX_HANDLES) return NULL;
    return handles[h - 0x1000u];
}

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t.tv_nsec - t0.tv_nsec);
}
uint64_t k_ticks(void) { return (uint64_t)((__uint128_t)now_ns() * TICKS_PER_SEC / 1000000000ull); }

/* wait until deadline (ns since t0; UINT64_MAX = forever) or a wake-up; returns 0 on timeout. A guest thread sleeps on its
   own condition variable (KC only for a caller that is not one) */
static void give_token(KThread *self);
void k_ev(int code, uint32_t a, uint32_t b);
extern int g_k_frame_thread; extern volatile uint64_t g_k_frame_idle_ns;
static int kwait_until_(uint64_t deadline);
static int kwait_until(uint64_t deadline) {   /* the frame thread's time in it: main.c's load line */
    if (!cur || cur->index != g_k_frame_thread) return kwait_until_(deadline);
    const uint64_t w0 = now_ns(); const int r = kwait_until_(deadline);
    g_k_frame_idle_ns += now_ns() - w0;
    return r;
}
static int kwait_until_(uint64_t deadline) {
    if (cur) give_token(cur);   /* blocking: let another guest thread run */
    if (cur && cur->abandon) return 0;   /* a save state is loading */
    k_ev(6, 0, 0);
    pthread_cond_t *cv = cur ? &tcv[cur->index] : &KC;
    if (cur) t_blocked[cur->index] = 1;
    int r;
    if (deadline == UINT64_MAX) { pthread_cond_wait(cv, &K); r = 1; }
    else {
        uint64_t n = now_ns();
        if (n >= deadline) r = 0;
        else {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t add = deadline - n;
            ts.tv_sec += (time_t)(add / 1000000000ull); ts.tv_nsec += (long)(add % 1000000000ull);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            r = pthread_cond_timedwait(cv, &K, &ts) != ETIMEDOUT || now_ns() < deadline;
        }
    }
    if (cur) t_blocked[cur->index] = 0;
    if (cur && cur->abandon) r = 0;
    return r;
}
static uint64_t deadline_from(int64_t ns) { return ns < 0 ? UINT64_MAX : now_ns() + (uint64_t)ns; }

void k_signal(KObject *o) {
    pthread_mutex_lock(&K); o->signaled = 1; wake_obj(o); pthread_mutex_unlock(&K);
}

/* ---- scheduling: one guest thread runs at a time (the 3DS app core), highest priority first ----
 * running = thread allowed to execute guest code. A thread gives up the token when it blocks, sleeps or exits, and
 * at any svc when a strictly higher-priority thread is ready. All under K. */
static KThread *running;
int g_main_prio = 0x30;

static int best_ready(KThread *self) {
    for (int i = 0; i < MAX_THREADS; i++) { KThread *o = threads[i]; if (o && o != self && o->ready && o->prio < self->prio) return 0; }
    return 1;
}
static int freeze_req;
static void freeze_note(void);
static void give_token(KThread *self) { if (running == self) { running = NULL; wake_ready(); freeze_note(); } }
/* the threads waiting for the token (they sort out among themselves who takes it) */
static void wake_ready(void) { for (int i = 0; i < MAX_THREADS; i++) if (threads[i] && threads[i]->ready) wake_index(i); }
/* the threads waiting for o */
static void wake_obj(KObject *o) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!t_blocked[i] || !t_nwo[i]) continue;
        for (int k = 0; k < t_nwo[i]; k++) if (t_wo[i][k] == o) { pthread_cond_signal(&tcv[i]); break; }
    }
}
static uint64_t ks_wait_ns, ks_waits, ks_pre_ns, ks_pres;   /* PWR_GX_STATS: ready threads waiting for the token (k_stats) */
static void take_token(KThread *self) {
    if (running == self) return;
    self->ready = 1;
    /* a higher-priority thread that becomes ready (an interrupt's event, a timer) runs at the running thread's next loop
       head or function return, as on the 3DS, not only at its next svc or the end of its 1 ms slice */
    if (running && running->prio > self->prio) { running->preempt = 1; __atomic_store_n(&running->cpu.budget, 0, __ATOMIC_RELAXED); }
    uint64_t w0 = 0; int lower = 0;
    if (!(running == NULL && best_ready(self))) { w0 = now_ns(); lower = running && running->prio > self->prio; }
    if (w0) k_ev(4, running ? (uint32_t)running->index : 0xff, 0);
    while (!self->abandon && !(running == NULL && best_ready(self) && (!freeze_req || self->nest > 1))) {
        t_blocked[self->index] = 1; pthread_cond_wait(&tcv[self->index], &K); t_blocked[self->index] = 0;
    }
    if (w0) k_ev(5, 0, 0);
    if (w0) { uint64_t d = now_ns() - w0; if (lower) { ks_pre_ns += d; ks_pres++; } else { ks_wait_ns += d; ks_waits++; } }
    self->ready = 0;
    if (!self->abandon) running = self;
}
/* a guest thread about to wait for something outside the kernel (gsp.c: room in the GX queue) lets the others run meanwhile */
void k_wait_outside_begin(void) { if (!cur) return; pthread_mutex_lock(&K); give_token(cur); pthread_mutex_unlock(&K); }
void k_wait_outside_end(void) { if (!cur) return; pthread_mutex_lock(&K); take_token(cur); pthread_mutex_unlock(&K); }
void k_stats(void) {
    pthread_mutex_lock(&K);
    fprintf(stderr, "kernel: token waits behind a lower-priority thread %llu x %.3f ms, others %llu x %.3f ms\n",
            (unsigned long long)ks_pres, ks_pres ? ks_pre_ns / 1e6 / ks_pres : 0., (unsigned long long)ks_waits, ks_waits ? ks_wait_ns / 1e6 / ks_waits : 0.);
    ks_wait_ns = ks_waits = ks_pre_ns = ks_pres = 0;
    pthread_mutex_unlock(&K);
}
/* svc boundary: re-acquire the token, or hand it to a strictly higher-priority ready thread */
static void sched_point(KThread *self) {
    pthread_mutex_lock(&K);
    if (running == self) {
        if (freeze_req && self->nest <= 1) give_token(self);   /* a save state: stop here */
        else for (int i = 0; i < MAX_THREADS; i++) { KThread *o = threads[i];
            if (o && o != self && o->ready && o->prio < self->prio) { give_token(self); break; } }
    }
    take_token(self);
    pthread_mutex_unlock(&K);
}

/* dispatch.cpp: the running thread used up its time slice in guest code */
void k_slice_end(void) { if (cur) sched_point(cur); }

/* ---- waitable objects ---- */
static int ready(KObject *o, KThread *t) {
    switch (o->type) {
    case KO_EVENT: case KO_TIMER: return o->signaled;
    case KO_SEMAPHORE: return o->count > 0;
    case KO_MUTEX: return o->owner == NULL || o->owner == t;
    case KO_THREAD: return o->thread && o->thread->finished;
    default: return 0;
    }
}
static void acquire(KObject *o, KThread *t) {
    switch (o->type) {
    case KO_EVENT: case KO_TIMER: if (o->reset_type == 0) o->signaled = 0; break;
    case KO_SEMAPHORE: o->count--; break;
    case KO_MUTEX: o->owner = t; o->lock_count++; break;
    default: break;
    }
}

static uint32_t wait_n(KThread *t, uint32_t *hs, int n, int all, int64_t ns, int32_t *out_idx) {
    KObject *objs[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) { objs[i] = k_obj(hs[i]); if (!objs[i]) return RES_BAD_HANDLE; }
    uint64_t dl = deadline_from(ns);
    uint32_t res = RES_TIMEOUT;
    if (t) { t_wo[t->index] = objs; t_nwo[t->index] = n; }   /* wake_obj finds this thread through them */
    for (;;) {
        if (all) {
            int ok = 1; for (int i = 0; i < n; i++) ok &= ready(objs[i], t);
            if (ok) { for (int i = 0; i < n; i++) acquire(objs[i], t); *out_idx = 0; res = RES_OK; break; }
        } else {
            int got = -1;
            for (int i = 0; i < n && got < 0; i++) if (ready(objs[i], t)) { acquire(objs[i], t); got = i; }
            if (got >= 0) { *out_idx = got; res = RES_OK; break; }
        }
        if (ns == 0 || !kwait_until(dl)) { *out_idx = -1; break; }
    }
    if (t) { t_nwo[t->index] = 0; t_wo[t->index] = NULL; }
    return res;
}

/* ---- threads ---- */
typedef struct { KThread *t; } ThreadStart;

static KThread *new_thread(void) {
    for (int i = 0; i < MAX_THREADS; i++) if (!threads[i]) {
        KThread *t = calloc(1, sizeof *t); t->index = i; threads[i] = t;
        t->obj = k_alloc(KO_THREAD); t->obj->thread = t; t->handle = k_new_handle(t->obj);
        return t;
    }
    fprintf(stderr, "kernel: too many threads\n"); abort();
}

static void thread_finish(KThread *t) {
    pthread_mutex_lock(&K); t->finished = 1; give_token(t); wake_obj(t->obj); pthread_mutex_unlock(&K);
}

/* a guest thread's host thread: runs it; when a save state is loaded, parks, then runs the thread's saved state (a wait
   it was in is issued again). Returns 1 when the guest thread returned, 0 when the state has no such thread. */
static int freeze_parked(KThread *t);
static int thread_body(KThread *t) {
    for (;;) {
        pthread_mutex_lock(&K); take_token(t); pthread_mutex_unlock(&K);
        if (!t->abandon && t->resume_svc) {
            uint32_t n = t->resume_svc - 1; t->resume_svc = 0;
            rt_svc(&t->cpu, n, t->cpu.r[15] - (t->cpu.thumb ? 2u : 4u));
        }
        if (!t->abandon) rt_run(&t->cpu);
        if (!t->abandon) return 1;
        if (!freeze_parked(t)) return 0;
    }
}

static void *thread_main(void *p) {
    ThreadStart s = *(ThreadStart *)p; free(p);
    cur = s.t;
    k_trace("thread start entry=%08x arg=%08x sp=%08x", s.t->cpu.r[15], s.t->cpu.r[0], s.t->cpu.r[13]);
    if (thread_body(s.t)) { k_trace("thread returned"); thread_finish(s.t); }
    return NULL;
}
static void *thread_resume_main(void *p) {   /* a thread of a loaded state that had no host thread here */
    KThread *t = p; cur = t;
    if (thread_body(t)) thread_finish(t);
    return NULL;
}

/* ---- memory map (svcQueryMemory): what the game allocated or was given, under K ---- */
typedef struct { uint32_t base, size, perm, state; } Region;
#define MAX_REGIONS 256
static Region regions[MAX_REGIONS];
static int nregions;
void k_add_region(uint32_t base, uint32_t size, uint32_t perm, uint32_t state) {
    size = (size + 0xfff) & ~0xfffu;
    if (!size) return;
    for (int i = 0; i < nregions; i++)          /* grow an allocation that continues one */
        if (regions[i].base + regions[i].size == base && regions[i].state == state && regions[i].perm == perm) { regions[i].size += size; return; }
    if (nregions < MAX_REGIONS) regions[nregions++] = (Region){ base, size, perm, state };
}
/* the map for Samus Returns' cheats (msr/cheats.cpp): a region's state at an address (0: none), the regions */
uint32_t k_region_state(uint32_t addr) {
    uint32_t st = 0;
    pthread_mutex_lock(&K);
    for (int i = 0; i < nregions; i++) if (addr >= regions[i].base && addr - regions[i].base < regions[i].size) { st = regions[i].state; break; }
    pthread_mutex_unlock(&K);
    return st;
}
int k_region_list(uint32_t *base, uint32_t *size, uint32_t *state, int max) {
    pthread_mutex_lock(&K);
    int n = nregions < max ? nregions : max;
    for (int i = 0; i < n; i++) { base[i] = regions[i].base; size[i] = regions[i].size; state[i] = regions[i].state; }
    pthread_mutex_unlock(&K);
    return n;
}
static void query_memory(uint32_t addr, uint32_t *base, uint32_t *size, uint32_t *perm, uint32_t *state) {
    uint32_t lo = 0, hi = 0xFFFFFFFFu;
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        if (addr >= r->base && addr - r->base < r->size) { *base = r->base; *size = r->size; *perm = r->perm; *state = r->state; return; }
        if (r->base + r->size <= addr && r->base + r->size > lo) lo = r->base + r->size;
        if (r->base > addr && r->base - 1 < hi) hi = r->base - 1;
    }
    *base = lo; *size = hi - lo + 1; *perm = 0; *state = 0;   /* free */
}

/* ---- timers: one host thread fires them, under K ---- */
#define MAX_TIMERS 64
typedef struct { KObject *o; uint64_t due, interval; int armed; } Timer;
static Timer timers[MAX_TIMERS];
static pthread_t timer_thread;
static void *timer_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&K);
    for (;;) {
        uint64_t n = now_ns(), next = UINT64_MAX;
        for (int i = 0; i < MAX_TIMERS; i++) {
            Timer *tm = &timers[i];
            if (!tm->o || !tm->armed) continue;
            if (tm->due <= n) {
                tm->o->signaled = 1;
                if (tm->interval) { tm->due += tm->interval; if (tm->due < n) tm->due = n + tm->interval; }
                else tm->armed = 0;
                wake_obj(tm->o);
                if (tm->o->reset_type == 2) tm->o->signaled = 0;   /* pulse: wakes the current waiters only */
            }
            if (tm->armed && tm->due < next) next = tm->due;
        }
        /* not kwait_until: this host thread holds no run token */
        if (next == UINT64_MAX) { pthread_cond_wait(&TC, &K); continue; }
        uint64_t add = next - now_ns();
        if ((int64_t)add <= 0) continue;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(add / 1000000000ull); ts.tv_nsec += (long)(add % 1000000000ull);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&TC, &K, &ts);
    }
    return NULL;
}
static Timer *timer_for(KObject *o) {
    for (int i = 0; i < MAX_TIMERS; i++) if (timers[i].o == o) return &timers[i];
    return NULL;
}

/* A sleep to within microseconds. nanosleep on Windows wakes on the 1 ms system tick, often 1-2 ms late; the game's frame
   limiter sleeps until 16 ms (8, 6 ms) after a frame began, then waits for the VBlank at 16.7 ms (8.3, 6.9 ms): a late
   wake-up missed it and the frame took two (60 FPS ran at ~43). The bulk waits on a high-resolution timer, the last
   millisecond yields in a loop. Also the host's VBlank tick (main.c).
   Power-saving waits (k_set_power_saving): the whole wait is on the timer, which wakes within a few tenths of a
   millisecond; main.c gives the frame limit above 30 FPS a millisecond more to spare then, so the loop is not needed to
   meet the VBlank.
   On a processor with few threads the loop took them from the game's, and on a handheld or laptop the graphics' power. */
/* The game's frame thread, for main.c's 2 s load line: the guest thread whose svcSleepThread returns to the frame limiter
   (g_k_limiter_lr, main.c sets it), and its time blocked in a wait (kwait_until: the VBlank, the GPU, other threads) or
   asleep, the rest of each frame being its own work. Only that thread adds to it. */
uint32_t g_k_limiter_lr;
int g_k_frame_thread = -1;
volatile uint64_t g_k_frame_idle_ns;
static volatile int power_saving;
void k_set_power_saving(int on) { power_saving = on != 0; }
int k_power_saving(void) { return power_saving; }
void k_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    uint64_t end = now_ns() + (uint64_t)ns;
#ifdef _WIN32
    static __thread HANDLE timer; static __thread int tried;
    if (!tried) { tried = 1; timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS); }
    const int64_t loop = power_saving && timer ? 0 : 1000000;   /* without the high-resolution timer (before Windows 10 1803) Sleep is too coarse */
    if (ns > loop + 500000) {
        int64_t bulk = ns - loop;
        LARGE_INTEGER due; due.QuadPart = -(bulk / 100);
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep((DWORD)(bulk / 1000000));
    }
    while (now_ns() < end) SwitchToThread();
#else
    struct timespec ts = { (time_t)(ns / 1000000000), (long)(ns % 1000000000) };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
    (void)end;
#endif
}

int k_cpu_threads(void) {
#ifdef _WIN32
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (int)n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* Below the game's threads (its guest threads, the GX worker, the VBlank loop, the DSP clock): work no frame waits for, such
   as shader and pipeline compiles (draws use the generic ones meanwhile), texture pack decodes (the game's texture shows
   meanwhile) and cache saves. On a processor with threads to spare this changes nothing; on one with few, the game's
   threads run first. */
void k_background_thread(void) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);   /* a thread's own nice value on Linux */
#endif
}

/* ---- svc ---- */
static uint32_t get_tick_lo, get_tick_hi;
static void svc_body(CPU *c, uint32_t num, uint32_t pc);

void rt_svc(CPU *c, uint32_t num, uint32_t pc) {
    KThread *t = cur;
    if (t) { t->svc_num = num; t->svc_lr = c->r[14]; memcpy(t->svc_r, c->r, sizeof t->svc_r);
             t->svc_cmd = num == 0x32 ? rd32(c->tls + TLS_CMDBUF) : 0; t->in_svc = 1; }
    k_ev(1, num, c->r[14]);
    svc_body(c, num, pc);
    if (t) t->in_svc = 0;
    k_ev(2, num, 0);
    sched_point(cur);
    k_ev(3, num, 0);
}

/* printed by main.c when the game stops presenting frames: each guest thread, the svc it is blocked in and the state
   of the objects it waits on (racy reads, diagnostics only) */
void k_hang_report(void) {
    static const char *tn[] = { "none", "event", "semaphore", "mutex", "arbiter", "thread", "session", "shmem", "process", "reslimit", "timer" };
    fprintf(stderr, "hang: guest threads\n");
    for (int i = 0; i < MAX_THREADS; i++) {
        KThread *t = threads[i];
        if (!t) continue;
        fprintf(stderr, "hang:  k%d prio %d%s%s", t->index, t->prio, t->ready ? " ready" : "", t->finished ? " finished" : "");
        if (!t->in_svc) { fprintf(stderr, " running guest code (or between svcs) pc %08x lr %08x\n", t->cpu.r[15], t->cpu.r[14]); continue; }
        fprintf(stderr, " in svc %02x lr %08x r0-r5 %08x %08x %08x %08x %08x %08x", t->svc_num, t->svc_lr,
                t->svc_r[0], t->svc_r[1], t->svc_r[2], t->svc_r[3], t->svc_r[4], t->svc_r[5]);
        uint32_t hs[8]; int n = 0;
        if (t->svc_num == 0x24) hs[n++] = t->svc_r[0];
        if (t->svc_num == 0x25) for (int k = 0; k < (int)t->svc_r[2] && k < 8; k++) hs[n++] = rd32(t->svc_r[1] + 4u * (uint32_t)k);
        if (t->svc_num == 0x32) { KObject *o = k_obj(t->svc_r[0]); fprintf(stderr, " ipc %s cmd %08x", o && o->svc ? o->svc->name : "?", t->svc_cmd); }
        if (t->svc_num == 0x22) fprintf(stderr, " arbiter *addr=%d", (int32_t)rd32(t->svc_r[1]));
        for (int k = 0; k < n; k++) { KObject *o = k_obj(hs[k]);
            if (!o) { fprintf(stderr, " [%x bad]", hs[k]); continue; }
            fprintf(stderr, " [%x %s sig %d count %d%s]", hs[k], o->type < 11 ? tn[o->type] : "?", o->signaled, o->count,
                    o->type == KO_MUTEX && o->owner ? (o->owner->index == 0 ? " owner k0" : " owned") : ""); }
        fputc('\n', stderr);
    }
}

static void svc_body(CPU *c, uint32_t num, uint32_t pc) {
    KThread *t = cur;
    switch (num) {
    case 0x01: { /* ControlMemory: r0 op, r1 addr0, r2 addr1, r3 size, r4 perm -> r1 addr */
        uint32_t op = c->r[0], a0 = c->r[1], size = c->r[3];
        uint32_t base = op & 0xff, out = a0;
        pthread_mutex_lock(&K);
        if (base == 3) {               /* ALLOC */
            if (op & 0x10000) { out = linear_next; linear_next += (size + 0xfff) & ~0xfffu; k_add_region(out, size, 3, 7); }
            else if (!a0) { out = heap_next; heap_next += (size + 0xfff) & ~0xfffu; k_add_region(out, size, 3, 5); }
            else { if (a0 + size > heap_next) heap_next = a0 + size; k_add_region(a0, size, 3, 5); }
        } else if (base == 1) {        /* FREE: memory stays mapped; zero it so reuse looks fresh */
            if (a0 && size) memset(g_mem + a0, 0, size);
        }
        pthread_mutex_unlock(&K);
        k_trace("ControlMemory op=%x addr0=%08x size=%x -> %08x", op, a0, size, out);
        c->r[0] = RES_OK; c->r[1] = out; return;
    }
    case 0x02: { /* QueryMemory: r2 addr -> r1 base, r2 size, r3 perm, r4 state, r5 page flags */
        uint32_t base, size, perm, state;
        pthread_mutex_lock(&K); query_memory(c->r[2], &base, &size, &perm, &state); pthread_mutex_unlock(&K);
        k_trace("QueryMemory %08x -> %08x+%x perm %x state %x", c->r[2], base, size, perm, state);
        c->r[0] = RES_OK; c->r[1] = base; c->r[2] = size; c->r[3] = perm; c->r[4] = state; c->r[5] = 0; return;
    }
    case 0x03: k_trace("ExitProcess"); exit(0);
    case 0x08: { /* CreateThread: r0 prio, r1 entry, r2 arg, r3 stacktop, r4 cpu -> r1 handle */
        /* code without recompiled code (a game mod's) runs on the fallback interpreter (dispatch.cpp) */
        if (!rt_has_code(c->r[1])) fprintf(stderr, "kernel: thread at %08x: no recompiled code there, interpreted\n", c->r[1]);
        pthread_mutex_lock(&K); KThread *nt = new_thread(); pthread_mutex_unlock(&K);
        nt->prio = (int)c->r[0];
        rt_cpu_init(&nt->cpu, c->r[1]);
        nt->cpu.tls = VA_TLS + 0x200u * (uint32_t)nt->index;
        nt->cpu.r[0] = c->r[2]; nt->cpu.r[13] = c->r[3]; nt->cpu.r[14] = EXIT_MAGIC;
        ThreadStart *s = malloc(sizeof *s); s->t = nt;
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
        pthread_create(&th, &at, thread_main, s); pthread_detach(th);
        k_trace("CreateThread entry=%08x prio=%d cpu=%d lr=%08x -> %x", c->r[1], (int)c->r[0], (int)c->r[4], c->r[14], nt->handle);
        c->r[0] = RES_OK; c->r[1] = nt->handle; return;
    }
    case 0x09: k_trace("ExitThread"); thread_finish(t); pthread_exit(NULL);
    case 0x0A: { /* SleepThread(r0:r1 ns) */
        int64_t ns = (int64_t)(((uint64_t)c->r[1] << 32) | c->r[0]);
        static int sleep_trace = -1; if (sleep_trace < 0) sleep_trace = getenv("PWR_TRACE_SLEEP") != NULL;
        if (sleep_trace) fprintf(stderr, "sleep: [k%d] t=%.4f ns=%lld lr=%08x\n", t ? t->index : -1, k_ticks() / 268111856.0, (long long)ns, c->r[14]);
        if (t && g_k_limiter_lr && c->r[14] == g_k_limiter_lr) g_k_frame_thread = t->index;   /* the load line's frame thread */
        const uint64_t s0 = now_ns();
        pthread_mutex_lock(&K); give_token(t); pthread_mutex_unlock(&K);   /* sleep(0) = yield; sched_point re-takes it */
        if (ns <= 0) sched_yield();
        while (ns > 0 && !(t && t->abandon)) { int64_t piece = ns > 20000000 ? 20000000 : ns; k_sleep_ns(piece); ns -= piece; }   /* a load ends it */
        if (t && t->index == g_k_frame_thread) g_k_frame_idle_ns += now_ns() - s0;
        return;
    }
    case 0x0B: { /* GetThreadPriority: r1 handle -> r1 */
        KObject *o = k_obj(c->r[1]);
        c->r[0] = o && o->thread ? RES_OK : RES_BAD_HANDLE; c->r[1] = o && o->thread ? (uint32_t)o->thread->prio : 0; return;
    }
    case 0x0C: { /* SetThreadPriority: r0 handle, r1 prio */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[0]);
        if (o && o->thread) o->thread->prio = (int)c->r[1];
        wake_ready(); pthread_mutex_unlock(&K);
        c->r[0] = o && o->thread ? RES_OK : RES_BAD_HANDLE; return;
    }
    case 0x0D: case 0x0F: c->r[0] = RES_OK; c->r[1] = 0; return;   /* GetThreadAffinityMask / GetThreadIdealProcessor */
    case 0x0E: case 0x10: c->r[0] = RES_OK; return;                /* SetThreadAffinityMask / SetThreadIdealProcessor */
    case 0x11: c->r[0] = 0; return;                                /* GetCurrentProcessorNumber: the app core */
    case 0x13: { KObject *o = k_alloc(KO_MUTEX); pthread_mutex_lock(&K); if (c->r[1]) { o->owner = t; o->lock_count = 1; } c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x14: { /* ReleaseMutex */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[0]);
        if (o && o->type == KO_MUTEX && o->owner == t && --o->lock_count == 0) { o->owner = NULL; wake_obj(o); }
        pthread_mutex_unlock(&K); c->r[0] = o ? RES_OK : RES_BAD_HANDLE; return;
    }
    case 0x15: { KObject *o = k_alloc(KO_SEMAPHORE); o->count = (int32_t)c->r[1]; o->max = (int32_t)c->r[2];
        pthread_mutex_lock(&K); c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x16: { /* ReleaseSemaphore: r1 handle, r2 release -> r1 previous count */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[1]); uint32_t prev = 0;
        if (o && o->type == KO_SEMAPHORE) { prev = (uint32_t)o->count; o->count += (int32_t)c->r[2]; if (o->count > o->max) o->count = o->max; wake_obj(o); }
        pthread_mutex_unlock(&K); c->r[0] = o ? RES_OK : RES_BAD_HANDLE; c->r[1] = prev; return;
    }
    case 0x17: { KObject *o = k_alloc(KO_EVENT); o->reset_type = (int)c->r[1];
        pthread_mutex_lock(&K); c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x18: case 0x19: { /* SignalEvent / ClearEvent */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[0]);
        if (o) o->signaled = num == 0x18;
        if (o && num == 0x18) wake_obj(o);
        pthread_mutex_unlock(&K); c->r[0] = o ? RES_OK : RES_BAD_HANDLE; return;
    }
    case 0x1A: { /* CreateTimer: r1 reset type -> r1 handle */
        KObject *o = k_alloc(KO_TIMER); o->reset_type = (int)c->r[1];
        pthread_mutex_lock(&K);
        Timer *tm = timer_for(NULL);
        if (tm) { tm->o = o; tm->armed = 0; }
        if (!timer_thread) pthread_create(&timer_thread, NULL, timer_main, NULL);
        c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K);
        c->r[0] = tm ? RES_OK : 0xD8609013u; return;
    }
    case 0x1B: { /* SetTimer: r0 handle, initial ns r2:r3, interval ns r1:r4 */
        int64_t initial = (int64_t)(((uint64_t)c->r[3] << 32) | c->r[2]), interval = (int64_t)(((uint64_t)c->r[4] << 32) | c->r[1]);
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[0]); Timer *tm = o ? timer_for(o) : NULL;
        if (tm) { tm->due = now_ns() + (uint64_t)(initial > 0 ? initial : 0); tm->interval = interval > 0 ? (uint64_t)interval : 0; tm->armed = 1; o->signaled = 0; pthread_cond_signal(&TC); }
        pthread_mutex_unlock(&K);
        c->r[0] = tm ? RES_OK : RES_BAD_HANDLE; return;
    }
    case 0x1C: case 0x1D: { /* CancelTimer / ClearTimer: r0 handle */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[0]); Timer *tm = o ? timer_for(o) : NULL;
        if (tm && num == 0x1C) tm->armed = 0;
        if (o && num == 0x1D) o->signaled = 0;
        pthread_mutex_unlock(&K);
        c->r[0] = o ? RES_OK : RES_BAD_HANDLE; return;
    }
    case 0x1E: { /* CreateMemoryBlock: r0 other perm, r1 addr, r2 size, r3 my perm -> r1 handle */
        KObject *o = k_alloc(KO_SHMEM); o->addr = c->r[1]; o->size = c->r[2];
        pthread_mutex_lock(&K);
        if (!o->addr) { o->addr = 0x10000000u + 0x01000000u; }   /* not expected from an app; keep it out of the way */
        c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K);
        k_trace("CreateMemoryBlock addr=%08x size=%x -> %x", o->addr, o->size, c->r[1]);
        c->r[0] = RES_OK; return;
    }
    case 0x1F: { /* MapMemoryBlock: r0 handle, r1 addr, r2 myperm, r3 otherperm */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[0]);
        if (o && o->type == KO_SHMEM) {
            if (c->r[1] && c->r[1] != o->addr) { memcpy(g_mem + c->r[1], g_mem + o->addr, o->size); o->addr = c->r[1]; }
        }
        pthread_mutex_unlock(&K);
        k_trace("MapMemoryBlock h=%x addr=%08x", c->r[0], c->r[1]);
        c->r[0] = o ? RES_OK : RES_BAD_HANDLE; return;
    }
    case 0x20: c->r[0] = RES_OK; return;   /* UnmapMemoryBlock */
    case 0x21: { KObject *o = k_alloc(KO_ARBITER); pthread_mutex_lock(&K); c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x22: { /* ArbitrateAddress: r0 handle, r1 addr, r2 type, r3 value, r4:r5 ns */
        uint32_t addr = c->r[1], type = c->r[2]; int32_t val = (int32_t)c->r[3];
        int64_t ns = (int64_t)(((uint64_t)c->r[5] << 32) | c->r[4]);
        static uint32_t arb_watch = 1; if (arb_watch == 1) { const char *e = getenv("PWR_TRACE_ARB"); arb_watch = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (arb_watch && addr == arb_watch) fprintf(stderr, "arb: [k%d] type=%u value=%d *addr=%d count/ns=%lld lr=%08x\n", t ? t->index : -1, type, val, (int32_t)rd32(addr), (long long)ns, c->r[14]);
        static int ntrace; if (ntrace < 20) { ntrace++; k_trace("ArbitrateAddress h=%x addr=%08x type=%u value=%d *addr=%d lr=%08x", c->r[0], addr, type, val, (int32_t)rd32(addr), c->r[14]); }
        pthread_mutex_lock(&K);
        uint32_t res = RES_OK;
        if (type == 0) {            /* signal: wake up to val waiters (val < 0: all) */
            int woken = 0;
            for (int i = 0; i < MAX_ARB && (val < 0 || woken < val); i++)
                if (arb_waiters[i] && arb_waiters[i]->addr == addr && !arb_waiters[i]->wake) {
                    arb_waiters[i]->wake = 1; woken++;
                    if (arb_waiters[i]->t) wake_index(arb_waiters[i]->t->index);
                }
        } else {
            int32_t cur_v = (int32_t)rd32(addr);
            if (cur_v < val) {
                if (type == 2 || type == 4) wr32(addr, (uint32_t)(cur_v - 1));
                ArbWaiter w = { addr, 0, t }; int slot = -1;
                for (int i = 0; i < MAX_ARB; i++) if (!arb_waiters[i]) { arb_waiters[i] = &w; slot = i; break; }
                if (slot < 0) { fprintf(stderr, "kernel: arbiter waiter table full\n"); abort(); }
                if (t) t->arb = &w;
                uint64_t dl = (type >= 3) ? deadline_from(ns) : UINT64_MAX;
                static int arb_trace = -1; if (arb_trace < 0) arb_trace = getenv("PWR_TRACE_SLEEP") != NULL;
                double t0 = arb_trace ? k_ticks() / 268111856.0 : 0;
                while (!w.wake) if (!kwait_until(dl)) { res = RES_TIMEOUT; break; }
                arb_waiters[slot] = NULL;
                if (t) t->arb = NULL;
                if (arb_trace) fprintf(stderr, "arbw: [k%d] t=%.4f waited=%.4f addr=%08x type=%u lr=%08x\n", t ? t->index : -1, t0, k_ticks() / 268111856.0 - t0, addr, type, c->r[14]);
            }
        }
        pthread_mutex_unlock(&K);
        c->r[0] = res; return;
    }
    case 0x23: { pthread_mutex_lock(&K); uint32_t h = c->r[0];
        if (h > 0x1000u && h < 0x1000u + MAX_HANDLES) handles[h - 0x1000u] = NULL;
        pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x24: { /* WaitSynchronization1: r0 handle, r2:r3 ns */
        int32_t idx; uint32_t h = c->r[0]; int64_t ns = (int64_t)(((uint64_t)c->r[3] << 32) | c->r[2]);
        static int wait_trace = -1; if (wait_trace < 0) wait_trace = getenv("PWR_TRACE_SLEEP") != NULL;
        double t0 = wait_trace ? k_ticks() / 268111856.0 : 0;
        pthread_mutex_lock(&K); c->r[0] = wait_n(t, &h, 1, 0, ns, &idx); pthread_mutex_unlock(&K);
        if (wait_trace) fprintf(stderr, "wait1: [k%d] t=%.4f waited=%.4f h=%x ns=%lld lr=%08x\n", t ? t->index : -1, t0, k_ticks() / 268111856.0 - t0, h, (long long)ns, c->r[14]);
        return;
    }
    case 0x25: { /* WaitSynchronizationN: r1 handles, r2 count, r3 waitAll, r0:r4 ns -> r1 index */
        int32_t idx; uint32_t hs[64]; int n = (int)c->r[2]; if (n > 64) n = 64;
        for (int i = 0; i < n; i++) hs[i] = rd32(c->r[1] + 4u * (uint32_t)i);
        int64_t ns = (int64_t)(((uint64_t)c->r[4] << 32) | c->r[0]);
        k_trace("WaitSynchronizationN n=%d all=%u h0=%x h1=%x ns=%lld lr=%08x", n, c->r[3], n > 0 ? hs[0] : 0, n > 1 ? hs[1] : 0, (long long)ns, c->r[14]);
        pthread_mutex_lock(&K); uint32_t r = wait_n(t, hs, n, (int)c->r[3], ns, &idx); pthread_mutex_unlock(&K);
        c->r[0] = r; c->r[1] = (uint32_t)idx; return;
    }
    case 0x27: { pthread_mutex_lock(&K); KObject *o = k_obj(c->r[1]); if (o) o->refs++; c->r[1] = o ? k_new_handle(o) : 0; pthread_mutex_unlock(&K); c->r[0] = o ? RES_OK : RES_BAD_HANDLE; return; }
    case 0x28: { uint64_t ticks = k_ticks();
        c->r[0] = (uint32_t)ticks; c->r[1] = (uint32_t)(ticks >> 32); (void)get_tick_lo; (void)get_tick_hi; return; }
    case 0x2D: { /* ConnectToPort: r1 name -> r1 session */
        const char *name = (const char *)(g_mem + c->r[1]);
        pthread_mutex_lock(&K); uint32_t h = k_session_for(name); pthread_mutex_unlock(&K);
        k_trace("ConnectToPort(\"%s\") -> %x", name, h);
        c->r[0] = h ? RES_OK : 0xD88007FAu; c->r[1] = h; return;
    }
    case 0x32: { /* SendSyncRequest(r0 session) */
        KObject *o = k_obj(c->r[0]);
        if (!o || o->type != KO_SESSION) { fprintf(stderr, "kernel: SendSyncRequest on bad handle %x at %08x\n", c->r[0], pc); c->r[0] = RES_BAD_HANDLE; return; }
        uint32_t *cmd = (uint32_t *)(g_mem + c->tls + TLS_CMDBUF);
        k_trace("ipc %s cmd=%08x", o->svc->name, cmd[0]);
        o->svc->handle(t, cmd, o);
        c->r[0] = RES_OK; return;
    }
    case 0x35: c->r[0] = RES_OK; c->r[1] = 0x28; return;            /* GetProcessId */
    case 0x33: {   /* OpenProcess(r1 process id): this process (a game mod's loader, Randovania's, opens itself) */
        static uint32_t self;
        pthread_mutex_lock(&K); if (!self) self = k_new_handle(k_alloc(KO_PROCESS)); pthread_mutex_unlock(&K);
        c->r[0] = RES_OK; c->r[1] = self; return;
    }
    case 0x70:     /* ControlProcessMemory(process, addr0, addr1, size, type, permissions): every page here is readable,
                      writable and executable already, so a mod's loader making its code executable just succeeds */
        k_trace("ControlProcessMemory %08x %08x size %x type %u perm %u", c->r[1], c->r[2], c->r[3], c->r[0], c->r[4]);
        c->r[0] = RES_OK; return;
    case 0x37: { KObject *o = k_obj(c->r[1]); c->r[0] = RES_OK; c->r[1] = o && o->thread ? 0x100u + (uint32_t)o->thread->index : 0; return; }
    case 0x38: { KObject *o = k_alloc(KO_RESLIMIT); pthread_mutex_lock(&K); c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x3A: { /* GetResourceLimitCurrentValues: r0 values(s64*), r1 handle, r2 names, r3 count */
        for (uint32_t i = 0; i < c->r[3]; i++) {
            uint32_t name = rd32(c->r[2] + 4 * i); uint64_t v = 0;
            if (name == 1) v = heap_next - VA_HEAP + (linear_next - VA_LINEAR);   /* commit */
            wr32(c->r[0] + 8 * i, (uint32_t)v); wr32(c->r[0] + 8 * i + 4, (uint32_t)(v >> 32));
        }
        c->r[0] = RES_OK; return;
    }
    case 0x29: c->r[0] = RES_OK; c->r[1] = 0; c->r[2] = 0; return;   /* GetHandleInfo */
    case 0x2A: { /* GetSystemInfo: r1 type, r2 param -> r1:r2 */
        uint64_t v = 0;
        if (c->r[1] == 0 && c->r[2] == 1) v = heap_next - VA_HEAP + (linear_next - VA_LINEAR);   /* used application memory */
        c->r[0] = RES_OK; c->r[1] = (uint32_t)v; c->r[2] = (uint32_t)(v >> 32); return;
    }
    case 0x2B: { /* GetProcessInfo: r1 process, r2 type -> r1:r2 */
        uint64_t v = 0;
        if (c->r[2] == 0 || c->r[2] == 2) v = heap_next - VA_HEAP + (linear_next - VA_LINEAR);
        if (c->r[2] == 20) v = 0x20000000u - VA_LINEAR;   /* added to a linear heap address gives its physical address */
        k_trace("GetProcessInfo type %u -> %llx", c->r[2], (unsigned long long)v);
        c->r[0] = RES_OK; c->r[1] = (uint32_t)v; c->r[2] = (uint32_t)(v >> 32); return;
    }
    case 0x2C: c->r[0] = RES_OK; c->r[1] = 0; return;               /* GetThreadInfo */
    case 0x3C: fprintf(stderr, "kernel: svcBreak(%u) at %08x\n", c->r[0], pc); abort();
    case 0x3D: { /* OutputDebugString: r0 text, r1 length */
        uint32_t n = c->r[1] > 1024 ? 1024 : c->r[1];
        fprintf(stderr, "game: %.*s\n", (int)n, (const char *)(g_mem + c->r[0]));
        c->r[0] = RES_OK; return;
    }
    default:
        fprintf(stderr, "kernel: unimplemented svc 0x%02x at %08x\n", num, pc); abort();
    }
}

uint32_t k_session_for(const char *name) {
    const Service *s = svc_find(name);
    if (!s) return 0;
    KObject *o = k_alloc(KO_SESSION); o->svc = s;
    return k_new_handle(o);
}

static void init_config_pages(void) {
    /* kernel config page: values for an old-3DS retail unit, application memory type 0 (64 MiB) */
    wr8(VA_CONFIG + 0x02, 0x22);              /* KERNEL_VERSIONREVISION/minor (kernel 2.34) */
    wr8(VA_CONFIG + 0x03, 0x02);              /* KERNEL_VERSIONMAJOR */
    wr32(VA_CONFIG + 0x04, 2);                /* UPDATEFLAG */
    wr32(VA_CONFIG + 0x08, 0x00040130);       /* NS_TID low */
    wr32(VA_CONFIG + 0x10, 0x0000002B);       /* SYSCOREVER */
    wr8(VA_CONFIG + 0x14, 1);                 /* ENVINFO: retail */
    wr8(VA_CONFIG + 0x16, 1);                 /* UNITINFO */
    wr32(VA_CONFIG + 0x30, 0);                /* APPMEMTYPE */
    wr32(VA_CONFIG + 0x40, 0x04000000);       /* APPMEMALLOC */
    wr32(VA_CONFIG + 0x44, 0x02C00000);       /* SYSMEMALLOC */
    wr32(VA_CONFIG + 0x48, 0x01400000);       /* BASEMEMALLOC */
    wr32(VA_SHARED + 0x04, 1);                /* RUNNING_HW: product */
}

void k_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < MAX_THREADS; i++) pthread_cond_init(&tcv[i], NULL);
    g_trace = getenv("PWR_TRACE") != NULL;
    init_config_pages();
}

typedef struct { uint32_t entry; volatile int done; int ret; } MainStart;
static void *main_thread(void *p) { MainStart *m = p; m->ret = k_run_main(m->entry); m->done = 1; return NULL; }

/* run the guest main thread on its own host thread (recompiled code nests deeply on the host stack) */
volatile int *k_start_main(uint32_t entry) {
    static MainStart m; m.entry = entry;
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    pthread_create(&th, &at, main_thread, &m); pthread_detach(th);
    return &m.done;
}

int k_run_main(uint32_t entry) {
    pthread_mutex_lock(&K); KThread *t = new_thread(); pthread_mutex_unlock(&K);
    cur = t;
    t->prio = g_main_prio;
    rt_cpu_init(&t->cpu, entry);
    t->cpu.tls = VA_TLS + 0x200u * (uint32_t)t->index;
    t->cpu.r[13] = VA_STACK_TOP; t->cpu.r[14] = EXIT_MAGIC;
    k_trace("main thread start, tls=%08x", t->cpu.tls);
    thread_body(t);
    k_trace("main thread returned r0=%08x", t->cpu.r[0]);
    return (int)t->cpu.r[0];
}

/* ---- save states (state.h) ----
 * k_freeze: no thread takes the run token any more; the running one gives it up at its next svc, loop head or function
 * return. Then each thread is parked at a scheduling point (its registers are its state), blocked in a wait or a sleep
 * (saved as that svc, issued again on load), or finished. A thread inside a nested rt_run (msr_game_hook's calls) is not
 * stopped there: freezing waits for it to come out. */
static pthread_cond_t KF = PTHREAD_COND_INITIALIZER;   /* the freezer and the loader wait on it */
static int parked_n;

static void freeze_note(void) { if (freeze_req) pthread_cond_broadcast(&KF); }

int k_freeze(int timeout_ms) {
    pthread_mutex_lock(&K);
    freeze_req = 1;
    uint64_t dl = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        int nested = 0;
        for (int i = 0; i < MAX_THREADS; i++) if (threads[i] && !threads[i]->finished && threads[i]->nest > 1) nested = 1;
        if (!running && !nested) break;
        if (running) { running->preempt = 1; __atomic_store_n(&running->cpu.budget, 0, __ATOMIC_RELAXED); }
        if (now_ns() >= dl) { freeze_req = 0; wake_ready(); pthread_mutex_unlock(&K); return -1; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 2000000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&KF, &K, &ts);
    }
    pthread_mutex_unlock(&K);
    return 0;
}

void k_thaw(void) {
    pthread_mutex_lock(&K); freeze_req = 0; wake_ready(); pthread_mutex_unlock(&K);
}

/* every live guest thread leaves its wait, sleep or scheduling point and parks (thread_body); 0 when all have */
int k_abandon_all(int timeout_ms) {
    pthread_mutex_lock(&K);
    int alive = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        KThread *t = threads[i];
        if (!t || t->finished) continue;
        alive++; t->abandon = 1;
        pthread_cond_signal(&tcv[i]);
    }
    uint64_t dl = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while (parked_n < alive) {
        if (now_ns() >= dl) { fprintf(stderr, "state: %d of %d threads stopped\n", parked_n, alive); pthread_mutex_unlock(&K); return -1; }
        for (int i = 0; i < MAX_THREADS; i++) if (threads[i] && threads[i]->abandon && !threads[i]->parked) pthread_cond_signal(&tcv[i]);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 2000000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&KF, &K, &ts);
    }
    pthread_mutex_unlock(&K);
    return 0;
}

/* objects by number: while saving, the ones met so far; while loading, the ones made */
static KObject **ks_objs; static uint32_t ks_n, ks_cap;
uint32_t ks_id(KObject *o) {
    if (!o) return 0;
    for (uint32_t i = 0; i < ks_n; i++) if (ks_objs[i] == o) return i + 1;
    if (ks_n == ks_cap) { ks_cap = ks_cap ? ks_cap * 2 : 512; ks_objs = realloc(ks_objs, ks_cap * sizeof *ks_objs); }
    ks_objs[ks_n++] = o;
    return ks_n;
}
KObject *ks_obj(uint32_t id) { return id && id <= ks_n ? ks_objs[id - 1] : NULL; }

const char *k_session_name(KObject *o) { return o && o->svc ? o->svc->name : ""; }
const Service *fs_service_find(const char *name);   /* fs.c: its file and directory sessions */

typedef struct {   /* a thread in the state */
    int32_t index; uint32_t handle, obj, finished, prio, resume_svc;
    uint32_t r[16], n, z, cf, v, q, ge, cpsr_rest, thumb, s[32], fpscr, tls, tls_rw, excl_valid, excl_addr; uint64_t excl_value;
} ThreadRec;

static void cpu_to_rec(const CPU *c, ThreadRec *t) {
    memcpy(t->r, c->r, sizeof t->r); t->n = c->n; t->z = c->z; t->cf = c->cf; t->v = c->v; t->q = c->q; t->ge = c->ge;
    t->cpsr_rest = c->cpsr_rest; t->thumb = c->thumb; memcpy(t->s, c->s, sizeof t->s); t->fpscr = c->fpscr; t->tls = c->tls;
    t->tls_rw = c->tls_rw; t->excl_valid = 0; t->excl_addr = 0; t->excl_value = 0;
}
static void rec_to_cpu(const ThreadRec *t, CPU *c) {
    rt_cpu_init(c, (t->r[15] & ~1u) | t->thumb);   /* the dispatcher's page table and memory callbacks */
    memcpy(c->r, t->r, sizeof c->r); c->n = t->n; c->z = t->z; c->cf = t->cf; c->v = t->v; c->q = t->q; c->ge = t->ge;
    c->cpsr_rest = t->cpsr_rest; c->thumb = t->thumb; memcpy(c->s, t->s, sizeof c->s); c->fpscr = t->fpscr; c->tls = t->tls; c->tls_rw = t->tls_rw;
}

int k_state_mem_fixups(uint32_t *addr, int max) {
    int n = 0;
    for (int i = 0; i < MAX_THREADS && n < max; i++) {
        KThread *t = threads[i];
        if (!t || t->finished || !t->in_svc || t->svc_num != 0x22 || !t->arb) continue;
        ArbWaiter *w = t->arb;
        if (!w->wake && (t->svc_r[2] == 2 || t->svc_r[2] == 4)) addr[n++] = t->svc_r[1];   /* issued again, it decrements again */
    }
    return n;
}

void k_state_save(SBuf *b) {
    pthread_mutex_lock(&K);
    ks_n = 0;
    uint64_t now = now_ns(); SB(b, now);
    sb_u32(b, heap_next); sb_u32(b, linear_next); sb_u32(b, (uint32_t)g_main_prio);
    sb_u32(b, (uint32_t)nregions); sb_put(b, regions, sizeof *regions * (size_t)nregions);
    uint32_t nh = 0; for (int i = 0; i < MAX_HANDLES; i++) nh += handles[i] != NULL;
    sb_u32(b, nh);
    for (int i = 0; i < MAX_HANDLES; i++) if (handles[i]) { sb_u32(b, (uint32_t)i); sb_u32(b, ks_id(handles[i])); }
    uint32_t nt = 0; for (int i = 0; i < MAX_THREADS; i++) nt += threads[i] != NULL;
    sb_u32(b, nt);
    for (int i = 0; i < MAX_THREADS; i++) {
        KThread *t = threads[i]; if (!t) continue;
        ThreadRec r; memset(&r, 0, sizeof r);
        r.index = t->index; r.handle = t->handle; r.obj = ks_id(t->obj); r.finished = (uint32_t)t->finished; r.prio = (uint32_t)t->prio;
        cpu_to_rec(&t->cpu, &r);
        if (!t->finished && t->in_svc) {
            int woken = t->svc_num == 0x22 && t->arb && ((ArbWaiter *)t->arb)->wake;
            if (woken) { r.r[0] = RES_OK; }   /* the arbiter woke it: the svc is over */
            else { r.resume_svc = t->svc_num + 1; memcpy(r.r, t->svc_r, sizeof t->svc_r); }
        }
        SB(b, r);
    }
    uint32_t ntm = 0; for (int i = 0; i < MAX_TIMERS; i++) ntm += timers[i].o != NULL;
    sb_u32(b, ntm);
    for (int i = 0; i < MAX_TIMERS; i++) if (timers[i].o) {
        Timer *tm = &timers[i];
        sb_u32(b, ks_id(tm->o)); uint64_t rel = tm->due > now ? tm->due - now : 0; SB(b, rel); SB(b, tm->interval); sb_u32(b, (uint32_t)tm->armed);
    }
    pthread_mutex_unlock(&K);
}

typedef struct {   /* an object in the state */
    uint32_t type, refs, signaled, reset_type; int32_t count, max, owner, lock_count, thread; uint32_t addr, size;
} ObjRec;

void k_state_save_objects(SBuf *b) {
    pthread_mutex_lock(&K);
    sb_u32(b, ks_n);
    for (uint32_t i = 0; i < ks_n; i++) {   /* fs_session_save may name no new objects: the count stays */
        KObject *o = ks_objs[i];
        ObjRec r = { (uint32_t)o->type, (uint32_t)o->refs, (uint32_t)o->signaled, (uint32_t)o->reset_type, o->count, o->max,
                     o->owner ? o->owner->index : -1, o->lock_count, o->thread ? o->thread->index : -1, o->addr, o->size };
        SB(b, r);
        const char *name = k_session_name(o); uint32_t len = (uint32_t)strlen(name);
        sb_u32(b, len); sb_put(b, name, len);
        SBuf extra = { 0 };
        if (o->svc_state && !fs_session_save(o, &extra)) {}
        sb_u32(b, (uint32_t)extra.n); if (extra.n) sb_put(b, extra.p, extra.n);
        free(extra.p);
    }
    pthread_mutex_unlock(&K);
}

static int32_t *ks_owner, *ks_thread;   /* loading: thread indices the objects name, set once the threads exist */
void k_state_load_objects(SRd *r) {
    uint32_t n = sr_u32(r);
    ks_n = 0;
    free(ks_owner); free(ks_thread);
    ks_owner = calloc(n + 1, sizeof *ks_owner); ks_thread = calloc(n + 1, sizeof *ks_thread);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        ObjRec rec; SR(r, rec);
        KObject *o = k_alloc((KType)rec.type);
        o->refs = (int)rec.refs; o->signaled = (int)rec.signaled; o->reset_type = (int)rec.reset_type; o->count = rec.count; o->max = rec.max;
        o->lock_count = rec.lock_count; o->addr = rec.addr; o->size = rec.size;
        ks_owner[i] = rec.owner; ks_thread[i] = rec.thread;
        char name[64] = { 0 }; uint32_t len = sr_u32(r); if (len >= sizeof name) { r->bad = 1; break; }
        sr_get(r, name, len);
        uint32_t extra = sr_u32(r);
        if (name[0]) { o->svc = svc_find(name); if (!o->svc) o->svc = fs_service_find(name); }
        if (extra) { SRd sub = { r->p + r->off, extra, 0, 0 }; fs_session_load(o, name, &sub); r->off += extra; if (r->off > r->n) r->bad = 1; }
        ks_id(o);
    }
}

static void *thread_resume_main(void *p);
int k_state_load(SRd *r) {
    pthread_mutex_lock(&K);
    uint64_t saved_now; SR(r, saved_now);
    heap_next = sr_u32(r); linear_next = sr_u32(r); g_main_prio = (int)sr_u32(r);
    nregions = (int)sr_u32(r); if (nregions > MAX_REGIONS) { nregions = 0; r->bad = 1; }
    sr_get(r, regions, sizeof *regions * (size_t)nregions);
    memset(handles, 0, sizeof handles);
    uint32_t nh = sr_u32(r);
    for (uint32_t i = 0; i < nh && !r->bad; i++) { uint32_t slot = sr_u32(r), id = sr_u32(r); if (slot < MAX_HANDLES) handles[slot] = ks_obj(id); }
    /* the clock: guest time goes on from where the state left it */
    { struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n);
      uint64_t nn = (uint64_t)n.tv_sec * 1000000000ull + (uint64_t)n.tv_nsec, base = nn > saved_now ? nn - saved_now : 0;
      t0.tv_sec = (time_t)(base / 1000000000ull); t0.tv_nsec = (long)(base % 1000000000ull); }
    uint32_t nt = sr_u32(r);
    ThreadRec recs[MAX_THREADS]; int have[MAX_THREADS] = { 0 };
    for (uint32_t i = 0; i < nt && !r->bad; i++) { ThreadRec rec; SR(r, rec); if (rec.index >= 0 && rec.index < MAX_THREADS) { recs[rec.index] = rec; have[rec.index] = 1; } }
    /* threads: a parked host thread takes the saved state of its index; one the state lacks (or has finished) ends; a saved
       thread without a host thread gets a new one (thread_resume_main), started by k_state_resume */
    for (int i = 0; i < MAX_THREADS; i++) {
        KThread *t = threads[i];
        if (!have[i]) {
            if (t && !t->finished) { t->dead = 1; t->abandon = 0; pthread_cond_signal(&tcv[i]); }
            threads[i] = NULL;
            continue;
        }
        const ThreadRec *rec = &recs[i];
        if (t && !t->finished && rec->finished) { t->dead = 1; t->abandon = 0; pthread_cond_signal(&tcv[i]); t = NULL; }
        if (!t || (t->finished && !rec->finished)) {   /* a new struct: an exited host thread's may still be read by nobody */
            KThread *nt2 = calloc(1, sizeof *nt2); nt2->index = i; nt2->spawn = !rec->finished; t = nt2;
        }
        threads[i] = t;
        t->handle = rec->handle; t->obj = ks_obj(rec->obj); t->finished = (int)rec->finished; t->prio = (int)rec->prio;
        t->ready = 0; t->in_svc = 0; t->preempt = 0; t->arb = NULL; t->nest = 0;
        t->resume_svc = rec->resume_svc;
        rec_to_cpu(rec, &t->cpu);
        if (t->obj) t->obj->thread = t;
        t_blocked[i] = 0; t_nwo[i] = 0; t_wo[i] = NULL;
    }
    for (uint32_t i = 0; i < ks_n; i++) {   /* the objects' threads */
        KObject *o = ks_objs[i];
        o->owner = ks_owner[i] >= 0 && ks_owner[i] < MAX_THREADS ? threads[ks_owner[i]] : NULL;
        if (ks_thread[i] >= 0 && ks_thread[i] < MAX_THREADS) o->thread = threads[ks_thread[i]];
    }
    memset(timers, 0, sizeof timers);
    uint32_t ntm = sr_u32(r);
    uint64_t now = now_ns();
    for (uint32_t i = 0; i < ntm && i < MAX_TIMERS && !r->bad; i++) {
        uint32_t id = sr_u32(r); uint64_t rel, interval; SR(r, rel); SR(r, interval); uint32_t armed = sr_u32(r);
        timers[i].o = ks_obj(id); timers[i].due = now + rel; timers[i].interval = interval; timers[i].armed = (int)armed;
    }
    if (ntm && !timer_thread) pthread_create(&timer_thread, NULL, timer_main, NULL);
    pthread_cond_signal(&TC);
    memset(arb_waiters, 0, sizeof arb_waiters);
    running = NULL;
    pthread_mutex_unlock(&K);
    return r->bad ? -1 : 0;
}

void k_state_resume(void) {
    pthread_mutex_lock(&K);
    parked_n = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        KThread *t = threads[i];
        if (!t || t->finished) continue;
        if (t->spawn) {
            t->spawn = 0;
            pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
            pthread_create(&th, &at, thread_resume_main, t); pthread_detach(th);
        } else { t->abandon = 0; pthread_cond_signal(&tcv[i]); }
    }
    freeze_req = 0;
    wake_ready();
    pthread_mutex_unlock(&K);
}

/* thread_body: a load abandoned this thread; parked until k_state_load gives it its new state (0: ended) */
static int freeze_parked(KThread *t) {
    pthread_mutex_lock(&K);
    t->parked = 1; parked_n++; pthread_cond_broadcast(&KF);
    while (t->abandon && !t->dead) { t_blocked[t->index] = 1; pthread_cond_wait(&tcv[t->index], &K); t_blocked[t->index] = 0; }
    t->parked = 0;
    int dead = t->dead;
    pthread_mutex_unlock(&K);
    return !dead;
}
