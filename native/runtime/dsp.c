/* dsp::DSP: HLE of the audio DSP firmware's host interface.
 * The sound library loads a DSP component, registers interrupt events, then talks over pipe 2 ("audio"): it writes a
 * 4-byte state change (0 = initialize) and reads back {u16 count = 15, u16 address[15]}, the DSP word addresses of the
 * shared structures (frame counter, source configurations, ...). Those live in two double-buffered regions of DSP RAM
 * (guest 0x1FF50000 and 0x1FF70000; DSP word address a <-> guest (a << 1) + 0x1FF40000). Every audio frame
 * (160 samples at 32728 Hz, ~4.9 ms) the DSP bumps the frame counter and raises the pipe-2 interrupt.
 * Each tick the DSP reads the source configurations of the region the
 * application wrote last (the one with the higher frame counter, which the application writes, not the DSP), advances
 * every enabled source through its buffer queue by 160 * rate_multiplier input samples, and writes the source statuses
 * (enabled, buffer position, current/last buffer id, sync count) into the other region. The title screen's clock follows
 * the music stream position read from there. The sources are also decoded (PCM8/PCM16/DSP-ADPCM, linear resampling), mixed with their
 * gains into the three intermediate mixes (main, aux A/B, see dsp_tick), combined into 160 stereo frames per tick, and played through SDL
 * (unless --mute; PWR_DSP_WAV=<file> writes the mix to a WAV file even when muted). No DSP delay/reverb effects, no source filters. Structure layout: see Azahar audio_core/hle/shared_memory.h (read only). */
#include "kernel.h"
#include "gpu.h"
#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR(id, normal, translate) (((uint32_t)(id) << 16) | ((normal) << 6) | (translate))
#define DSP_REGION0 0x1FF50000u
#define DSP_REGION1 0x1FF70000u
#define N_STRUCTS 15
#define N_SRC 24
#define CFG_OFF 0x3D24u                  /* source_configurations in a region: 24 x 192 bytes */
#define STAT_OFF 0x0D00u                 /* source_statuses: 24 x 12 bytes */
#define CNT_OFF 0x7FFEu                  /* frame counter */
#define MAXQ 16
#define COEF_OFF 0x4F24u                 /* adpcm_coefficients: 24 x 16 s16 */
#define DSPCFG_OFF 0x2860u               /* dsp_configuration (master volume at +4, aux return volumes +8/+0xC, aux bus enables +0x28/+0x2A) */
#define IMIX_OFF 0x2924u                 /* intermediate_mix_samples: aux A then aux B, s32 [4 channels][160] each (plain little endian) */
extern int g_mute;

#include "state.h"
static pthread_mutex_t D = PTHREAD_MUTEX_INITIALIZER;
static KObject *pipe_irq[4];          /* event registered for (interrupt 2 = pipe, channel n) */
static KObject *sem_event;            /* GetSemaphoreEventHandle */
static int running;                   /* initialized through the audio pipe */
static uint8_t pipe2_out[64]; static uint32_t pipe2_len;
static uint16_t frame;

static void audio_pipe_write(const uint8_t *buf, uint32_t size) {
    if (size < 1) return;
    uint32_t state = buf[0];
    k_trace("dsp: audio pipe state change %u", state);
    if (state == 0 || state == 2) {   /* initialize / wakeup: report the structure addresses */
        running = 1;
        pipe2_out[0] = N_STRUCTS; pipe2_out[1] = 0;
        for (int i = 0; i < N_STRUCTS; i++) {
            static const uint16_t addr[N_STRUCTS] = { 0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
                                                       0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C };
            uint16_t a = addr[i];
            pipe2_out[2 + 2 * i] = (uint8_t)a; pipe2_out[3 + 2 * i] = (uint8_t)(a >> 8);
        }
        pipe2_len = 2 + 2 * N_STRUCTS;
    } else running = 0;
    if (pipe_irq[2]) k_signal(pipe_irq[2]);
}

