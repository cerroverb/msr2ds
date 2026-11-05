/* fs:USER: archives and files (from Woohoo! Flight School).
 *   0x3 SelfNCCH  -> the RomFS level-3 image read straight out of the dump (g_dump at g_romfs_offset, main.c; a
 *                   compressed dump through its frames, dump.c; the SDK walks its tables itself)
 *   0x4 SaveData, 0x6/0x7 ExtSaveData, 0x9 SDMC -> host folders under $PWR_SAVE_DIR (default ./save)
 * File and directory handles are sessions answering the File/Directory command sets. */
#define _GNU_SOURCE
#include "kernel.h"
#include "runtime.h"
#include "dump.h"
#include "msr/msr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#define HDR(id, normal, translate) (((uint32_t)(id) << 16) | ((normal) << 6) | (translate))
#define RES_NOT_FOUND   0xC8804478u
#define RES_EXISTS      0xC82044BEu
#define RES_ARCH_NOT_FOUND 0xC8804464u
#define RES_UNSUPPORTED 0xE0C04702u

const char *g_romfs_path;          /* the dump */
uint64_t g_romfs_offset, g_romfs_size; /* its RomFS level-3 image */

typedef struct { uint32_t id; char root[1024]; } Archive;   /* root empty = SelfNCCH */
#define MAX_ARCH 32
static Archive archives[MAX_ARCH];

typedef struct { FILE *f; int romfs, write; char path[1600]; int ropen; } FsFile;   /* path, write: to open it again
    (save states); the RomFS has no FILE: it reads through the game file's handle (g_dump, dump.c), ropen while open */
typedef struct { DIR *d; char path[1200]; } FsDir;

static const char *save_root(void) { const char *s = getenv("PWR_SAVE_DIR"); return s ? s : "save"; }

