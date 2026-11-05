/* The game file: a .3ds/.cci/.cxi read as it is, or Azahar's compressed Z3DS (.zcci, .zcxi) read in place, without
 * unpacking it (2026-11-03, msr2ds issue 22: "Is it possible to add support for compressed .zcci rom files?").
 * Z3DS (Azahar src/common/file_derived.h, Z3DSFileHeader, version 1): a 0x20-byte header
 *   0x00 "Z3DS", 0x04 the file's own magic ("NCSD"...), 0x08 u8 version 1, 0x0A u16 header size, 0x0C u32 metadata size
 *   (padded to 16), 0x10 u64 compressed size, 0x18 u64 uncompressed size,
 * the metadata, then zstd's seekable format (contrib/seekable_format/zstd_seekable_compression_format.md): independent
 * zstd frames (256 KiB of the game file each for a .cci) and, last in the file, a skippable frame (magic 0x184D2A5E)
 * holding a seek table: each frame's compressed and decompressed size (and a checksum when the descriptor's bit 7 says
 * so), then the frame count, the descriptor and 0x8F92EAB1. A read decompresses the frames it covers: whole ones
 * straight into the caller's buffer, partial ones through a few cached frames. zstd's own decoder (libzstd; the build
 * takes the system's or MSR_ZSTD_SOURCE, native/CMakeLists.txt). */
#include "dump.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <zstd.h>

DumpFile *g_dump;

#define CACHE_BYTES (8u << 20)   /* the partly read frames kept decompressed */
#define CACHE_MAX 16

typedef struct { uint32_t frame; uint8_t *data; uint64_t used; } Cached;
struct DumpFile {
    FILE *f;
    pthread_mutex_t lock;
    uint64_t size;            /* the game file's */
    int z3ds;
    uint64_t base;            /* the seekable stream's start in the file */
    uint32_t frames;
    uint64_t *coff, *doff;    /* each frame's start, compressed (from base) and in the game file; frames + 1 entries */
    ZSTD_DCtx *dctx;
    uint8_t *cbuf; size_t cbuf_cap;
    Cached cache[CACHE_MAX]; int cached, cache_cap; uint64_t tick;
};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static int raw_read(FILE *f, uint64_t off, void *dst, size_t n) {
    return fseeko(f, (off_t)off, SEEK_SET) || fread(dst, 1, n, f) != n ? -1 : 0;
}

/* the seek table of a Z3DS file of `file_size` bytes; 0 when it is whole */
static int z3ds_open(DumpFile *d, uint64_t file_size) {
    uint8_t h[0x20], foot[9];
    if (raw_read(d->f, 0, h, sizeof h) || h[8] != 1) return -1;
    const uint64_t header = le32(h + 0x0A) & 0xffff, meta = le32(h + 0x0C);
    d->base = header + meta;
    d->size = le64(h + 0x18);
    if (d->base > file_size || file_size - d->base < 8 + sizeof foot) return -1;
    if (raw_read(d->f, file_size - sizeof foot, foot, sizeof foot) || le32(foot + 5) != 0x8F92EAB1u || (foot[4] & 0x7C)) return -1;
    const uint32_t frames = le32(foot), entry = foot[4] & 0x80 ? 12 : 8;
    if (!frames || frames > 0x8000000u) return -1;
    const uint64_t table = (uint64_t)frames * entry, start = file_size - sizeof foot - table - 8;   /* the skippable frame */
    if (table + sizeof foot + 8 > file_size - d->base) return -1;
    uint8_t skip[8];
    if (raw_read(d->f, start, skip, sizeof skip) || le32(skip) != 0x184D2A5Eu || le32(skip + 4) != table + sizeof foot) return -1;
    uint8_t *t = malloc(table);
    d->coff = malloc(((size_t)frames + 1) * sizeof *d->coff);
    d->doff = malloc(((size_t)frames + 1) * sizeof *d->doff);
    if (!t || !d->coff || !d->doff || raw_read(d->f, start + 8, t, table)) { free(t); return -1; }
    uint64_t c = 0, u = 0;
    uint32_t largest = 0;
    for (uint32_t i = 0; i < frames; i++) {
        const uint32_t cs = le32(t + (size_t)i * entry), ds = le32(t + (size_t)i * entry + 4);
        if (!cs || ds > 0x40000000u) { free(t); return -1; }   /* empty frames are allowed (zstd ends with one) */
        d->coff[i] = c; d->doff[i] = u;
        c += cs; u += ds;
        if (ds > largest) largest = ds;
    }
    free(t);
    d->coff[frames] = c; d->doff[frames] = u;
    if (u != d->size || d->base + c > start) return -1;   /* the frames end before the seek table */
    d->frames = frames;
    d->cache_cap = (int)(CACHE_BYTES / largest);
    if (d->cache_cap < 1) d->cache_cap = 1;
    if (d->cache_cap > CACHE_MAX) d->cache_cap = CACHE_MAX;
    d->dctx = ZSTD_createDCtx();
    return d->dctx ? 0 : -1;
}