typedef struct { uint32_t len, pos, va; uint16_t id; uint8_t loop, from_queue, played, fmt, stereo, ad_dirty; int16_t yn[2]; } Buf;
typedef struct {
    int enabled, buffer_update, have_cur, starved;
    uint16_t sync, cur_id, last_id;
    float rate, gain[3][4], gstart[3][4];   /* gain[mix]: intermediate mixes 0 (main), 1 and 2 (aux A/B); each left/right front, left/right rear */
    int ramp[3];                         /* gains changed: ramp from gstart over this frame, as the DSP does */
    uint8_t fmt, stereo;                 /* 0 PCM8, 1 PCM16, 2 ADPCM; channels (2 = stereo) */
    Buf q[MAXQ]; int nq;                 /* queued buffers, lowest id first */
    Buf cur; double cur_pos;             /* current buffer and samples consumed in it */
    int ci; int c0[2], c1[2];            /* integer sample index whose value is c0 (c1 = the one before), for linear interpolation */
    int af_idx, af_next, yn1, yn2, ys1, ys2; int16_t af[14]; int16_t coef[16];   /* ADPCM: decoded frame, history (ys = at the buffer start), coefficients */
} Src;
static Src src[N_SRC];
static float dsp_vol[3] = { 1.0f, 0.0f, 0.0f }; static int dsp_aux_on[2];   /* the mixer's volumes and aux buses (dsp_tick) */
static int dsp_dbg = -1;
void dsp_hang_report(void) {
    fprintf(stderr, "hang: dsp running %d frame %u pipe irq events", running, frame);
    for (int i = 0; i < 4; i++) fprintf(stderr, " %s", pipe_irq[i] ? (pipe_irq[i]->signaled ? "signaled" : "clear") : "-");
    fprintf(stderr, ", semaphore event %s\n", sem_event ? (sem_event->signaled ? "signaled" : "clear") : "-");
}

static uint32_t dsp32(uint32_t a) { uint32_t v = rd32(a); return (v << 16) | (v >> 16); }   /* DSP words are half-swapped */
static void dsp_w32(uint32_t a, uint32_t v) { wr32(a, (v << 16) | (v >> 16)); }

static void q_push(Src *s, Buf b) {
    if (s->nq >= MAXQ) return;
    int i = s->nq++;
    while (i > 0 && s->q[i - 1].id > b.id) { s->q[i] = s->q[i - 1]; i--; }
    s->q[i] = b;
}

static int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* the sample with index i of the current buffer (stereo: l, r; mono: both the same), decoded on demand; ADPCM is decoded forward only */
static void fetch(Src *s, int i, int out[2]) {
    const Buf *b = &s->cur;
    if (!b->va || i < 0 || (uint32_t)i >= b->len) { out[0] = out[1] = 0; return; }
    if (b->fmt == 2) {
        int fi = i / 14;
        if (fi < s->af_idx) { s->af_next = 0; s->yn1 = s->ys1; s->yn2 = s->ys2; s->af_idx = -1; }
        while (s->af_next <= fi) {       /* DSP-ADPCM: 8-byte frames, header (predictor, scale) + 14 nibbles */
            uint32_t a = b->va + 8u * (uint32_t)s->af_next; uint8_t h = g_mem[a]; int sc = 1 << (h & 15), ix = (h >> 4) & 7;
            int c1 = s->coef[2 * ix], c2 = s->coef[2 * ix + 1];
            for (int k = 0; k < 14; k++) {
                uint8_t by = g_mem[a + 1 + (uint32_t)k / 2]; int n = (k & 1) ? (by & 15) : (by >> 4); if (n >= 8) n -= 16;
                int v = clamp16((((sc * n) << 11) + 0x400 + c1 * s->yn1 + c2 * s->yn2) >> 11);
                s->af[k] = (int16_t)v; s->yn2 = s->yn1; s->yn1 = v;
            }
            s->af_idx = s->af_next++;
        }
        out[0] = out[1] = s->af[i % 14]; return;
    }
    int ch = b->stereo == 2 ? 2 : 1;
    for (int c = 0; c < 2; c++) {
        uint32_t idx = (uint32_t)i * (uint32_t)ch + (uint32_t)(ch == 2 ? c : 0);
        out[c] = b->fmt == 1 ? (int16_t)(g_mem[b->va + idx * 2] | g_mem[b->va + idx * 2 + 1] << 8) : (int)(int8_t)g_mem[b->va + idx] * 256;
    }
}

