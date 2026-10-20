#pragma once
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <io.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void*)-1)
#define MFD_CLOEXEC 1
#define MADV_DONTNEED 4
#define MADV_FREE 8
#define MADV_DONTDUMP 16
#ifdef __cplusplus
extern "C" {
#endif
void* prosperity_mmap(void*, size_t, int, int, int, int64_t);
int prosperity_munmap(void*, size_t);
int prosperity_mprotect(void*, size_t, int);
int prosperity_mincore(void*, size_t, unsigned char*);
int prosperity_madvise(void*, size_t, int);
int prosperity_memfd_create(const char*, unsigned);
int prosperity_ftruncate(int, int64_t);
int prosperity_close(int);
#ifdef __cplusplus
}
#endif
#define mmap prosperity_mmap
#define munmap prosperity_munmap
#define mprotect prosperity_mprotect
#define mincore prosperity_mincore
#define madvise prosperity_madvise
#define memfd_create prosperity_memfd_create
#define ftruncate prosperity_ftruncate
#define close prosperity_close
