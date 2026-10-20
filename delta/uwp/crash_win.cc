#include <windows.h>
#include <cstdio>
#include <mutex>
#include <vector>
#include <cstring>
#include <stdexcept>
#include "kern/crash.h"
#include "kern/process.h"
#include "kern/module.h"
#include "kern/ps4/dev/gc_dev.h"
#include "cpu/mmio.h"
#include "host_memory/host_memory.h"
#include "base/logging.h"
#include "uwp/memory_win.h"
#include "guest/pause.h"
#include "guest/session.h"
#include "embedding/host.h"

extern "C" __declspec(dllimport) PVOID WINAPI AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER);
extern "C" __declspec(dllimport) ULONG WINAPI RemoveVectoredExceptionHandler(PVOID);

namespace kern {
namespace {
thread_local uintptr_t stack_scan_base;
CsRangeDescriber describe_compute;
struct NullGuard { uintptr_t address; GuardReg reg; int length; };
std::mutex guard_mutex;
std::vector<NullGuard> guards;
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter;
std::mutex filter_mutex;
bool filter_installed = false;
PVOID vectored_handler = nullptr;
LONG DispatchException(EXCEPTION_POINTERS* exception, bool vectored) {
  const auto* record = exception->ExceptionRecord;
  auto* context = exception->ContextRecord;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
    const auto address = record->ExceptionInformation[1];
    if (record->ExceptionInformation[0] == 8 && guest::HandleStopFault(context))
      return EXCEPTION_CONTINUE_EXECUTION;
    const bool write = record->ExceptionInformation[0] == 1;
    if (write && GcDevice::IsDoorbellAddress(address) && cpu::HandleMmioWrite(context, address, GcDevice::WriteDoorbell))
      return EXCEPTION_CONTINUE_EXECUTION;
    if (write && host_memory::HandleWriteFault(address)) return EXCEPTION_CONTINUE_EXECUTION;
    if (uwp::HandleMemoryFault(address, write, record->ExceptionInformation[0] == 8))
      return EXCEPTION_CONTINUE_EXECUTION;
    if (const auto* memory_error = uwp::MemoryFaultError(); memory_error && guest::OnGuestThread()) {
      embedding::ReportFailure(memory_error);
      guest::RequestStop();
      if (guest::HandleStopFault(context)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (!write && address < 0x10000) {
      std::lock_guard lock(guard_mutex);
      for (const auto& guard : guards) if (guard.address == context->Rip) {
        if (guard.reg == GuardReg::rax) context->Rax = 0;
        else context->Rsi = 0;
        context->Rip += guard.length;
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
  }
  if (!guest::OnGuestThread())
    return !vectored && previous_filter ? previous_filter(exception) : EXCEPTION_CONTINUE_SEARCH;
  char symbol[256];
  Symbolize(context->Rip, symbol, sizeof(symbol));
  BASE_LOGE("crashHandler", "Windows exception {:#x}: {} rsp={:#x}", record->ExceptionCode, symbol, context->Rsp);
  if (record->NumberParameters >= 2)
    BASE_LOGE("crashHandler", "access={} address={:#x} winerror={} memory={}", record->ExceptionInformation[0], record->ExceptionInformation[1], GetLastError(), uwp::MemoryFaultError() ? uwp::MemoryFaultError() : "none");
  BASE_LOGE("crashHandler", "rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x} rdi={:#x} rsi={:#x} r8={:#x} r9={:#x}", context->Rax,context->Rbx,context->Rcx,context->Rdx,context->Rdi,context->Rsi,context->R8,context->R9);
  Backtrace(context->Rbp);
  embedding::ReportFailure(symbol);
  if (guest::OnGuestThread()) {
    guest::RequestStop();
    if (guest::HandleStopFault(context)) return EXCEPTION_CONTINUE_EXECUTION;
  }
  return !vectored && previous_filter ? previous_filter(exception) : EXCEPTION_CONTINUE_SEARCH;
 }
LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS* exception) { return DispatchException(exception, false); }
LONG WINAPI VectoredFilter(EXCEPTION_POINTERS* exception) {
  // Safe-copy probes have a local SEH boundary. Do not turn a failed probe
  // into a guest termination or recurse while collecting a guest backtrace.
  if (uwp::InSafeMemoryCopy()) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = exception->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
    return EXCEPTION_CONTINUE_SEARCH;
  return DispatchException(exception, true);
}
}
void InstallCrashHandler() {
  std::lock_guard lock(filter_mutex);
  if (!filter_installed) {
    // Resolve guest demand faults before Windows attempts to unwind raw PS4
    // or generated frames, which have no Windows .pdata unwind metadata.
    vectored_handler = AddVectoredExceptionHandler(1, VectoredFilter);
    if (!vectored_handler) throw std::runtime_error("Unable to register guest vectored exception handler");
    previous_filter = SetUnhandledExceptionFilter(ExceptionFilter);
    filter_installed = true;
  }
}
void UninstallCrashHandler() {
  std::lock_guard lock(filter_mutex);
  if (!filter_installed) return;
  if (vectored_handler) { RemoveVectoredExceptionHandler(vectored_handler); vectored_handler = nullptr; }
  auto current = SetUnhandledExceptionFilter(previous_filter);
  if (current != ExceptionFilter) SetUnhandledExceptionFilter(current);
  filter_installed = false;
}
void SetCsRangeDescriber(CsRangeDescriber fn) { describe_compute = fn; }
void Symbolize(uintptr_t address, char* out, size_t size) {
  if (auto* process = Process::GetActive()) for (auto& module : process->GetModuleList()) {
    auto& info = module->GetInfo();
    for (const auto* segment : {&info.text_seg, &info.data_seg}) {
      const auto begin = reinterpret_cast<uintptr_t>(segment->addr);
      if (begin && address >= begin && address - begin < segment->size) {
        std::snprintf(out, size, "%s+%#llx", info.name.c_str(), static_cast<unsigned long long>(address - begin));
        return;
      }
    }
  }
  std::snprintf(out, size, "%#llx (host/unmapped)", static_cast<unsigned long long>(address));
}
void Backtrace(uintptr_t frame) {
  for (int i = 0; i < 32 && frame >= 0x10000 && !(frame & 7); ++i) {
    uintptr_t values[2];
    if (!uwp::ReadMemory(values, reinterpret_cast<void*>(frame), sizeof(values))) break;
    char symbol[256]; Symbolize(values[1], symbol, sizeof(symbol));
    BASE_LOGI("crashHandler", "  #{} {}", i, symbol);
    if (values[0] <= frame) break;
    frame = values[0];
  }
}
void SetGuestStackScanBase(uintptr_t sp) { stack_scan_base = sp; }
uintptr_t GuestStackScanBase() { return stack_scan_base; }
void GuestStackTraceFrom(uintptr_t sp, const char* tag, int max_frames, long tid) {
  for (int i = 0; i < max_frames && i < 4096; ++i) {
    uintptr_t address;
    if (!uwp::ReadMemory(&address, reinterpret_cast<void*>(sp + i * sizeof(address)), sizeof(address))) break;
    char symbol[256]; Symbolize(address, symbol, sizeof(symbol));
    if (!std::strstr(symbol, "(host/unmapped)")) BASE_LOGI("stack", "{} tid={} {}", tag, tid, symbol);
  }
}
void GuestStackTrace(const char* tag, int frames) { GuestStackTraceFrom(stack_scan_base, tag, frames, GetCurrentThreadId()); }
void SetNullGuard(uintptr_t address, GuardReg reg, int length) {
  if (length <= 0 || length > 15) throw std::invalid_argument("Invalid null-guard instruction length");
  std::lock_guard lock(guard_mutex);
  guards.push_back({address, reg, length});
}
// Windows dispatches SEH; POSIX sigaltstack is not used on this platform.
void InstallSigAltStack() {}
}