/* the current buffer was just chosen: prime the decoder and the interpolator. ADPCM history comes from the buffer only when the
 * application marked it (adpcm_dirty); otherwise decoding continues from the previous buffer, as one stream split in buffers.
 * cont: the previous buffer was playing right up to this one, so its last sample is the interpolator's previous sample */
static void start_cur(Src *s, int cont) {
    if (s->cur.ad_dirty) { s->yn1 = s->cur.yn[0]; s->yn2 = s->cur.yn[1]; }
    s->ys1 = s->yn1; s->ys2 = s->yn2; s->af_idx = -1; s->af_next = 0;
    int prev[2] = { s->c0[0], s->c0[1] };
    s->ci = (int)s->cur_pos; fetch(s, s->ci, s->c0);
    s->c1[0] = cont ? prev[0] : s->c0[0]; s->c1[1] = cont ? prev[1] : s->c0[1];
}

static int dequeue(Src *s, int cont) {
    if (!s->nq) return 0;
    Buf b = s->q[0]; memmove(s->q, s->q + 1, (size_t)(--s->nq) * sizeof(Buf));
    s->cur = b; s->have_cur = 1; s->cur_id = b.id; s->last_id = 0;
    s->cur_pos = b.played ? 0 : b.pos;
    s->buffer_update = b.from_queue && !b.played;
    if (b.loop) { b.played = 1; q_push(s, b); }
    start_cur(s, cont);
    return 1;
}

static void src_parse(Src *s, uint32_t c, uint32_t coef) {
    uint32_t dirty = rd32(c);
    if (!dirty) return;
    if (dirty & (1u << 29)) { memset(s, 0, sizeof *s); s->rate = 1.0f; s->stereo = 1; }
    if (dirty & (1u << 4)) s->nq = 0;
    if (dirty & (1u << 16)) s->enabled = g_mem[c + 0xA0] != 0;
    if (dirty & (1u << 28)) s->sync = (uint16_t)rd16(c + 0xA2);
    if (dirty & (1u << 2)) for (int k = 0; k < 16; k++) s->coef[k] = (int16_t)rd16(coef + 2u * (uint32_t)k);   /* only when marked: the other region's copy is stale */
    if (dirty & (1u << 18)) { float r; memcpy(&r, g_mem + c + 0x34, 4); s->rate = r > 0 ? r : 1.0f; }
    for (int m = 0; m < 3; m++) if (dirty & (1u << (25 + m))) {   /* gain_0/1/2_dirty */
        if (!s->ramp[m]) memcpy(s->gstart[m], s->gain[m], 16);
        memcpy(s->gain[m], g_mem + c + 4 + 16u * (uint32_t)m, 16); s->ramp[m] = 1;
    }
    if (dirty & (1u << 30 | 3u)) { uint16_t f = (uint16_t)rd16(c + 0xB4); s->stereo = f & 3; s->fmt = (uint8_t)((f >> 2) & 3); }
    uint32_t play_pos = (dirty & (1u << 21)) ? dsp32(c + 0xA4) : 0;
    if (dirty & (1u << 30)) {            /* embedded buffer */
        uint32_t len = dsp32(c + 0xB0);
        if ((int32_t)len >= 0) {
            Buf b = { len, play_pos, pa_to_va(dsp32(c + 0xAC)), (uint16_t)rd16(c + 0xBE), (uint8_t)((rd16(c + 0xBC) >> 1) & 1), 0, 0, s->fmt, s->stereo, (uint8_t)(rd16(c + 0xBC) & 1),
                      { (int16_t)rd16(c + 0xB8), (int16_t)rd16(c + 0xBA) } };
            q_push(s, b);
        }
    }
    if (dirty & (1u << 19)) {            /* buffer queue */
        uint32_t bd = rd16(c + 0x4A);
        for (int i = 0; i < 4; i++) if (bd & (1u << i)) {
            uint32_t o = c + 0x4C + 20u * (uint32_t)i, len = dsp32(o + 4);
            if ((int32_t)len >= 0) {
                Buf b = { len, 0, pa_to_va(dsp32(o)), (uint16_t)rd16(o + 0x10), g_mem[o + 0xF] != 0, 1, 0, s->fmt, s->stereo, g_mem[o + 0xE],
                          { (int16_t)rd16(o + 0xA), (int16_t)rd16(o + 0xC) } };
                q_push(s, b);
            }
        }
        wr16(c + 0x4A, 0);
    }
    wr32(c, 0);                          /* the DSP clears the dirty flags it handled */
}

