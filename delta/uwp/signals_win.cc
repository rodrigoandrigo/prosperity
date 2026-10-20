#include "uwp/signals_win.h"
#include <windows.h>
#include <array>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include "base/logging.h"
#include "cpu/backend.h"
#include "guest/session.h"
#include "guest_abi.h"
#include "kern/lv2/error_table.h"
#include "kern/lv2/sys_thread.h"
extern "C" __declspec(dllimport) VOID WINAPI RtlCaptureContext(PCONTEXT);
namespace uwp {
namespace {
using Mask = std::array<uint32_t, 4>;
struct Action { uintptr_t handler; uint32_t flags; Mask mask; uint32_t padding; };
static_assert(sizeof(Action) == 32);
struct Thread { Mask pending{}, blocked{}; std::condition_variable changed; };
struct Registry {
  std::mutex mutex;
  std::array<Action, 129> actions{};
  std::map<uint32_t, std::shared_ptr<Thread>> threads;
};
Registry& State() { static Registry state; return state; }
thread_local bool delivering = false;
const guest::SessionReset reset([] { delivering=false; auto& s=State(); std::lock_guard lock(s.mutex); s.threads.clear(); s.actions={}; });
std::shared_ptr<Thread> CurrentLocked(Registry& state) {
  auto& thread=state.threads[*kern::CurrentGuestTidPtr()];
  if (!thread) thread=std::make_shared<Thread>();
  return thread;
}
bool Pending(const Thread& thread) {
  for (size_t i=0;i<4;++i) if (thread.pending[i] & ~thread.blocked[i]) return true;
  return false;
}
void Invoke(int signal, const Action& action, const Mask& mask) {
  if (action.handler <= 1) return;
  // FreeBSD/Orbis ucontext is 0x500 bytes; mcontext begins at 0x40.
  // Capture the cooperative syscall boundary: guest roots saved by the
  // generated syscall bridge remain on this same native stack.
  alignas(16) unsigned char context[0x500]{};
  CONTEXT native{}; RtlCaptureContext(&native);
  std::memcpy(context, mask.data(), sizeof(mask));
  uint64_t registers[16]={0,native.Rdi,native.Rsi,native.Rdx,native.Rcx,native.R8,native.R9,
    native.Rax,native.Rbx,native.Rbp,native.R10,native.R11,native.R12,native.R13,native.R14,native.R15};
  std::memcpy(context+0x40, registers, sizeof(registers));
  auto put=[&](size_t offset,uint64_t value) { std::memcpy(context+0x40+offset,&value,8); };
  put(0xa0,native.Rip); put(0xa8,native.SegCs); put(0xb0,native.EFlags);
  put(0xb8,native.Rsp); put(0xc0,native.SegSs); put(0xc8,0x480);
  put(0x440,cpu::ThreadFsBase());
  std::memcpy(context+0x140,&native.FltSave,sizeof(native.FltSave));
  alignas(8) unsigned char information[80]{};
  std::memcpy(information,&signal,sizeof(signal));
  const int pid=0x1337; std::memcpy(information+12,&pid,sizeof(pid));
  const bool prior=delivering; delivering=true;
  reinterpret_cast<void(PS4ABI*)(int,void*,void*)>(action.handler)(signal,information,context);
  delivering=prior;
}
bool DispatchOne(const std::shared_ptr<Thread>& thread) {
  auto& state=State(); Action action{}; Mask prior{}; int signal=0;
  {
    std::lock_guard lock(state.mutex);
    for(int i=1;i<=128;++i) if ((thread->pending[(i-1)/32] & ~thread->blocked[(i-1)/32]) & (1u<<((i-1)%32))) { signal=i; break; }
    if (!signal) return false;
    thread->pending[(signal-1)/32] &= ~(1u<<((signal-1)%32));
    action=state.actions[signal]; prior=thread->blocked;
    for(size_t i=0;i<4;++i) thread->blocked[i] |= action.mask[i];
    thread->blocked[(signal-1)/32] |= 1u<<((signal-1)%32);
  }
  Invoke(signal,action,prior);
  { std::lock_guard lock(state.mutex); thread->blocked=prior; }
  return true;
}
}
int SignalAction(int signal,const void* action,void* previous) {
  if(signal<1 || signal>128 || signal==9 || signal==17) return -kern::SysError::eINVAL;
  auto& state=State(); std::lock_guard lock(state.mutex); CurrentLocked(state);
  if(previous) std::memcpy(previous,&state.actions[signal],sizeof(Action));
  if(action) { std::memcpy(&state.actions[signal],action,sizeof(Action)); BASE_LOGI("signal","sigaction {} handler={:#x} flags={:#x}",signal,state.actions[signal].handler,state.actions[signal].flags); }
  return 0;
}
int SignalMask(int how,const void* mask,void* previous) {
  auto& state=State(); std::lock_guard lock(state.mutex); auto thread=CurrentLocked(state);
  if(previous) std::memcpy(previous,thread->blocked.data(),sizeof(Mask));
  if(!mask) return 0;
  if(how<1 || how>3) return -kern::SysError::eINVAL;
  Mask bits{}; std::memcpy(bits.data(),mask,sizeof(bits));
  for(size_t i=0;i<4;++i) thread->blocked[i]=how==1 ? thread->blocked[i]|bits[i] : how==2 ? thread->blocked[i]&~bits[i] : bits[i];
  thread->blocked[0] &= ~((1u<<8)|(1u<<16));
  return 0;
}
int SendThreadSignal(uint32_t tid,int signal) {
  if(signal<0 || signal>128) return -kern::SysError::eINVAL;
  auto& state=State(); std::lock_guard lock(state.mutex);
  auto it=state.threads.find(tid); if(it==state.threads.end()) return -kern::SysError::eSRCH;
  if(signal) { it->second->pending[(signal-1)/32] |= 1u<<((signal-1)%32); it->second->changed.notify_all(); }
  return 0;
}
bool HasPendingSignal() {
  auto& state=State(); std::lock_guard lock(state.mutex); return Pending(*CurrentLocked(state));
}
void PollSignals() {
  if(delivering) return;
  std::shared_ptr<Thread> thread;
  { auto& state=State(); std::lock_guard lock(state.mutex); thread=CurrentLocked(state); }
  while(!guest::Stopping() && DispatchOne(thread)) {}
}
int SuspendForSignal(const void* mask) {
  if(!mask) return -kern::SysError::eFAULT;
  auto& state=State(); std::unique_lock lock(state.mutex); auto thread=CurrentLocked(state);
  Mask prior=thread->blocked; std::memcpy(thread->blocked.data(),mask,sizeof(Mask));
  while(!Pending(*thread) && !guest::Stopping()) thread->changed.wait_for(lock,std::chrono::milliseconds(2));
  lock.unlock(); if(!guest::Stopping()) DispatchOne(thread);
  lock.lock(); thread->blocked=prior;
  return -kern::SysError::eINTR;
}
}
