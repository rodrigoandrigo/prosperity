#include "uwp/memory_win.h"
#include "uwp/storage_bridge.h"
int32_t PROSPERITY_CALL prosperity_configure_memory(prosperity_context* context, const prosperity_memory_policy* policy) {
  if (!uwp::IsActiveContext(context) || !policy) return PROSPERITY_INVALID_ARGUMENT;
  const auto state = prosperity_get_state(context);
  if (state != PROSPERITY_INITIALIZING && state != PROSPERITY_READY && state != PROSPERITY_STOPPED && state != PROSPERITY_FAILED) return PROSPERITY_INVALID_STATE;
  try { return uwp::ConfigureMemory(*policy) ? PROSPERITY_OK : PROSPERITY_INVALID_ARGUMENT; }
  catch (...) { return PROSPERITY_INTERNAL_ERROR; }
}
int32_t PROSPERITY_CALL prosperity_get_memory_info(prosperity_context* context, prosperity_memory_info* info) {
  if (!uwp::IsActiveContext(context) || !info || info->size != sizeof(*info)) return PROSPERITY_INVALID_ARGUMENT;
  try { *info = uwp::MemoryInfo(); return PROSPERITY_OK; } catch (...) { return PROSPERITY_INTERNAL_ERROR; }
}