static int32_t mixq[3][160][4];          /* this tick's intermediate mixes (main, aux A, aux B), 4 channels each */

/* one audio frame of a source: advance 160 output samples (160 * rate input samples), mix them into the three intermediate mixes
 * with their gains (a stereo source feeds its left sample to channels 0/2, right to 1/3; gains that changed ramp over the frame) */
static void src_frame(Src *s) {
    if (!s->have_cur) {
        if (dequeue(s, 0)) s->starved = 0;
        else {
            /* the application feeds buffers from its own threads, which can lag the host's fixed audio clock by more than the
             * hardware would allow: wait a little for the next buffer instead of ending the stream at once */
            if (s->starved++ < 40) return;
            s->enabled = 0; s->buffer_update = 1; s->last_id = s->cur_id; s->cur_id = 0; return;
        }
    }
    for (int k = 0; k < 160; k++) {
        int guard = 0;
        while (s->cur_pos >= s->cur.len && guard++ < 8) { s->have_cur = 0; if (!dequeue(s, 1)) break; }
        if (!s->have_cur || s->cur_pos >= s->cur.len) break;
        int fl = (int)s->cur_pos;
        while (s->ci < fl) { s->c1[0] = s->c0[0]; s->c1[1] = s->c0[1]; s->ci++; fetch(s, s->ci, s->c0); }
        float fr = (float)(s->cur_pos - fl), l = (float)s->c1[0] + (float)(s->c0[0] - s->c1[0]) * fr, r = (float)s->c1[1] + (float)(s->c0[1] - s->c1[1]) * fr;
        float t = (float)k / 159.f;
        for (int m = 0; m < 3; m++) {
            const float *g = s->gain[m], *g0 = s->gstart[m];
            if (!s->ramp[m] && !g[0] && !g[1] && !g[2] && !g[3]) continue;
            float a0 = s->ramp[m] ? g0[0] + (g[0] - g0[0]) * t : g[0], a1 = s->ramp[m] ? g0[1] + (g[1] - g0[1]) * t : g[1];
            float a2 = s->ramp[m] ? g0[2] + (g[2] - g0[2]) * t : g[2], a3 = s->ramp[m] ? g0[3] + (g[3] - g0[3]) * t : g[3];
            mixq[m][k][0] += (int32_t)(l * a0); mixq[m][k][1] += (int32_t)(r * a1); mixq[m][k][2] += (int32_t)(l * a2); mixq[m][k][3] += (int32_t)(r * a3);
        }
        s->cur_pos += s->rate;
    }
    for (int m = 0; m < 3; m++) if (s->ramp[m]) { s->ramp[m] = 0; memcpy(s->gstart[m], s->gain[m], 16); }
    if (s->have_cur && s->cur_pos >= s->cur.len) { s->have_cur = 0; if (!dequeue(s, 1)) { /* queue ran dry exactly now: next tick starves */ } }
}

/* ---- audio output: a ring of stereo s16 frames filled by dsp_tick, drained by SDL's audio thread ---- */
#define RING 16384                       /* frames (~0.5 s) */
static int16_t ring[RING][2];
static volatile unsigned rhead, rtail;   /* produced / consumed, free-running */
static volatile unsigned a_underruns, a_dropped;   /* device ran dry while playing / frames dropped because the ring was full */
static SDL_AudioDeviceID adev;
static FILE *wavf; static uint32_t wav_frames;

#define PREFILL 1600                     /* frames (~50 ms) collected before playback starts or resumes after an underrun */
#define TARGET_FILL 2048.0               /* frames (~63 ms) the DSP clock keeps in the ring while playing */
static volatile float volume = 1.f;   /* the settings overlay's Volume (--volume) */
void dsp_set_volume(float v) { volume = v < 0 ? 0 : v > 1 ? 1 : v; }
static void audio_cb(void *u, Uint8 *stream, int len) {
    (void)u; int16_t *o = (int16_t *)stream; int n = len / 4; static int playing; const float g = volume;
    if (!playing && rhead - rtail >= PREFILL) playing = 1;
    for (int i = 0; i < n; i++) {
        if (playing && rtail != rhead) { o[2 * i] = (int16_t)(ring[rtail % RING][0] * g); o[2 * i + 1] = (int16_t)(ring[rtail % RING][1] * g); rtail++; }
        else { if (playing) a_underruns++; o[2 * i] = o[2 * i + 1] = 0; playing = 0; }
    }
}

