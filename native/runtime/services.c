/* IPC services. Each handler reads the command buffer (header = id<<16 | normal<<6 | translate) and writes the
 * reply in place. Commands without an implementation are logged once and answered with success and no outputs,
 * so boot can progress and the log shows what to implement next. */
#include "kernel.h"
#include "input.h"
#include "state.h"
#include "msr/msr.h"
#include <stdio.h>
#include <string.h>

#define HDR(id, normal, translate) (((uint32_t)(id) << 16) | ((normal) << 6) | (translate))

static void unhandled(const char *svc, uint32_t *cmd) {
    static uint32_t seen[256][2]; static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i][0] == cmd[0] && seen[i][1] == (uint32_t)(uintptr_t)svc) goto reply;
    if (nseen < 256) { seen[nseen][0] = cmd[0]; seen[nseen][1] = (uint32_t)(uintptr_t)svc; nseen++; }
    fprintf(stderr, "svc: unhandled %s command %08x (replying success)\n", svc, cmd[0]);
reply:
    cmd[0] = HDR(cmd[0] >> 16, 1, 0); cmd[1] = RES_OK;
}

/* ---- srv: ---- */
static void srv_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; (void)s;
    switch (cmd[0]) {
    case 0x00010002: cmd[0] = HDR(1, 1, 0); cmd[1] = RES_OK; return;                 /* RegisterClient */
    case 0x00020000: { KObject *o = k_alloc(KO_SEMAPHORE); o->max = 0x7fffffff;        /* EnableNotification */
        cmd[0] = HDR(2, 1, 2); cmd[1] = RES_OK; cmd[2] = 0x20; cmd[3] = k_new_handle(o); return; }
    case 0x00050100: {                                                                  /* GetServiceHandle */
        char name[9] = {0}; memcpy(name, &cmd[1], 8); uint32_t len = cmd[3]; if (len < 9) name[len] = 0;
        uint32_t h = k_session_for(name);
        k_trace("srv:GetServiceHandle(\"%s\") -> %x", name, h);
        if (!h) fprintf(stderr, "svc: srv:GetServiceHandle(\"%s\"): no such service\n", name);
        cmd[0] = HDR(5, 1, 2); cmd[1] = h ? RES_OK : 0xD8E06406u; cmd[2] = 0; cmd[3] = h; return;
    }
    case 0x000B0000: cmd[0] = HDR(0xB, 2, 0); cmd[1] = RES_OK; cmd[2] = 0; return;    /* ReceiveNotification */
    default: unhandled("srv:", cmd);
    }
}

static void generic_handle(KThread *t, uint32_t *cmd, KObject *s) { (void)t; unhandled(s->svc->name, cmd); }

/* ---- APT:U (applet manager). The app is started by the HOME Menu, which sends it a Wakeup parameter. ---- */
static struct {
    uint32_t lock, notif_event, param_event;
    int param_pending;           /* a parameter is waiting to be received */
    uint32_t param_cmd;          /* 1 = Wakeup */
} apt;

static void apt_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; (void)s;
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x01:   /* GetLockHandle(attr) -> attr, state, lock mutex */
        if (!apt.lock) { KObject *m = k_alloc(KO_MUTEX); apt.lock = k_new_handle(m); }
        cmd[0] = HDR(1, 3, 2); cmd[1] = RES_OK; cmd[2] = 0 /* attr */; cmd[3] = 0 /* state */; cmd[4] = 0x04000000; cmd[5] = apt.lock; return;
    case 0x02: { /* Initialize(appid, attr) -> notification event, parameter event */
        KObject *n = k_alloc(KO_EVENT), *p = k_alloc(KO_EVENT);
        apt.notif_event = k_new_handle(n); apt.param_event = k_new_handle(p);
        /* the HOME Menu immediately hands the application its Wakeup parameter: only the parameter event fires;
           the notification event stays clear until there is a notification (InquireNotification has none) */
        apt.param_pending = 1; apt.param_cmd = 1;
        p->signaled = 1;
        cmd[0] = HDR(2, 1, 3); cmd[1] = RES_OK; cmd[2] = 0x04000000; cmd[3] = apt.notif_event; cmd[4] = apt.param_event; return;
    }
    case 0x03: cmd[0] = HDR(3, 1, 0); cmd[1] = RES_OK; return;                       /* Enable */
    case 0x0B: cmd[0] = HDR(0xB, 2, 0); cmd[1] = RES_OK; cmd[2] = 0; return;          /* InquireNotification: none */
    case 0x0D: case 0x0E: {  /* ReceiveParameter / GlanceParameter(appid, buffer size) */
        uint32_t c_ = apt.param_pending ? apt.param_cmd : 0;
        if (id == 0x0D) apt.param_pending = 0;
        cmd[0] = HDR(id, 4, 4); cmd[1] = RES_OK;
        cmd[2] = 0;          /* sender app id */
        cmd[3] = c_;         /* signal type: 1 = Wakeup */
        cmd[4] = 0;          /* parameter size */
        cmd[5] = 0x10; cmd[6] = 0;   /* handle descriptor + handle (none) */
        cmd[7] = 0; cmd[8] = 0;      /* static buffer descriptor (no data) */
        return;
    }
    case 0x43: cmd[0] = HDR(0x43, 1, 0); cmd[1] = RES_OK; return;                     /* NotifyToWait */
    case 0x4B: cmd[0] = HDR(0x4B, 2, 0); cmd[1] = RES_OK; cmd[2] = 0; return;         /* AppletUtility */
    case 0x3E: cmd[0] = HDR(0x3E, 1, 0); cmd[1] = RES_OK; return;                     /* ReplySleepQuery */
    case 0x3F: cmd[0] = HDR(0x3F, 1, 0); cmd[1] = RES_OK; return;                     /* ReplySleepNotificationComplete */
    default: unhandled("APT:U", cmd);
    }
}

