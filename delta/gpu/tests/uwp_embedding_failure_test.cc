// Failure containment for GPU checks and the embedding host boundary.
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "embedding/host.h"
#include "base/check.h"
#include "base/standard_streams.h"
#include "gpu/gpu_check.h"
#include "logger/logger.h"
bool guest_thread = false, stop_requested = false;
int failures = 0, log_writes = 0;
std::jmp_buf guest_exit;
namespace guest {
bool OnGuestThread() { return guest_thread; }
void RequestStop() { stop_requested = true; }
}
namespace cpu { void ExitGuestThread() { std::longjmp(guest_exit, 1); } }
namespace uwp { void WriteHostLog(uint32_t, const char*) noexcept { ++log_writes; } }
namespace logger { void AddLogMsg(LogLevel, u32, const char*, base::String) { ++log_writes; } }
int main() {
  embedding::SetFailureHandler([](const char*) { ++failures; });
  try { embedding::Fail("normal worker error"); return 1; } catch (const std::runtime_error&) {}
  try { base::detail::FatalCheckFailure(__FILE__, __LINE__, "fatal core check"); return 2; } catch (const std::runtime_error&) {}
  try { GPU_BUGCHECK(false); return 3; } catch (const std::runtime_error&) {}
  try { LOG_ASSERT(false); return 4; } catch (const std::runtime_error&) {}
  base::WriteStandardError("stderr bridge", 13);
  if (failures != 4 || !log_writes) return 5;
  guest_thread = true;
  if (_setjmp(guest_exit, nullptr) == 0) embedding::Fail("guest error");
  if (!stop_requested || failures != 5) return 6;
  std::puts("Embedding failures: worker/GPU/logger/check errors and stderr returned to host; guest exit gate preserved process");
}