void dsp_close_wav(void) {   /* also main.c, which ends without atexit on Linux */
    if (!wavf) return;
    uint32_t data = wav_frames * 4, rate = 32728, br = rate * 4, v;
    fseek(wavf, 4, SEEK_SET); v = 36 + data; fwrite(&v, 4, 1, wavf);
    fseek(wavf, 40, SEEK_SET); fwrite(&data, 4, 1, wavf); (void)br;
    fclose(wavf); wavf = NULL;
}

static void audio_open(void) {
    static int done; if (done) return; done = 1;
    const char *w = getenv("PWR_DSP_WAV");
    if (w && (wavf = fopen(w, "wb"))) {
        uint8_t h[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0, 0xD8,0x7F,0,0, 0x60,0xFF,1,0, 4,0,16,0, 'd','a','t','a',0,0,0,0 };
        fwrite(h, 1, 44, wavf); atexit(dsp_close_wav);
    }
    if (g_mute) {   /* PWR_DSP_DUMMY_TEST=1: exercise the SDL path while muted, on SDL's dummy driver (cannot make a sound) */
        if (!getenv("PWR_DSP_DUMMY_TEST")) return;
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "dsp: SDL audio: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec want = { .freq = 32728, .format = AUDIO_S16SYS, .channels = 2, .samples = 512, .callback = audio_cb }, got;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!adev) { fprintf(stderr, "dsp: SDL_OpenAudioDevice: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(adev, 0);
    fprintf(stderr, "pwr: audio output opened (%d Hz)\n", got.freq);
}

/* final mix: each intermediate mix times its volume (master, aux return A/B), downmixed to stereo (left = front + rear), clamped */
static void audio_push(const float vol[3]) {
    int16_t out[160][2];
    for (int k = 0; k < 160; k++) for (int c = 0; c < 2; c++) {
        int v = 0;
        for (int m = 0; m < 3; m++) v = clamp16(v + clamp16((int)(vol[m] * (float)(mixq[m][k][c] + mixq[m][k][c + 2]))));
        out[k][c] = (int16_t)v;
    }
    if (wavf) { fwrite(out, 4, 160, wavf); wav_frames += 160; }
    if (adev) {
        for (int k = 0; k < 160; k++) {
            if (rhead - rtail >= RING) { a_dropped += 160 - (unsigned)k; break; }   /* the device is far behind: drop */
            ring[rhead % RING][0] = out[k][0]; ring[rhead % RING][1] = out[k][1]; rhead++;
        }
    }
}

/* one line for the 2 s stats block when sound is playing: an underrun is a gap (a click) on the user's device */
void dsp_stats(void) {
    if (adev) fprintf(stderr, "pwr: audio ring fill %u frames, underruns %u, dropped %u\n", rhead - rtail, a_underruns, a_dropped);
}

/* host tick, called every 160 / 32728 s by the audio thread (below) */
static void dsp_tick(void) {
    pthread_mutex_lock(&D);
    if (dsp_dbg < 0) dsp_dbg = getenv("PWR_DSP_TRACE") != NULL;
    /* The DSP runs in real time whatever the guest does: sources keep playing and the mix keeps coming. Only the frame interrupt is paced:
     * the guest's audio threads run slower than the 200 Hz clock at times (single run token, heavy scenes), and a message queue between
     * them overflows when frames are signalled faster than they are picked up, so an interrupt is raised only when the previous one
     * has been taken (PWR_DSP_NOPACE=1: always). The "buffer finished" flag stays set until an interrupt has carried it. */
    static int nopace = -1; if (nopace < 0) nopace = getenv("PWR_DSP_NOPACE") != NULL;
    int notify = nopace || !pipe_irq[2] || !pipe_irq[2]->signaled;
    if (running) {
        uint16_t c0 = (uint16_t)rd16(DSP_REGION0 + CNT_OFF), c1 = (uint16_t)rd16(DSP_REGION1 + CNT_OFF);
        int cur = (c0 == 0xFFFF && c1 != 0xFFFE) ? 1 : (c1 == 0xFFFF && c0 != 0xFFFE) ? 0 : (c0 > c1 ? 0 : 1);
        uint32_t rd = cur ? DSP_REGION1 : DSP_REGION0, wr = cur ? DSP_REGION0 : DSP_REGION1;
        static int nosrc = -1; if (nosrc < 0) nosrc = getenv("PWR_DSP_NOSRC") != NULL;   /* A/B: statuses stay all zero */
        /* DSP configuration: master volume (bit 16), aux return volumes (24/25), aux bus enables (8/9). An enabled aux bus hands its mix to
         * the application through the intermediate-mix area (written to the other region, read back from this one next frame, where the
         * application may have processed it); a disabled one goes straight to the final mix. Not done: the DSP's own delay and reverb
         * effects (this game never enables them: its reverb runs in its own software mixer, "spu", before the DSP), surround/mono output */
        float *vol = dsp_vol; int *aux_on = dsp_aux_on;
        { uint32_t dc = rd + DSPCFG_OFF, d = rd32(dc);
          if (d & (1u << 16)) memcpy(&vol[0], g_mem + dc + 4, 4);
          if (d & (1u << 24)) memcpy(&vol[1], g_mem + dc + 8, 4);
          if (d & (1u << 25)) memcpy(&vol[2], g_mem + dc + 12, 4);
          if (d & (1u << 8)) aux_on[0] = rd16(dc + 0x28) != 0;
          if (d & (1u << 9)) aux_on[1] = rd16(dc + 0x2A) != 0;
          if (dsp_dbg && d) {   /* PWR_DSP_TRACE: one line when the mixer or effect settings change */
              static uint32_t pk[8]; uint32_t k[8] = { (uint32_t)aux_on[0], (uint32_t)aux_on[1], rd16(dc + 0x2C + 2), rd16(dc + 0x40 + 2), 0, 0, 0, 0 };
              memcpy(&k[4], vol, 12); for (int w = 1; w < 26; w++) k[7] |= rd16(dc + 0x54 + 2u * (uint32_t)w) | rd16(dc + 0x88 + 2u * (uint32_t)w);
              if (memcmp(k, pk, sizeof k)) { memcpy(pk, k, sizeof k);
                  fprintf(stderr, "dsp: frame %u mixer: volumes %.3f %.3f %.3f aux bus %d %d delay enable %u %u reverb %s\n", frame, vol[0], vol[1], vol[2],
                          aux_on[0], aux_on[1], k[2], k[3], k[7] ? "configured (not implemented)" : "off"); } }
          wr32(dc, 0); }
        audio_open(); memset(mixq, 0, sizeof mixq);
        for (int i = 0; i < N_SRC && !nosrc; i++) {
            Src *s = &src[i];
            if (!s->rate) s->rate = 1.0f;
            src_parse(s, rd + CFG_OFF + 192u * (uint32_t)i, rd + COEF_OFF + 32u * (uint32_t)i);
            if (s->enabled) src_frame(s);
            else for (int m = 0; m < 3; m++) { s->ramp[m] = 0; memcpy(s->gstart[m], s->gain[m], 16); }
            uint32_t st = wr + STAT_OFF + 12u * (uint32_t)i;
            g_mem[st] = (uint8_t)s->enabled; g_mem[st + 1] = (uint8_t)s->buffer_update; if (notify) s->buffer_update = 0;
            wr16(st + 2, s->sync); dsp_w32(st + 4, (uint32_t)s->cur_pos);
            wr16(st + 8, s->cur_id); wr16(st + 10, s->last_id);
            if (dsp_dbg) {                /* PWR_DSP_TRACE=1: one line when a source's enable/buffer/queue state changes */
                static uint32_t prev[N_SRC]; uint32_t key = (uint32_t)s->enabled | (uint32_t)s->cur_id << 1 | (uint32_t)s->last_id << 17 ^ (uint32_t)s->nq << 28;
                if (key != prev[i]) { prev[i] = key; fprintf(stderr, "dsp: frame %u src %d enabled=%d cur_id=%u last_id=%u queue=%d sync=%u rate=%.3f cur_len=%u pos=%.0f\n",
                    frame, i, s->enabled, s->cur_id, s->last_id, s->nq, s->sync, s->rate, s->cur.len, s->cur_pos); }
            }
        }
        for (int b = 0; b < 2; b++) if (aux_on[b]) {   /* aux bus: send this frame's mix, take back what the application returned */
            uint32_t sendp = wr + IMIX_OFF + 0xA00u * (uint32_t)b, retp = rd + IMIX_OFF + 0xA00u * (uint32_t)b;
            for (int k = 0; k < 160; k++) for (int ch = 0; ch < 4; ch++) {
                wr32(sendp + 4u * (uint32_t)(ch * 160 + k), (uint32_t)mixq[b + 1][k][ch]);
                mixq[b + 1][k][ch] = (int32_t)rd32(retp + 4u * (uint32_t)(ch * 160 + k));
            }
        }
        audio_push(vol);
        frame++;
        if (dsp_dbg && frame % 500 == 0) { struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn); fprintf(stderr, "dsp: t=%.2f frame %u, ring fill %u, consumed %u\n", (double)tn.tv_sec + tn.tv_nsec * 1e-9, frame, rhead - rtail, rtail); }
        if (notify && pipe_irq[2]) k_signal(pipe_irq[2]);
    }
    pthread_mutex_unlock(&D);
}