static void mkdirs(const char *p) {
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s", p);
    for (char *q = tmp + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    mkdir(tmp, 0755);
}

/* FS path (type, size, guest pointer) -> host string. type: 1 empty, 2 binary, 3 ASCII, 4 UTF-16 */
static void fs_path(uint32_t type, uint32_t size, uint32_t ptr, char *out, size_t cap) {
    out[0] = 0;
    if (type == 3) { snprintf(out, cap, "%.*s", (int)size, (const char *)(g_mem + ptr)); }
    else if (type == 4) {
        size_t o = 0;
        for (uint32_t i = 0; i + 1 < size && o + 4 < cap; i += 2) {
            uint32_t ch = rd16(ptr + i); if (!ch) break;
            if (ch < 0x80) out[o++] = (char)ch;
            else if (ch < 0x800) { out[o++] = (char)(0xc0 | ch >> 6); out[o++] = (char)(0x80 | (ch & 63)); }
            else { out[o++] = (char)(0xe0 | ch >> 12); out[o++] = (char)(0x80 | ((ch >> 6) & 63)); out[o++] = (char)(0x80 | (ch & 63)); }
        }
        out[o] = 0;
    } else if (type == 2) {   /* binary: hex-encode so it can name a folder */
        size_t o = 0; for (uint32_t i = 0; i < size && o + 3 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%02x", rd8(ptr + i));
    }
}

static uint32_t open_archive(uint32_t id, uint32_t ptype, uint32_t psize, uint32_t pptr, uint64_t *handle) {
    int slot = -1;
    for (int i = 1; i < MAX_ARCH; i++) if (!archives[i].id) { slot = i; break; }
    if (slot < 0) return RES_UNSUPPORTED;
    Archive *a = &archives[slot]; a->id = id; a->root[0] = 0;
    if (id != 3) {
        char sub[512]; fs_path(ptype, psize, pptr, sub, sizeof sub);
        const char *kind = id == 4 ? "savedata" : id == 9 ? "sdmc" : (id == 6 || id == 7) ? "extdata" : NULL;
        if (!kind) { a->id = 0; fprintf(stderr, "svc: fs: archive id %#x not supported\n", id); return RES_UNSUPPORTED; }
        snprintf(a->root, sizeof a->root, "%s/%s%s%s", save_root(), kind, sub[0] ? "/" : "", sub);
        mkdirs(a->root);
    }
    *handle = (uint64_t)slot;
    k_trace("fs: OpenArchive id=%x -> %d root=%s", id, slot, a->root[0] ? a->root : "(romfs)");
    return RES_OK;
}

static void file_handle(KThread *t, uint32_t *cmd, KObject *s);
static void dir_handle(KThread *t, uint32_t *cmd, KObject *s);
static const Service file_service = { "fs:file", file_handle };
static const Service dir_service = { "fs:dir", dir_handle };

/* open a file in an archive; returns result and a session handle */
static uint32_t open_file(Archive *a, uint32_t ptype, uint32_t psize, uint32_t pptr, uint32_t flags, uint32_t *out) {
    FsFile *ff = calloc(1, sizeof *ff);
    if (a->id == 3) {
        /* SelfNCCH: the binary file path selects RomFS (type 0); other NCCH parts are not needed by the game */
        ff->romfs = 1; ff->ropen = 1;
        if (!g_dump) { free(ff); fprintf(stderr, "svc: fs: cannot open the RomFS in %s\n", g_romfs_path ? g_romfs_path : "(no dump)"); return RES_NOT_FOUND; }
    } else {
        char rel[512], p[1600]; fs_path(ptype, psize, pptr, rel, sizeof rel);
        snprintf(p, sizeof p, "%s%s", a->root, rel);
        int create = flags & 4, write = flags & 2;
        size_t pl = strlen(p);
        ff->f = fopen(p, write ? "r+b" : "rb");
        if (!ff->f && create) ff->f = fopen(p, "w+b");
        snprintf(ff->path, sizeof ff->path, "%s", p); ff->write = write != 0;
        k_trace("fs: OpenFile %s flags=%x -> %s", p, flags, ff->f ? "ok" : "not found");
        if (!ff->f) { free(ff); return RES_NOT_FOUND; }
    }
    KObject *o = k_alloc(KO_SESSION); o->svc = &file_service; o->svc_state = ff;
    *out = k_new_handle(o);
    return RES_OK;
}

static Archive *arch(uint32_t lo) { return lo > 0 && lo < MAX_ARCH && archives[lo].id ? &archives[lo] : NULL; }

static void host_path(Archive *a, uint32_t ptype, uint32_t psize, uint32_t pptr, char *p, size_t cap) {
    char rel[512]; fs_path(ptype, psize, pptr, rel, sizeof rel); snprintf(p, cap, "%s%s", a->root, rel);
}

void fs_user_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; (void)s;
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x801: cmd[0] = HDR(0x801, 1, 0); cmd[1] = RES_OK; return;                                /* Initialize */
    case 0x839: cmd[0] = HDR(0x839, 1, 0); cmd[1] = RES_OK; return;                                /* InitializeWithSdkVersion */
    case 0x802: {                                                                                  /* OpenFile */
        Archive *a = arch(cmd[2]); uint32_t h = 0, r = a ? open_file(a, cmd[4], cmd[5], cmd[9], cmd[6], &h) : RES_ARCH_NOT_FOUND;
        cmd[0] = HDR(0x802, 1, 2); cmd[1] = r; cmd[2] = 0x10; cmd[3] = h; return;
    }
    case 0x803: {                                                                                  /* OpenFileDirectly */
        uint64_t ah; uint32_t h = 0;
        uint32_t r = open_archive(cmd[2], cmd[3], cmd[4], cmd[10], &ah);
        if (r == RES_OK) { r = open_file(&archives[ah], cmd[5], cmd[6], cmd[12], cmd[7], &h); archives[ah].id = 0; }
        cmd[0] = HDR(0x803, 1, 2); cmd[1] = r; cmd[2] = 0x10; cmd[3] = h; return;
    }
    case 0x804: {                                                                                  /* DeleteFile */
        Archive *a = arch(cmd[2]); char p[1600]; if (a) host_path(a, cmd[4], cmd[5], cmd[7], p, sizeof p);
        cmd[0] = HDR(0x804, 1, 0); cmd[1] = a && !remove(p) ? RES_OK : RES_NOT_FOUND; return;
    }
    case 0x806: case 0x809: {                                                                      /* DeleteDirectory / CreateDirectory */
        Archive *a = arch(cmd[2]); char p[1600]; if (a) host_path(a, cmd[4], cmd[5], id == 0x806 ? cmd[7] : cmd[8], p, sizeof p);
        int ok = a && (id == 0x806 ? rmdir(p) == 0 : (mkdir(p, 0755) == 0 || errno == EEXIST));
        cmd[0] = HDR(id, 1, 0); cmd[1] = ok ? RES_OK : RES_NOT_FOUND; return;
    }
    case 0x808: {                                                                                  /* CreateFile(size) */
        Archive *a = arch(cmd[2]); char p[1600]; uint32_t r = RES_ARCH_NOT_FOUND;
        if (a) {
            host_path(a, cmd[4], cmd[5], cmd[10], p, sizeof p);
            FILE *f = fopen(p, "rb");
            if (f) { fclose(f); r = RES_EXISTS; }
            else if ((f = fopen(p, "wb"))) { uint64_t sz = cmd[7] | ((uint64_t)cmd[8] << 32); if (sz) { fseek(f, (long)sz - 1, SEEK_SET); fputc(0, f); } fclose(f); r = RES_OK; }
            k_trace("fs: CreateFile %s -> %x", p, r);
        }
        cmd[0] = HDR(0x808, 1, 0); cmd[1] = r; return;
    }
    case 0x80B: {                                                                                  /* OpenDirectory */
        Archive *a = arch(cmd[1]); uint32_t h = 0, r = RES_NOT_FOUND;
        if (a) {
            FsDir *fd = calloc(1, sizeof *fd); host_path(a, cmd[3], cmd[4], cmd[6], fd->path, sizeof fd->path);
            fd->d = opendir(fd->path);
            if (fd->d) { KObject *o = k_alloc(KO_SESSION); o->svc = &dir_service; o->svc_state = fd; h = k_new_handle(o); r = RES_OK; }
            else free(fd);
        }
        cmd[0] = HDR(0x80B, 1, 2); cmd[1] = r; cmd[2] = 0x10; cmd[3] = h; return;
    }
    case 0x80C: {                                                                                  /* OpenArchive */
        uint64_t ah = 0; uint32_t r = open_archive(cmd[1], cmd[2], cmd[3], cmd[5], &ah);
        cmd[0] = HDR(0x80C, 3, 0); cmd[1] = r; cmd[2] = (uint32_t)ah; cmd[3] = (uint32_t)(ah >> 32); return;
    }
    case 0x80D: cmd[0] = HDR(0x80D, 1, 0); cmd[1] = RES_OK; return;                                /* ControlArchive (commit) */
    case 0x80E: { Archive *a = arch(cmd[1]); if (a) a->id = 0; cmd[0] = HDR(0x80E, 1, 0); cmd[1] = RES_OK; return; }  /* CloseArchive */
    case 0x80F: { char p[1100]; snprintf(p, sizeof p, "%s/savedata", save_root()); mkdirs(p);   /* FormatThisUserSaveData */
        cmd[0] = HDR(0x80F, 1, 0); cmd[1] = RES_OK; return; }
    case 0x812: cmd[0] = HDR(0x812, 3, 0); cmd[1] = RES_OK; cmd[2] = 0x10000000; cmd[3] = 0; return;  /* GetFreeBytes */
    case 0x817: cmd[0] = HDR(0x817, 2, 0); cmd[1] = RES_OK; cmd[2] = 0; return;                    /* IsSdmcDetected: no */
    case 0x818: cmd[0] = HDR(0x818, 2, 0); cmd[1] = RES_OK; cmd[2] = 0; return;                    /* IsSdmcWritable */
    case 0x83F: cmd[0] = HDR(0x83F, 1, 0); cmd[1] = RES_OK; return;
    default:
        fprintf(stderr, "svc: unhandled fs:USER command %08x (replying success)\n", cmd[0]);
        cmd[0] = HDR(id, 1, 0); cmd[1] = RES_OK;
    }
}

