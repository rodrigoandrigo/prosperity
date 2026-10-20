#include <windows.h>
#include <roapi.h>
#include "MemoryBudget.h"
namespace Prosperity_UWP {
int32_t PROSPERITY_CALL QueryMemoryBudget(void*, uint64_t* usage, uint64_t* limit) {
  if (!usage || !limit) return PROSPERITY_INVALID_ARGUMENT;
  const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
  int32_t result = PROSPERITY_INTERNAL_ERROR;
  try {
    *usage = Windows::System::MemoryManager::AppMemoryUsage;
    *limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
    result = *limit ? PROSPERITY_OK : PROSPERITY_INTERNAL_ERROR;
  } catch (...) {}
  if (SUCCEEDED(apartment)) RoUninitialize();
  return result;
}
}