/* the DSP's clock: one frame every 160 samples at 32728 Hz, on its own thread (from the 60 Hz main loop the frames came in bursts of 3-4,
   and all but the first of a burst were skipped by the pacing above, so audio ran at 60 frames/s instead of 200) */
static void *dsp_clock(void *arg) {
    (void)arg; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double next = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
    const char *hz = getenv("PWR_DSP_HZ");   /* A/B: tick rate (default 32728/160 = 204.55); the guest sound engine runs once per tick */
    double period = hz ? 1.0 / atof(hz) : 160.0 / 32728.0, fill = TARGET_FILL;
    for (;;) {
        /* with a sound device, its clock sets the pace: run up to 5% fast or slow to keep the ring near TARGET_FILL (our timer and
           the device's crystal drift apart, and a ring that runs dry is a ~50 ms gap, heard as a click) */
        double adj = 0;
        if (adev) {
            fill += ((double)(rhead - rtail) - fill) * 0.01;
            adj = (fill - TARGET_FILL) / TARGET_FILL * 0.1; adj = adj > 0.05 ? 0.05 : adj < -0.05 ? -0.05 : adj;
        }
        next += period * (1 + adj);
        clock_gettime(CLOCK_MONOTONIC, &ts); double now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
        if (next > now) { struct timespec d = { 0, (long)((next - now) * 1e9) }; nanosleep(&d, NULL); }
        else if (now - next > 0.25) next = now;
        dsp_tick();
    }
    return NULL;
}

