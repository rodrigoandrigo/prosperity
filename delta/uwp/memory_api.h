#pragma once
#include "prosperity_api.h"
#ifdef __cplusplus
extern "C" {
#endif
// Synchronous worker callback: zero means success. Do not throw across the ABI.
typedef int32_t (PROSPERITY_CALL *prosperity_memory_query)(void*, uint64_t* usage, uint64_t* limit);
typedef struct prosperity_memory_policy {
  uint32_t size;
  prosperity_memory_query query;
  void* user;
  uint64_t cap_bytes, reserve_bytes;
} prosperity_memory_policy;
typedef struct prosperity_memory_info {
  uint32_t size;
  uint64_t committed_bytes, ram_chunks, chunk_bytes, commit_bytes, cap_bytes, reserve_bytes, rejected_commits;
} prosperity_memory_info;
// Call before boot; callback storage remains alive until destroy returns.
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_configure_memory(prosperity_context*, const prosperity_memory_policy*);
PROSPERITY_API int32_t PROSPERITY_CALL prosperity_get_memory_info(prosperity_context*, prosperity_memory_info*);
#ifdef __cplusplus
}
#endif
