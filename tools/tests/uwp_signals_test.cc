#include <windows.h>
#include <atomic>
#include <array>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "base/standard_streams.h"
namespace base { void WriteStandardError(const char* text, mem_size size) noexcept { std::fwrite(text,1,size,stderr); } namespace detail { [[noreturn]] void FatalCheckFailure(const char*,int,const char*) { std::abort(); } } }
#include "uwp/signals_win.h"
#include "guest_abi.h"
#include "guest/session.h"
namespace { thread_local u32 tid=1; std::atomic<bool> stopping{false}, ready{false}, suspended{false}, resumed{false}, done{false}; std::atomic<int> delivered{0}; }
namespace kern { const u32* CurrentGuestTidPtr() { return &tid; } }
namespace guest { bool Stopping() { return stopping; } void RegisterSessionReset(void(*)()) {} }
namespace cpu { u64 ThreadFsBase() { return 0; } }
struct Action { uintptr_t handler; uint32_t flags; std::array<uint32_t,4> mask; uint32_t padding; };
void PS4ABI Resume(int signal,void*,void*) { if(signal==31) resumed=true; }
void PS4ABI Suspend(int signal,void*,void* context) {
  uint64_t rsp=0; std::memcpy(&rsp,static_cast<char*>(context)+0x40+0xb8,8);
  if(signal!=30 || !rsp) { stopping=true; return; }
  ++delivered; suspended=true;
  std::array<uint32_t,4> mask; mask.fill(~0u); mask[0]&=~(1u<<30);
  while(!resumed && !stopping) uwp::SuspendForSignal(mask.data());
}
int main() {
  Action suspend{reinterpret_cast<uintptr_t>(&Suspend),0x40,{},0};
  Action resume{reinterpret_cast<uintptr_t>(&Resume),0x40,{},0};
  if(uwp::SignalAction(30,&suspend,nullptr) || uwp::SignalAction(31,&resume,nullptr)) return 1;
  std::thread worker([] { tid=2; uwp::HasPendingSignal(); ready=true; while(!stopping) { uwp::PollSignals(); Sleep(1); } done=true; });
  auto wait=[](const std::atomic<bool>& flag) { for(int i=0;i<5000 && !flag;++i) Sleep(1); return flag.load(); };
  if(!wait(ready) || uwp::SendThreadSignal(2,30) || !wait(suspended) || resumed || uwp::SendThreadSignal(2,31) || !wait(resumed)) {
    stopping=true; worker.join(); std::puts("FAIL cooperative signal suspend/resume"); return 1;
  }
  stopping=true; worker.join();
  if(delivered!=1 || !done || uwp::SendThreadSignal(999,30)>=0) return 1;
  std::puts("PASS: guest signal handler registration, target-thread delivery, Orbis context, GC suspend/resume handshake and unknown target");
}
