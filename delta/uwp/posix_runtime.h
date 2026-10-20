#pragma once
#include <windows.h>
#include <direct.h>
#include <process.h>
#include <unistd.h>
#include <io.h>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <algorithm>
#include "uwp/storage_posix.h"
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 1
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_PHYS_PAGES 2
#define _SC_CLK_TCK 3
#define _SC_NPROCESSORS_ONLN 4
#endif
long prosperity_sysconf(int name);
void* prosperity_memmem(const void* haystack, size_t haystack_size, const void* needle, size_t needle_size);
#define sysconf prosperity_sysconf
#define mkdir(path, ...) _mkdir(path)
inline int prosperity_getpid() { return static_cast<int>(GetCurrentProcessId()); }
inline int gettid() { return static_cast<int>(GetCurrentThreadId()); }
#define getpid prosperity_getpid
#define memmem prosperity_memmem
inline char* strndup(const char* input, size_t limit) {
  const auto end = static_cast<const char*>(std::memchr(input, 0, limit));
  const size_t size = end ? static_cast<size_t>(end - input) : limit;
  auto* result = static_cast<char*>(std::malloc(size + 1));
  if (result) { std::memcpy(result, input, size); result[size] = 0; }
  return result;
}