DumpFile *dump_open(const char *path) {
    DumpFile *d = calloc(1, sizeof *d);
    if (!d || !path || !(d->f = fopen(path, "rb"))) { free(d); return NULL; }
    pthread_mutex_init(&d->lock, NULL);
    uint8_t magic[4] = {0};
    fseeko(d->f, 0, SEEK_END);
    const uint64_t file_size = (uint64_t)ftello(d->f);
    if (file_size >= 4 && !raw_read(d->f, 0, magic, 4) && !memcmp(magic, "Z3DS", 4)) {
        d->z3ds = 1;
        if (z3ds_open(d, file_size)) {
            fprintf(stderr, "dump: %s is a compressed game file (Z3DS) that cannot be read: damaged?\n", path);
            dump_close(d);
            return NULL;
        }
        fprintf(stderr, "dump: compressed (Z3DS), %u frames, %llu bytes as %llu\n", d->frames, (unsigned long long)file_size,
                (unsigned long long)d->size);
    } else {
        d->size = file_size;
    }
    return d;
}

uint64_t dump_size(const DumpFile *d) { return d ? d->size : 0; }
int dump_compressed(const DumpFile *d) { return d && d->z3ds; }

/* frame i decompressed into dst (its whole size); 0 when it was */
static int frame_into(DumpFile *d, uint32_t i, uint8_t *dst) {
    const uint64_t cs = d->coff[i + 1] - d->coff[i], ds = d->doff[i + 1] - d->doff[i];
    if (cs > d->cbuf_cap) {
        uint8_t *b = realloc(d->cbuf, (size_t)cs);
        if (!b) return -1;
        d->cbuf = b; d->cbuf_cap = (size_t)cs;
    }
    if (raw_read(d->f, d->base + d->coff[i], d->cbuf, (size_t)cs)) return -1;
    const size_t n = ZSTD_decompressDCtx(d->dctx, dst, (size_t)ds, d->cbuf, (size_t)cs);
    return ZSTD_isError(n) || n != ds ? -1 : 0;
}

/* frame i, decompressed: from the cache, else into its least recently used slot */
static const uint8_t *frame_cached(DumpFile *d, uint32_t i) {
    Cached *slot = NULL;
    for (int k = 0; k < d->cached; k++)
        if (d->cache[k].frame == i) { d->cache[k].used = ++d->tick; return d->cache[k].data; }
    if (d->cached < d->cache_cap) {
        slot = &d->cache[d->cached++];
    } else {
        slot = &d->cache[0];
        for (int k = 1; k < d->cached; k++) if (d->cache[k].used < slot->used) slot = &d->cache[k];
    }
    slot->frame = UINT32_MAX;   /* empty until it holds frame i */
    slot->used = 0;
    const size_t ds = (size_t)(d->doff[i + 1] - d->doff[i]);
    uint8_t *data = realloc(slot->data, ds);
    if (!data) return NULL;
    slot->data = data;
    if (frame_into(d, i, data)) return NULL;
    slot->frame = i;
    slot->used = ++d->tick;
    return data;
}

int dump_read(DumpFile *d, uint64_t offset, void *dst, size_t size) {
    if (!d || offset > d->size || size > d->size - offset) return -1;
    if (!size) return 0;
    pthread_mutex_lock(&d->lock);
    int r = 0;
    if (!d->z3ds) {
        r = raw_read(d->f, offset, dst, size);
    } else {
        uint32_t lo = 0, hi = d->frames - 1;   /* the frame holding offset */
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo + 1) / 2;
            if (d->doff[mid] <= offset) lo = mid; else hi = mid - 1;
        }
        uint8_t *out = dst;
        for (uint32_t i = lo; size && !r && i < d->frames; i++) {
            const uint64_t at = offset - d->doff[i], len = d->doff[i + 1] - d->doff[i];
            if (len <= at) continue;   /* an empty frame */
            const size_t take = (size_t)(len - at < size ? len - at : size);
            if (at == 0 && take == len) {
                r = frame_into(d, i, out);
            } else {
                const uint8_t *frame = frame_cached(d, i);
                if (frame) memcpy(out, frame + at, take);
                else r = -1;
            }
            out += take; offset += take; size -= take;
        }
    }
    pthread_mutex_unlock(&d->lock);
    return r;
}

void dump_close(DumpFile *d) {
    if (!d) return;
    if (d->f) fclose(d->f);
    for (int k = 0; k < d->cached; k++) free(d->cache[k].data);
    free(d->cbuf); free(d->coff); free(d->doff);
    if (d->dctx) ZSTD_freeDCtx(d->dctx);
    pthread_mutex_destroy(&d->lock);
    if (g_dump == d) g_dump = NULL;
    free(d);
}