static void file_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; FsFile *ff = s->svc_state; uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x802: {   /* Read(offset u64, size, buffer) -> bytes read */
        uint64_t off = cmd[1] | ((uint64_t)cmd[2] << 32); uint32_t size = cmd[3], buf = cmd[5];
        if (ff->romfs && msr_romfs_layered()) {   /* rebuilt with the game mods' files (msr/layered_fs.cpp) */
            rt_mem_commit(buf, size);
            size_t n = msr_romfs_read(off, size, g_mem + buf);
            cmd[0] = HDR(0x802, 2, 2); cmd[1] = RES_OK; cmd[2] = (uint32_t)n; cmd[3] = (size << 4) | 0xC; cmd[4] = buf; return;
        }
        size_t n;
        if (ff->romfs) {   /* the image inside the game file (a compressed one too: dump.c): no reads past its end */
            if (off > g_romfs_size) off = g_romfs_size;
            if (size > g_romfs_size - off) size = (uint32_t)(g_romfs_size - off);
            rt_mem_commit(buf, size);
            n = dump_read(g_dump, g_romfs_offset + off, g_mem + buf, size) ? 0 : size;
        } else {
            fseeko(ff->f, (off_t)off, SEEK_SET);
            rt_mem_commit(buf, size);
            n = fread(g_mem + buf, 1, size, ff->f);
        }
        cmd[0] = HDR(0x802, 2, 2); cmd[1] = RES_OK; cmd[2] = (uint32_t)n; cmd[3] = (size << 4) | 0xC; cmd[4] = buf; return;
    }
    case 0x803: {   /* Write(offset u64, size, flush, buffer) -> bytes written */
        uint64_t off = cmd[1] | ((uint64_t)cmd[2] << 32); uint32_t size = cmd[3], buf = cmd[6];
        size_t n = 0;
        if (!ff->romfs) { fseeko(ff->f, (off_t)off, SEEK_SET); rt_mem_commit(buf, size); n = fwrite(g_mem + buf, 1, size, ff->f); fflush(ff->f); }
        cmd[0] = HDR(0x803, 2, 2); cmd[1] = ff->romfs ? RES_UNSUPPORTED : RES_OK; cmd[2] = (uint32_t)n; cmd[3] = (size << 4) | 0xA; cmd[4] = buf; return;
    }
    case 0x804: {   /* GetSize */
        uint64_t sz = msr_romfs_layered() ? msr_romfs_size() : g_romfs_size;
        if (!ff->romfs) { off_t cur = ftello(ff->f); fseeko(ff->f, 0, SEEK_END); sz = (uint64_t)ftello(ff->f); fseeko(ff->f, cur, SEEK_SET); }
        cmd[0] = HDR(0x804, 3, 0); cmd[1] = RES_OK; cmd[2] = (uint32_t)sz; cmd[3] = (uint32_t)(sz >> 32); return;
    }
    case 0x805: {   /* SetSize */
        uint64_t sz = cmd[1] | ((uint64_t)cmd[2] << 32);
        if (!ff->romfs) { fflush(ff->f); if (ftruncate(fileno(ff->f), (off_t)sz)) {} }
        cmd[0] = HDR(0x805, 1, 0); cmd[1] = RES_OK; return;
    }
    case 0x808: if (ff->f) fclose(ff->f); ff->f = NULL; ff->ropen = 0; cmd[0] = HDR(0x808, 1, 0); cmd[1] = RES_OK; return;   /* Close */
    case 0x809: if (ff->f) fflush(ff->f); cmd[0] = HDR(0x809, 1, 0); cmd[1] = RES_OK; return;                  /* Flush */
    default:
        fprintf(stderr, "svc: unhandled fs file command %08x (replying success)\n", cmd[0]);
        cmd[0] = HDR(id, 1, 0); cmd[1] = RES_OK;
    }
}

