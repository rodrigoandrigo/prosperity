#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <sys/types.h>
#include <errno.h>
inline ssize_t getrandom(void* output, size_t size, unsigned flags) {
  if (flags || size > ULONG_MAX) { errno = EINVAL; return -1; }
  if (BCryptGenRandom(nullptr, static_cast<PUCHAR>(output), static_cast<ULONG>(size),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) { errno = EIO; return -1; }
  return static_cast<ssize_t>(size);
}
