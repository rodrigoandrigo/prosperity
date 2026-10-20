#pragma once
#include "uwp/prosperity_api.h"
namespace uwp {
void ConfigureLogging(const char* directory, prosperity_log_callback callback, void* user);
void CloseLogging();
bool InLogCallback();
void WriteHostLog(uint32_t level, const char* message) noexcept;
}
