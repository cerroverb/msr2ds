/* The game file (dump.c): a .3ds, .cci or .cxi read as it is, or Azahar's compressed one (.zcci, .zcxi: Z3DS) read
   through its zstd frames, the same bytes. One handle serves every reader at once (a lock around each read). */
#ifndef PWR_DUMP_H
#define PWR_DUMP_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DumpFile DumpFile;
DumpFile *dump_open(const char *path);   /* NULL when it cannot be opened, or its compressed form cannot be read */
int dump_read(DumpFile *d, uint64_t offset, void *dst, size_t size);   /* 0 when all of it was read */
uint64_t dump_size(const DumpFile *d);   /* the game file's size (uncompressed) */
int dump_compressed(const DumpFile *d);  /* a Z3DS file */
void dump_close(DumpFile *d);
extern DumpFile *g_dump;   /* the game's (main.c): its RomFS for fs.c */
#ifdef __cplusplus
}
#endif
#endif
