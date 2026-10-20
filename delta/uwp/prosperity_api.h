#pragma once
#include <stdint.h>
#include "../../shared/io/storage_api.h"

#ifdef _WIN32
#define PROSPERITY_CALL __cdecl
#ifdef PROSPERITY_EXPORTS
#define PROSPERITY_API __declspec(dllexport)
#else
#define PROSPERITY_API
#endif
#else
#define PROSPERITY_CALL
#define PROSPERITY_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PROSPERITY_ABI_VERSION 2u
typedef struct prosperity_context prosperity_context;
typedef struct prosperity_file prosperity_file;
typedef enum prosperity_result {
  PROSPERITY_OK = 0,
  PROSPERITY_INVALID_ARGUMENT = -1,
  PROSPERITY_INVALID_STATE = -2,
  PROSPERITY_IO_ERROR = -3,
  PROSPERITY_INTERNAL_ERROR = -4,
  PROSPERITY_TIMEOUT = -5
} prosperity_result;
typedef enum prosperity_state {
  PROSPERITY_READY = 0, PROSPERITY_STARTING = 1, PROSPERITY_RUNNING = 2,
  PROSPERITY_PAUSED = 3, PROSPERITY_STOPPING = 4, PROSPERITY_STOPPED = 5,
  PROSPERITY_FAILED = 6, PROSPERITY_INITIALIZING = 7
} prosperity_state;
typedef void (PROSPERITY_CALL *prosperity_log_callback)(void* user_data,
    uint32_t level, const char* utf8_message);
typedef void (PROSPERITY_CALL *prosperity_state_callback)(void* user_data,
    prosperity_state state, prosperity_result result, const char* utf8_message);

/* All strings are UTF-8. No C++ objects or CRT-owned allocations cross the ABI.
 * Callbacks may run on emulator workers; pixel memory is valid during the call.
 * The host must keep user_data and callbacks alive until destroy returns. */
typedef void (PROSPERITY_CALL *prosperity_frame_callback)(
    void* user_data, const void* pixels, uint32_t width, uint32_t height,
    uint32_t pitch, uint32_t bgra);
typedef struct prosperity_config {
  uint32_t size;
  uint32_t abi_version;
  const char* data_directory;
  void* user_data;
  prosperity_frame_callback on_frame;
  prosperity_log_callback on_log;
  prosperity_state_callback on_state;
} prosperity_config;

PROSPERITY_API uint32_t PROSPERITY_CALL prosperity_get_abi_version(void);
PROSPERITY_API prosperity_result PROSPERITY_CALL prosperity_create(
    const prosperity_config* config, prosperity_context** context);
PROSPERITY_API prosperity_result PROSPERITY_CALL prosperity_boot(
    prosperity_context* context, const char* path);
PROSPERITY_API prosperity_result PROSPERITY_CALL prosperity_pause(
    prosperity_context* context, uint32_t paused);
PROSPERITY_API prosperity_result PROSPERITY_CALL prosperity_stop(prosperity_context* context);
/* boot/stop enqueue work and return immediately. Use state/callbacks to observe
 * completion. wait is explicitly blocking (UINT32_MAX = infinite); do not call
 * it or destroy from a callback or the UI thread. Callbacks may query state,
 * copy errors, or request_stop; callback text is valid only during the call. */
PROSPERITY_API prosperity_result PROSPERITY_CALL prosperity_request_stop(prosperity_context* context);
PROSPERITY_API prosperity_result PROSPERITY_CALL prosperity_wait(prosperity_context* context, uint32_t timeout_ms);
PROSPERITY_API prosperity_state PROSPERITY_CALL prosperity_get_state(prosperity_context* context);
/* Returns INVALID_STATE in callbacks. On cleanup failure the context and all
 * callback storage must remain alive; only OK releases ownership. */
PROSPERITY_API prosperity_result PROSPERITY_CALL prosperity_destroy(prosperity_context* context);
/* Copies the diagnostic into caller-owned storage, always NUL terminating. */
PROSPERITY_API void PROSPERITY_CALL prosperity_get_last_error(
    prosperity_context* context, char* buffer, uint32_t capacity);

/* Broker I/O is synchronous: call file/stat/list APIs from workers, never the
 * XAML thread. The built-in core/frontend performs its reads on workers.
 * Mount copies the table and retains user. Replace/unmount affects future
 * opens; outstanding files/iterators retain their provider until closed.
 * A StorageFile is an exact-path mount; a StorageFolder serves descendants.
 * Native import duplicates the caller's handle; stream import retains user.
 * Imported files are read-only. Provider mounts may additionally allow writes.
 * Callback tables must remain callable until their last release, even after
 * unmount/destroy if the caller still owns an open file. Callbacks must not
 * throw across the ABI, and must support concurrent independent operations.
 * Do not destroy a context concurrently with a mount/open/stat/list call. */
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_mount_storage(prosperity_context*, const char* root, const prosperity_storage*);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_import_file_handle(prosperity_context*, const char* path, void* handle);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_import_file_stream(prosperity_context*, const char* path, const prosperity_io_stream*);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_unmount_storage(prosperity_context*, const char* root);
/* The same layer used by loaders, archives and guest VFS. No shared file cursor:
 * read_at/write_at are safe to use concurrently on a retained open file.
 * Close must follow completion of the caller's outstanding operations. */
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_file_open(prosperity_context*, const char*, uint32_t flags, prosperity_file**);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_file_read_at(prosperity_file*, uint64_t, void*, uint64_t, uint64_t*);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_file_write_at(prosperity_file*, uint64_t, const void*, uint64_t, uint64_t*);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_file_get_size(prosperity_file*, uint64_t*);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_file_flush(prosperity_file*);
PROSPERITY_API void PROSPERITY_CALL prosperity_file_close(prosperity_file*);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_storage_stat(prosperity_context*, const char*, prosperity_file_info*);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_storage_list(prosperity_context*, const char*, prosperity_directory_emit, void*);

#ifdef __cplusplus
}
#endif