/* ---- cfg:u: system configuration blocks ---- */
static void cfg_block(uint32_t id, uint8_t *out, uint32_t size) {
    memset(out, 0, size);
    switch (id) {
    case 0x00050005: {   /* stereo camera settings (8 floats); defaults as documented by Citra */
        static const float st[8] = { 62.0f, 289.0f, 76.80000305f, 46.08000183f, 10.0f, 5.0f, 55.58000183f, 21.56999969f };
        memcpy(out, st, size < sizeof st ? size : sizeof st); return;
    }
    case 0x000A0002: out[0] = (uint8_t)msr_settings_language(); return;   /* language: the launcher's (English by default) */
    case 0x000B0000: if (size >= 4) { out[2] = 0; out[3] = 49; } return; /* country: USA (49), no province */
    case 0x000A0000: { const char *n = "PWR"; for (int i = 0; n[i] && 2u * (uint32_t)i + 1 < size; i++) out[2 * i] = (uint8_t)n[i]; return; }   /* username (UTF-16) */
    case 0x000A0001: if (size >= 2) { out[0] = 1; out[1] = 1; } return;   /* birthday: month, day */
    case 0x00070001: out[0] = 1; return;                               /* sound output: stereo */
    case 0x000F0004: out[0] = 0; return;                               /* model: original 3DS */
    case 0x00130000: return;                                           /* (zeros) */
    default: {
        static uint32_t seen[64]; static int n;
        for (int i = 0; i < n; i++) if (seen[i] == id) return;
        if (n < 64) seen[n++] = id;
        fprintf(stderr, "svc: cfg block %08x (size %u) not known, returning zeros\n", id, size);
    }
    }
}

static void cfg_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; (void)s;
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x01: { uint32_t size = cmd[1], blk = cmd[2], ptr = cmd[4];          /* GetConfigInfoBlk2(size, block, buffer) */
        cfg_block(blk, g_mem + ptr, size);
        k_trace("cfg: GetConfigInfoBlk2 %08x size %u", blk, size);
        cmd[0] = HDR(1, 1, 2); cmd[1] = RES_OK; cmd[2] = (size << 4) | 0xC; cmd[3] = ptr; return; }
    /* the region: USA, or Europe for German, Italian and Dutch, the languages the game shows only there (the country
       stays USA, as the emulator-based 1.x left it) */
    case 0x02: cmd[0] = HDR(2, 2, 0); cmd[1] = RES_OK; cmd[2] = (uint32_t)msr_settings_region(); return;   /* SecureInfoGetRegion */
    case 0x03: cmd[0] = HDR(3, 3, 0); cmd[1] = RES_OK; cmd[2] = 0x12345678; cmd[3] = 0x9abcdef0 & 0xffff; return;   /* GenHashConsoleUnique */
    case 0x04: cmd[0] = HDR(4, 2, 0); cmd[1] = RES_OK; cmd[2] = msr_settings_region() == 1; return;   /* GetRegionCanadaUSA */
    case 0x05: cmd[0] = HDR(5, 2, 0); cmd[1] = RES_OK; cmd[2] = 0; return;   /* GetSystemModel: CTR */
    case 0x06: cmd[0] = HDR(6, 2, 0); cmd[1] = RES_OK; cmd[2] = 1; return;   /* GetModelNintendo2DS: 1 = not a 2DS */
    default: unhandled("cfg:u", cmd);
    }
}

/* ---- err:f: the game reports a fatal error; print it so the run log explains the failure ---- */
static void errf_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; (void)s;
    fprintf(stderr, "kernel: err:f fatal error report:");
    for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", cmd[i]);
    fprintf(stderr, "\n");
    cmd[0] = HDR(cmd[0] >> 16, 1, 0); cmd[1] = RES_OK;
}

void fs_user_handle(KThread *t, uint32_t *cmd, KObject *s);
void gsp_handle(KThread *t, uint32_t *cmd, KObject *s);
void dsp_handle(KThread *t, uint32_t *cmd, KObject *s);