void dsp_handle(KThread *t, uint32_t *cmd, KObject *s) {
    static int clock_started;
    if (!clock_started) { clock_started = 1; pthread_t th; pthread_create(&th, NULL, dsp_clock, NULL); pthread_detach(th); }
    (void)t; (void)s;
    uint32_t id = cmd[0] >> 16;
    pthread_mutex_lock(&D);
    switch (id) {
    case 0x01: cmd[0] = HDR(1, 2, 0); cmd[1] = RES_OK; cmd[2] = 1; break;               /* RecvData(register) */
    case 0x02: cmd[0] = HDR(2, 2, 0); cmd[1] = RES_OK; cmd[2] = 1; break;               /* RecvDataIsReady */
    case 0x03: case 0x07: case 0x12: case 0x13: case 0x14: case 0x17: case 0x21:
        cmd[0] = HDR(id, 1, 0); cmd[1] = RES_OK; break;  /* SendData, SetSemaphore, Unload, Flush/Invalidate, SetSemaphoreMask, ForceHeadphoneOut */
    case 0x04: cmd[0] = HDR(4, 2, 0); cmd[1] = RES_OK; cmd[2] = 1; break;               /* SendDataIsEmpty */
    case 0x0C: { uint32_t a = cmd[1];                                                   /* ConvertProcessAddressFromDspDram */
        cmd[0] = HDR(0xC, 2, 0); cmd[1] = RES_OK; cmd[2] = (a << 1) + 0x1FF40000u; break; }
    case 0x0D: { uint32_t ch = cmd[1], size = cmd[2], buf = cmd[4];                     /* WriteProcessPipe(channel, size, buffer) */
        if (ch == 2) audio_pipe_write(g_mem + buf, size);
        cmd[0] = HDR(0xD, 1, 0); cmd[1] = RES_OK; break; }
    case 0x10: case 0x0E: {                                                             /* ReadPipeIfPossible / ReadPipe(channel, peer, size) */
        uint32_t ch = cmd[1], size = cmd[3] & 0xffff, buf = cmd[0x41], n = 0;
        if (ch == 2) { n = size < pipe2_len ? size : pipe2_len; if (buf) memcpy(g_mem + buf, pipe2_out, n); memmove(pipe2_out, pipe2_out + n, pipe2_len - n); pipe2_len -= n; }
        cmd[0] = HDR(id, 2, 2); cmd[1] = RES_OK; cmd[2] = n; cmd[3] = (n << 14) | 2; cmd[4] = buf; break; }
    case 0x0F: cmd[0] = HDR(0xF, 2, 0); cmd[1] = RES_OK; cmd[2] = (cmd[1] == 2) ? pipe2_len : 0; break;   /* GetPipeReadableSize */
    case 0x11: cmd[0] = HDR(0x11, 2, 2); cmd[1] = RES_OK; cmd[2] = 1; cmd[3] = (cmd[1] << 4) | 0xA; break;   /* LoadComponent -> loaded */
    case 0x15: { uint32_t irq = cmd[1], ch = cmd[2]; KObject *e = k_obj(cmd[4]);       /* RegisterInterruptEvents(interrupt, channel, event) */
        if (irq == 2 && ch < 4) pipe_irq[ch] = e;
        k_trace("dsp: RegisterInterruptEvents irq=%u ch=%u event=%x", irq, ch, cmd[4]);
        cmd[0] = HDR(0x15, 1, 0); cmd[1] = RES_OK; break; }
    case 0x16: if (!sem_event) { sem_event = k_alloc(KO_EVENT); }                      /* GetSemaphoreEventHandle */
        cmd[0] = HDR(0x16, 1, 2); cmd[1] = RES_OK; cmd[2] = 0; cmd[3] = k_new_handle(sem_event); sem_event->refs++; break;
    case 0x1F: cmd[0] = HDR(0x1F, 2, 0); cmd[1] = RES_OK; cmd[2] = 0; break;            /* GetHeadphoneStatus: none */
    default:
        fprintf(stderr, "svc: unhandled dsp::DSP command %08x (replying success)\n", cmd[0]);
        cmd[0] = HDR(id, 1, 0); cmd[1] = RES_OK;
    }
    pthread_mutex_unlock(&D);
}

