#pragma once
#include <stdint.h>
#ifndef PROSPERITY_CALL
#ifdef _WIN32
#define PROSPERITY_CALL __cdecl
#else
#define PROSPERITY_CALL
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Versioned C broker ABI. Strings are UTF-8, offsets/counts are 64-bit.
 * No rate limiter, whole-file staging or file-count limit is imposed. */
#define PROSPERITY_STORAGE_VERSION 1u
enum prosperity_io_status {
  PROSPERITY_IO_OK = 0, PROSPERITY_IO_END = 1,
  PROSPERITY_IO_NOT_FOUND = -1, PROSPERITY_IO_DENIED = -2,
  PROSPERITY_IO_UNSUPPORTED = -3, PROSPERITY_IO_FAILED = -4,
  PROSPERITY_IO_INVALID = -5, PROSPERITY_IO_CANCELLED = -6,
  PROSPERITY_IO_NOT_DIRECTORY = -7
};
enum prosperity_io_flags {
  PROSPERITY_IO_READ = 1, PROSPERITY_IO_WRITE = 2,
  PROSPERITY_IO_CREATE = 4, PROSPERITY_IO_TRUNCATE = 8,
  PROSPERITY_IO_APPEND = 16, PROSPERITY_IO_DIRECTORY = 32
};
typedef struct prosperity_file_info {
  uint32_t size, flags;
  uint64_t file_size;
  int64_t modified_unix_seconds;
} prosperity_file_info;
typedef struct prosperity_io_stream {
  uint32_t size;
  void* user;
  void (PROSPERITY_CALL *retain)(void*);
  void (PROSPERITY_CALL *release)(void*);
  int32_t (PROSPERITY_CALL *read_at)(void*, uint64_t, void*, uint64_t, uint64_t*);
  int32_t (PROSPERITY_CALL *write_at)(void*, uint64_t, const void*, uint64_t, uint64_t*);
  int32_t (PROSPERITY_CALL *get_size)(void*, uint64_t*);
  int32_t (PROSPERITY_CALL *flush)(void*);
} prosperity_io_stream;
typedef struct prosperity_opened_file {
  uint32_t size;
  /* Successful open transfers this HANDLE or one retained stream reference
   * to the DLL. Native handles are preferred; the DLL closes the handle. */
  void* native_handle;
  prosperity_io_stream stream;
} prosperity_opened_file;
typedef int32_t (PROSPERITY_CALL *prosperity_directory_emit)(void*, const char*, const prosperity_file_info*);
typedef struct prosperity_storage {
  uint32_t size, version;
  void* user;
  void (PROSPERITY_CALL *retain)(void*);
  void (PROSPERITY_CALL *release)(void*);
  int32_t (PROSPERITY_CALL *open)(void*, const char* relative_path, uint32_t flags, prosperity_opened_file*);
  int32_t (PROSPERITY_CALL *stat)(void*, const char* relative_path, prosperity_file_info*);
  int32_t (PROSPERITY_CALL *list_begin)(void*, const char* relative_path, void** cursor);
  /* Exactly one entry per OK; END finishes. Cursor ownership is per iterator;
   * no directory snapshot or recursive pre-scan is required. */
  int32_t (PROSPERITY_CALL *list_next)(void* cursor, prosperity_directory_emit, void*);
  void (PROSPERITY_CALL *list_close)(void* cursor);
} prosperity_storage;
#ifdef __cplusplus
}
#endif