static void dir_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; FsDir *fd = s->svc_state; uint32_t id = cmd[0] >> 16;
    if (id == 0x801) {   /* Read(count, buffer) -> entries (0x228 bytes each: UTF-16 name[0x106], short name, flags, size) */
        uint32_t count = cmd[1], buf = cmd[3], n = 0; struct dirent *e;
        while (n < count && fd->d && (e = readdir(fd->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            uint32_t ent = buf + n * 0x228; memset(g_mem + ent, 0, 0x228);
            for (int i = 0; e->d_name[i] && i < 0x105; i++) wr16(ent + 2u * (uint32_t)i, (uint8_t)e->d_name[i]);
            char p[1500]; struct stat st; snprintf(p, sizeof p, "%s/%s", fd->path, e->d_name);
            if (!stat(p, &st)) { wr8(ent + 0x21C, S_ISDIR(st.st_mode)); wr32(ent + 0x220, (uint32_t)st.st_size); }
            n++;
        }
        cmd[0] = HDR(0x801, 2, 2); cmd[1] = RES_OK; cmd[2] = n; cmd[3] = ((count * 0x228) << 4) | 0xC; cmd[4] = buf; return;
    }
    if (id == 0x802) { if (fd->d) closedir(fd->d); fd->d = NULL; cmd[0] = HDR(0x802, 1, 0); cmd[1] = RES_OK; return; }   /* Close */
    fprintf(stderr, "svc: unhandled fs directory command %08x (replying success)\n", cmd[0]);
    cmd[0] = HDR(id, 1, 0); cmd[1] = RES_OK;
}

/* ---- save states (state.h): the archives, and the open files and folders, opened again on load ---- */
#include "state.h"
const Service *fs_service_find(const char *name) {
    return !strcmp(name, file_service.name) ? &file_service : !strcmp(name, dir_service.name) ? &dir_service : NULL;
}
void fs_state_save(SBuf *b) { sb_put(b, archives, sizeof archives); }
void fs_state_load(SRd *r) { sr_get(r, archives, sizeof archives); }
int fs_session_save(KObject *o, SBuf *b) {
    if (o->svc == &file_service) {
        FsFile *ff = o->svc_state; uint32_t kind = 1, open = ff->romfs ? ff->ropen != 0 : ff->f != NULL;
        sb_u32(b, kind); sb_u32(b, (uint32_t)ff->romfs); sb_u32(b, (uint32_t)ff->write); sb_u32(b, open); sb_put(b, ff->path, sizeof ff->path);
        return 1;
    }
    if (o->svc == &dir_service) {
        FsDir *fd = o->svc_state; uint32_t kind = 2, open = fd->d != NULL;
        sb_u32(b, kind); sb_u32(b, 0); sb_u32(b, 0); sb_u32(b, open); sb_put(b, fd->path, sizeof fd->path);
        return 1;
    }
    return 0;
}
void fs_session_load(KObject *o, const char *name, SRd *r) {
    (void)name;
    uint32_t kind = sr_u32(r), romfs = sr_u32(r), write = sr_u32(r), open = sr_u32(r);
    if (kind == 1) {
        FsFile *ff = calloc(1, sizeof *ff); sr_get(r, ff->path, sizeof ff->path); ff->path[sizeof ff->path - 1] = 0;
        ff->romfs = (int)romfs; ff->write = (int)write;
        if (romfs) ff->ropen = (int)open;   /* it reads through g_dump */
        else if (open) ff->f = fopen(ff->path, write ? "r+b" : "rb");
        if (open && (romfs ? !g_dump : !ff->f)) fprintf(stderr, "state: %s could not be opened again\n", romfs ? "the RomFS" : ff->path);
        o->svc = &file_service; o->svc_state = ff;
    } else if (kind == 2) {
        FsDir *fd = calloc(1, sizeof *fd); sr_get(r, fd->path, sizeof fd->path); fd->path[sizeof fd->path - 1] = 0;
        if (open) fd->d = opendir(fd->path);
        o->svc = &dir_service; o->svc_state = fd;
    }
}
