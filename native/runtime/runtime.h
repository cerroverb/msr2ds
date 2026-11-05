#ifndef MSR_NATIVE_RUNTIME_H
#define MSR_NATIVE_RUNTIME_H
#include "cpu.h"
#include <stddef.h>
int rt_mem_init(void);                                  /* reserve the 4 GiB guest address space */
int rt_replace_file(const char *tmp, const char *dst);  /* rename() that also replaces dst on Windows */
void rt_mem_commit(uint32_t addr, size_t len);          /* Windows: commit guest pages before file I/O into them */
#endif
