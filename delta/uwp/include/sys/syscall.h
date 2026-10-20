#pragma once
#include <stdint.h>
// Host compatibility operations, not Linux syscall numbers.
#define SYS_gettid 1
#define SYS_process_vm_readv 2
#define SYS_process_vm_writev 3
#ifdef __cplusplus
extern "C" {
#endif
intptr_t prosperity_syscall(int operation, ...);
#ifdef __cplusplus
}
#endif
#define syscall prosperity_syscall