/* ---- hid:USER: shared memory with pad/touch/accelerometer rings (filled by input.c) + 5 events ---- */
static uint32_t hid_handles[6];   /* shared memory, PAD0, PAD1, accelerometer, gyroscope, debug pad */
static void hid_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; (void)s;
    uint32_t *handles = hid_handles;
    uint32_t id = cmd[0] >> 16;
    if (id == 0x0A) {   /* GetIPCHandles -> shared memory, PAD0, PAD1, accelerometer, gyroscope, debug pad events */
        if (!handles[0]) {
            KObject *m = k_alloc(KO_SHMEM); m->size = 0x2B0; handles[0] = k_new_handle(m);
            for (int i = 1; i < 6; i++) { KObject *e = k_alloc(KO_EVENT); e->reset_type = 0; handles[i] = k_new_handle(e); }
            input_bind(m, k_obj(handles[1]));
        }
        cmd[0] = HDR(0xA, 1, 7); cmd[1] = RES_OK; cmd[2] = 0x14000000;
        for (int i = 0; i < 6; i++) cmd[3 + i] = handles[i];
        return;
    }
    /* EnableAccelerometer/DisableAccelerometer/EnableGyroscopeLow/... */
    if (id >= 0x11 && id <= 0x16) { cmd[0] = HDR(id, 1, 0); cmd[1] = RES_OK; return; }
    unhandled("hid:USER", cmd);
}

/* replies with an event handle (made once per command id and service) */
static void event_reply(uint32_t *cmd, uint32_t *cache) {
    if (!*cache) { KObject *e = k_alloc(KO_EVENT); *cache = k_new_handle(e); }
    cmd[0] = HDR(cmd[0] >> 16, 1, 2); cmd[1] = RES_OK; cmd[2] = 0; cmd[3] = *cache;
}

/* ---- ir:USER: the Circle Pad Pro, which is never connected ---- */
static uint32_t ir_ev[3], nfc_ev[2];
static void ir_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; (void)s;
    uint32_t *ev = ir_ev;
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x0A: case 0x0B: case 0x0C: event_reply(cmd, &ev[id - 0x0A]); return;   /* Get{Receive,Send,ConnectionStatus}Event */
    default: unhandled("ir:USER", cmd);
    }
}

/* ---- nfc:u: amiibo; no tag is ever in range ---- */
static void nfc_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; (void)s;
    uint32_t *ev = nfc_ev;
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x0B: case 0x0C: event_reply(cmd, &ev[id - 0x0B]); return;   /* GetTagInRangeEvent / GetTagOutOfRangeEvent */
    case 0x0D: cmd[0] = HDR(0xD, 2, 0); cmd[1] = RES_OK; cmd[2] = 0; return;   /* GetTagState: not initialized */
    case 0x0F: cmd[0] = HDR(0xF, 2, 0); cmd[1] = RES_OK; cmd[2] = 0; return;   /* CommunicationGetStatus */
    default: unhandled("nfc:u", cmd);
    }
}

static const Service services[] = {
    { "srv:", srv_handle },
    { "fs:USER", fs_user_handle },
    { "APT:U", apt_handle },
    { "APT:A", apt_handle },
    { "APT:S", apt_handle },
    { "err:f", errf_handle },
    { "gsp::Gpu", gsp_handle },
    { "gsp::Lcd", generic_handle },
    { "dsp::DSP", dsp_handle },
    { "hid:USER", hid_handle },
    { "hid:SPVR", hid_handle },
    { "cfg:u", cfg_handle },
    { "cfg:s", cfg_handle },
    { "cfg:i", cfg_handle },
    { "ndm:u", generic_handle },
    { "ir:USER", ir_handle },
    { "nfc:u", nfc_handle },
    { "ptm:u", generic_handle },
    { "ac:u", generic_handle },
    { "frd:u", generic_handle },
    { "cecd:u", generic_handle },
    { "boss:U", generic_handle },
};

const Service *svc_find(const char *name) {
    for (size_t i = 0; i < sizeof services / sizeof *services; i++)
        if (!strcmp(services[i].name, name)) return &services[i];
    return NULL;
}

/* ---- save states: the services' handles (numbers: the handle table is saved as it is) ---- */
void services_state_save(SBuf *b) {
    SB(b, apt); sb_put(b, hid_handles, sizeof hid_handles); sb_put(b, ir_ev, sizeof ir_ev); sb_put(b, nfc_ev, sizeof nfc_ev);
}
void services_state_load(SRd *r) {
    SR(r, apt); sr_get(r, hid_handles, sizeof hid_handles); sr_get(r, ir_ev, sizeof ir_ev); sr_get(r, nfc_ev, sizeof nfc_ev);
    if (hid_handles[0]) input_bind(k_obj(hid_handles[0]), k_obj(hid_handles[1]));   /* input.c writes the pads there */
}
