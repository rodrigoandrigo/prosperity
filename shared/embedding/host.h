#pragma once
namespace embedding {
using FailureHandler = void (*)(const char*);
void SetFailureHandler(FailureHandler handler);
void ReportFailure(const char* message) noexcept;
[[noreturn]] void Fail(const char* message);
}
