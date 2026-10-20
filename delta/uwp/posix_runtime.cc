#include "uwp/posix_runtime.h"
#include "uwp/memory_win.h"
#include <sys/syscall.h>
#include <sys/uio.h>
#include <algorithm>
#include <cstdarg>
#include <cerrno>
#include <climits>

long prosperity_sysconf(int name) {
  SYSTEM_INFO system{};
  GetNativeSystemInfo(&system);
  switch (name) {
    case _SC_PAGESIZE: return system.dwPageSize;
    case _SC_NPROCESSORS_ONLN: return system.dwNumberOfProcessors;
    case _SC_CLK_TCK: return 1000;
    case _SC_PHYS_PAGES: {
      MEMORYSTATUSEX memory{sizeof(memory)};
      if (!GlobalMemoryStatusEx(&memory)) return -1;
      return static_cast<long>(std::min<uint64_t>(memory.ullTotalPhys / system.dwPageSize, LONG_MAX));
    }
  }
  errno = EINVAL;
  return -1;
}
void* prosperity_memmem(const void* haystack, size_t size, const void* needle, size_t count) {
  if (!count) return const_cast<void*>(haystack);
  if (count > size) return nullptr;
  const auto* bytes = static_cast<const unsigned char*>(haystack);
  for (size_t at = 0; at <= size - count; ++at)
    if (!memcmp(bytes + at, needle, count)) return const_cast<unsigned char*>(bytes + at);
  return nullptr;
}
extern "C" intptr_t prosperity_syscall(int operation, ...) {
  if (operation == SYS_gettid) return GetCurrentThreadId();
  if (operation != SYS_process_vm_readv && operation != SYS_process_vm_writev) {
    errno = ENOSYS;
    return -1;
  }
  va_list arguments;
  va_start(arguments, operation);
  const int process = va_arg(arguments, int);
  const auto* local = va_arg(arguments, const iovec*);
  const auto locals = va_arg(arguments, uintptr_t);
  const auto* remote = va_arg(arguments, const iovec*);
  const auto remotes = va_arg(arguments, uintptr_t);
  const auto flags = va_arg(arguments, uintptr_t);
  va_end(arguments);
  if (process != prosperity_getpid() || flags || !local || !remote || locals > 1024 || remotes > 1024) {
    errno = EINVAL;
    return -1;
  }
  size_t li = 0, ri = 0, lo = 0, ro = 0, copied = 0;
  while (li < locals && ri < remotes) {
    const auto bytes = std::min(local[li].iov_len - lo, remote[ri].iov_len - ro);
    auto* l = static_cast<unsigned char*>(local[li].iov_base) + lo;
    auto* r = static_cast<unsigned char*>(remote[ri].iov_base) + ro;
    const bool ok = operation == SYS_process_vm_readv ? uwp::ReadMemory(l, r, bytes) : uwp::WriteMemory(r, l, bytes);
    if (!ok) { errno = EFAULT; return copied ? static_cast<intptr_t>(copied) : -1; }
    copied += bytes;
    lo += bytes;
    ro += bytes;
    if (lo == local[li].iov_len) { ++li; lo = 0; }
    if (ro == remote[ri].iov_len) { ++ri; ro = 0; }
  }
  return static_cast<intptr_t>(copied);
}
