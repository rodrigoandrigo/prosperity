#include "guest/pause.h"
#include "guest/session.h"

#include <cerrno>
#if defined(_WIN32)
#include <windows.h>
#include <stdexcept>
#include <exception>
#if defined(DELTA_UWP)
#include "embedding/host.h"
#endif
#endif

#include "base/atomic.h"
#include "base/containers/array.h"
#include "base/containers/vector.h"
#include "base/threading/lock_guard.h"
#include "base/threading/mutex.h"
#include "base/threading/thread.h"

#if defined(__linux__)
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace guest {
namespace {
base::Atomic<i32> g_paused{0};
thread_local bool t_guest = false;
thread_local void (*t_exit)() = nullptr;
thread_local bool (*t_is_guest_pc)(uintptr_t) = nullptr;
thread_local base::Atomic<bool> t_parked{false};
base::Mutex g_mutex;
struct Thread {
  int tid;
  base::Atomic<bool>* parked;
#if defined(_WIN32)
  HANDLE handle = nullptr;
  bool suspended = false;
#endif
};
base::Vector<Thread> g_threads;
struct CodeRange {
  uintptr_t begin;
  uintptr_t end;
};
base::Array<CodeRange, 4096> g_code;
base::Atomic<u32> g_code_count{0};
bool g_signal_installed = false;
#if defined(_WIN32)
[[noreturn]] void WindowsExitGate() {
  if (t_exit) t_exit();
#if defined(DELTA_UWP)
  embedding::Fail("Missing guest thread exit handler");
#else
  std::terminate();
#endif
}
bool IsGuestPc(uintptr_t pc) {
  const u32 count = g_code_count.load(base::memory_order_acquire);
  for (u32 i = 0; i < count; ++i)
    if (pc >= g_code[i].begin && pc < g_code[i].end) return true;
  return false;
}
#endif

#if defined(__linux__)
void PauseSignal(int, siginfo_t*, void* context) {
  if (!t_guest || !Paused())
    return;
  auto* uc = static_cast<ucontext_t*>(context);
#if defined(__x86_64__)
  const auto pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
  const auto pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
#else
  return;
#endif
  if (t_is_guest_pc && t_is_guest_pc(pc)) {
    PausePoint();
    return;
  }
  // Host calls finish and park at their return gate, outside emulator locks.
  const u32 count = g_code_count.load(base::memory_order_acquire);
  for (u32 i = 0; i < count; ++i) {
    if (pc >= g_code[i].begin && pc < g_code[i].end) {
      PausePoint();
      return;
    }
  }
}
#endif
}  // namespace

bool Paused() {
  return g_paused.load(base::memory_order_acquire) != 0;
}

const i32* PauseFlagAddress() {
  static_assert(sizeof(g_paused) == sizeof(i32));
  return reinterpret_cast<const i32*>(&g_paused);
}

void PausePoint() {
  if (!t_guest || !Paused())
    return;
  const int saved_errno = errno;
  t_parked.store(true, base::memory_order_release);
  while (Paused() && !Stopping()) {
#if defined(__linux__)
    ::syscall(SYS_futex, &g_paused, FUTEX_WAIT_PRIVATE, 1, nullptr, nullptr, 0);
#else
    base::SleepForMilliseconds(1);
#endif
  }
  t_parked.store(false, base::memory_order_release);
  errno = saved_errno;
  if (Stopping() && t_exit)
    t_exit();
}

void PollPause() {
#if defined(_WIN32)
  if (!Paused()) return;
  base::LockGuard lock(g_mutex);
  for (auto& thread : g_threads) {
    if (thread.tid == static_cast<int>(GetCurrentThreadId()) || thread.parked->load(base::memory_order_acquire)) continue;
    if (!thread.suspended && SuspendThread(thread.handle) == static_cast<DWORD>(-1)) continue;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread.handle, &context) || !IsGuestPc(context.Rip)) {
      ResumeThread(thread.handle);
      thread.suspended = false;
      continue;
    }
    thread.suspended = true;
    if (Stopping()) {
      // SetThreadContext is a desktop-only API. Revoke the current guest code
      // page instead: the UWP exception filter redirects its execution fault
      // to the normal thread exit gate using the supplied continuation context.
      DWORD previous;
      if (VirtualProtectFromApp(reinterpret_cast<void*>(context.Rip & ~uintptr_t(4095)),
          4096, PAGE_NOACCESS, &previous)) {
        ResumeThread(thread.handle);
        thread.suspended = false;
      }
    }
  }
