#include "base/check.h"
#include "base/standard_streams.h"
#include "embedding/host.h"
#include "uwp/logging.h"
#include <string>
namespace base {
namespace { CheckHandler* check_handler = nullptr; }
void SetCheckHandler(CheckHandler* handler) { check_handler = handler; }
namespace detail {
void DCheck(const SourceLocation& location, const char* message) {
  if (check_handler) check_handler(location.format, location.file, location.func, message);
  embedding::Fail(message ? message : "Core check failed");
}
void BugCheck(const SourceLocation& location, const char* message) { DCheck(location, message); }
[[noreturn]] void FatalCheckFailure(const char*, int, const char* message) { embedding::Fail(message); }
}
void WriteStandardError(const char* text, mem_size size) noexcept {
  try { std::string copy(text, size); uwp::WriteHostLog(4, copy.c_str()); } catch (...) {}
}
void WriteStandardOutput(const char* text, mem_size size) noexcept { WriteStandardError(text, size); }
}
