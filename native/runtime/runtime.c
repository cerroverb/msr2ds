/* Guest address space (from Woohoo! Flight School). */
#include "cpu.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t *g_mem;

#ifdef _WIN32
/* Windows: the 4 GiB are only reserved; a page is committed (zero-filled) the first time anything touches it, as Linux's
   MAP_NORESERVE does, so the commit charge stays near what the game uses */
static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *a = (uint8_t *)r->ExceptionInformation[1];
    if (a < g_mem || a >= g_mem + ((size_t)1 << 32)) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *chunk = g_mem + (((size_t)(a - g_mem)) & ~(size_t)0xffff);
    return VirtualAlloc(chunk, 0x10000, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
#endif

int rt_mem_init(void) {
#ifdef _WIN32
    g_mem = VirtualAlloc(NULL, (SIZE_T)1 << 32, MEM_RESERVE, PAGE_READWRITE);
    if (!g_mem) return -1;
    AddVectoredExceptionHandler(1, commit_on_touch);
    return 0;
#else
    void *p = mmap(NULL, (size_t)1 << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -1;
    g_mem = p;
    return 0;
#endif
}

/* Windows: commit guest pages before the OS itself reads or writes them (file I/O does not fault into
   commit_on_touch, it just fails) */
void rt_mem_commit(uint32_t addr, size_t len) {
#ifdef _WIN32
    if (!len) return;
    uint64_t lo = addr & ~0xffffull, hi = ((uint64_t)addr + len + 0xffff) & ~0xffffull;
    if (hi > (1ull << 32)) hi = 1ull << 32;
    VirtualAlloc(g_mem + lo, (SIZE_T)(hi - lo), MEM_COMMIT, PAGE_READWRITE);
#else
    (void)addr; (void)len;
#endif
}

/* write tmp over dst (rename() does not replace an existing file on Windows) */
int rt_replace_file(const char *tmp, const char *dst) {
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(tmp, dst);
#endif
}