#endif
#if defined(__linux__)
  if (!Paused())
    return;
  base::LockGuard<base::Mutex> lock(g_mutex);
  if (!g_signal_installed)
    return;
  for (const auto& thread : g_threads)
    if (!thread.parked->load(base::memory_order_acquire))
      ::syscall(SYS_tgkill, ::getpid(), thread.tid, SIGUSR2);
#endif
}

void SetPaused(bool paused) {
  g_paused.store(paused ? 1 : 0, base::memory_order_release);
#if defined(_WIN32)
  if (!paused) {
    base::LockGuard lock(g_mutex);
    for (auto& thread : g_threads) if (thread.suspended) {
      ResumeThread(thread.handle);
      thread.suspended = false;
    }
  }
#endif
#if defined(__linux__)
  ::syscall(SYS_futex, &g_paused, FUTEX_WAKE_PRIVATE, 0x7fffffff, nullptr,
            nullptr, 0);
#endif
  if (paused)
    PollPause();
}

bool OnGuestThread() {
  return t_guest;
}
#if defined(_WIN32)
bool HandleStopFault(void* raw_context) {
  auto* context = static_cast<CONTEXT*>(raw_context);
  if (!t_guest || !Stopping() || !IsGuestPc(context->Rip)) return false;
  context->Rip = reinterpret_cast<uintptr_t>(&WindowsExitGate);
  context->Rsp = (context->Rsp & ~uintptr_t(15)) - 40;
  return true;
}
#endif

void RegisterCode(uintptr_t address, mem_size size) {
  if (!address || !size)
    return;
  base::LockGuard<base::Mutex> lock(g_mutex);
  const u32 count = g_code_count.load(base::memory_order_relaxed);
  if (count < g_code.size()) {
    g_code[count] = {address, address + size};
    g_code_count.store(count + 1, base::memory_order_release);
  }
}

void SetThreadExitHandler(void (*exit)()) {
  t_exit = exit;
}

void ResetCodeRanges() {
  base::LockGuard lock(g_mutex);
#if defined(_WIN32)
  if (!g_threads.empty()) throw std::runtime_error("Cannot reset code ranges while guest threads remain registered");
#endif
  g_threads = {};
  g_code_count.store(0);
}

ThreadRegistration::ThreadRegistration(bool (*is_guest_pc)(uintptr_t)) {
  if (t_guest)
    return;
  owner_ = true;
  t_is_guest_pc = is_guest_pc;
  t_guest = true;
#if defined(_WIN32)
  HANDLE handle = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
      THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0)) {
    owner_ = false;
    t_guest = false;
    throw std::runtime_error("Could not register Windows guest thread");
  }
  {
    base::LockGuard lock(g_mutex);
    g_threads.push_back({static_cast<int>(GetCurrentThreadId()), &t_parked, handle, false});
  }
#endif
#if defined(__linux__)
  {
    base::LockGuard<base::Mutex> lock(g_mutex);
    if (!g_signal_installed) {
      struct sigaction action {};
      action.sa_sigaction = PauseSignal;
      action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
      sigemptyset(&action.sa_mask);
      g_signal_installed = ::sigaction(SIGUSR2, &action, nullptr) == 0;
    }
    g_threads.push_back({static_cast<int>(::syscall(SYS_gettid)), &t_parked});
  }
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGUSR2);
  ::pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
#endif
}

ThreadRegistration::~ThreadRegistration() {
  if (!owner_)
    return;
#if defined(_WIN32)
  {
    base::LockGuard lock(g_mutex);
    for (mem_size i = 0; i < g_threads.size(); ++i) if (g_threads[i].parked == &t_parked) {
      if (g_threads[i].suspended) ResumeThread(g_threads[i].handle);
      CloseHandle(g_threads[i].handle);
      g_threads.erase(g_threads.begin() + i);
      break;
    }
  }
#endif
#if defined(__linux__)
  base::LockGuard<base::Mutex> lock(g_mutex);
  for (mem_size i = 0; i < g_threads.size(); ++i) {
    if (g_threads[i].parked == &t_parked) {
      g_threads.erase(g_threads.begin() + i);
      break;
    }
  }
#endif
  t_guest = false;
  t_is_guest_pc = nullptr;
  t_exit = nullptr;
}

}  // namespace guest
