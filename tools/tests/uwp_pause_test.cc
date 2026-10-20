#include <windows.h>
#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <thread>
#include "guest/pause.h"
#include <stdexcept>
// This harness replaces the embedding host's failure receiver; the production
// implementation is exercised separately through the DLL ABI test.
namespace embedding {
void ReportFailure(const char* message) noexcept { std::fprintf(stderr, "%s\n", message); }
[[noreturn]] void Fail(const char* message) { throw std::runtime_error(message); }
}
namespace base::detail {
[[noreturn]] void FatalCheckFailure(const char*, int, const char* message) { embedding::Fail(message); }
}
// This standalone desktop harness supplies executable test code. Production
// code must use its packaged codeGeneration allocation path instead.
extern "C" __declspec(dllimport) BOOL WINAPI VirtualProtect(LPVOID, SIZE_T, DWORD, PDWORD);
std::atomic<bool> stopping{false}, finished{false};
namespace guest { bool Stopping() { return stopping.load(); } }
thread_local std::jmp_buf exit_target;
void Exit() { std::longjmp(exit_target, 1); }
LONG WINAPI Fault(EXCEPTION_POINTERS* exception) {
  if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      exception->ExceptionRecord->ExceptionInformation[0] == 8 &&
      guest::HandleStopFault(exception->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
  return EXCEPTION_CONTINUE_SEARCH;
}
int main() {
  SetUnhandledExceptionFilter(Fault);
  alignas(8) volatile LONG64 counter = 0;
  volatile LONG escape = 0;
  unsigned char program[] = {
    0x48,0xb8,0,0,0,0,0,0,0,0, // mov rax, counter
    0x48,0xba,0,0,0,0,0,0,0,0, // mov rdx, escape
    0xf0,0x48,0xff,0x00,        // lock inc qword [rax]
    0x83,0x3a,0x00,             // cmp dword [rdx], 0
    0x74,0xf7,0xc3              // je loop; ret
  };
  const auto counter_pointer = reinterpret_cast<uintptr_t>(&counter);
  const auto escape_pointer = reinterpret_cast<uintptr_t>(&escape);
  std::memcpy(program + 2, &counter_pointer, 8);
  std::memcpy(program + 12, &escape_pointer, 8);
  auto* code = VirtualAllocFromApp(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!code) return 1;
  std::memcpy(code, program, sizeof(program));
  DWORD previous;
  if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &previous)) return 2;
  guest::RegisterCode(reinterpret_cast<uintptr_t>(code), sizeof(program));
  std::thread worker([&] {
    guest::ThreadRegistration registration;
    guest::SetThreadExitHandler(Exit);
    if (_setjmp(exit_target, nullptr) == 0) reinterpret_cast<void(*)()>(code)();
    finished = true;
  });
  const auto started = GetTickCount64();
  while (InterlockedCompareExchange64(&counter, 0, 0) < 1000 && GetTickCount64() - started < 2000) Sleep(1);
  guest::SetPaused(true);
  for (int i = 0; i < 20; ++i) { guest::PollPause(); Sleep(1); }
  const auto paused = InterlockedCompareExchange64(&counter, 0, 0);
  Sleep(20);
  const bool paused_ok = paused > 0 && InterlockedCompareExchange64(&counter, 0, 0) == paused;
  guest::SetPaused(false);
  Sleep(20);
  const bool resumed_ok = InterlockedCompareExchange64(&counter, 0, 0) > paused;
  stopping = true;
  guest::SetPaused(true);
  while (!finished && GetTickCount64() - started < 4000) { guest::PollPause(); Sleep(1); }
  const bool stopped_ok = finished;
  if (!stopped_ok) {
    InterlockedExchange(&escape, 1);
    VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &previous);
    guest::SetPaused(false);
  }
  worker.join();
  guest::SetPaused(false);
  guest::ResetCodeRanges();
  VirtualFree(code, 0, MEM_RELEASE);
  std::printf("Windows guest loop: pause=%d resume=%d stop=%d\n", paused_ok, resumed_ok, stopped_ok);
  return paused_ok && resumed_ok && stopped_ok ? 0 : 3;
}