/* ---- save states (state.h) ---- */
void dsp_pause(int on) { if (on) pthread_mutex_lock(&D); else pthread_mutex_unlock(&D); }
void dsp_state_save(SBuf *b) {   /* under D (dsp_pause) */
    for (int i = 0; i < 4; i++) sb_u32(b, ks_id(pipe_irq[i]));
    sb_u32(b, ks_id(sem_event)); sb_u32(b, (uint32_t)running); sb_put(b, pipe2_out, sizeof pipe2_out); sb_u32(b, pipe2_len); sb_u32(b, frame);
    sb_put(b, src, sizeof src); sb_put(b, dsp_vol, sizeof dsp_vol); sb_put(b, dsp_aux_on, sizeof dsp_aux_on);
}
void dsp_state_load(SRd *r) {
    for (int i = 0; i < 4; i++) pipe_irq[i] = ks_obj(sr_u32(r));
    sem_event = ks_obj(sr_u32(r)); running = (int)sr_u32(r); sr_get(r, pipe2_out, sizeof pipe2_out); pipe2_len = sr_u32(r); frame = (uint16_t)sr_u32(r);
    if (pipe2_len > sizeof pipe2_out) pipe2_len = 0;
    sr_get(r, src, sizeof src); sr_get(r, dsp_vol, sizeof dsp_vol); sr_get(r, dsp_aux_on, sizeof dsp_aux_on);
    rtail = rhead;   /* what was queued for the speakers belongs to the other timeline */
}
