#include "uwp/thread_stack_win.h"
#include <windows.h>
#include <winternl.h>
#include <atomic>
#include <cstring>
#include "guest_abi.h"
#include "kern/process.h"
#include "kern/module.h"
#include "base/logging.h"
#include "uwp/memory_win.h"
namespace uwp {
namespace {
using Query = int(PS4ABI*)(void*, void**);
std::atomic<uintptr_t> posixQuery{0}, sceQuery{0};
int QueryStack(void* thread, void** attribute, uintptr_t target) {
  if (!target) return 22;
  const int result = reinterpret_cast<Query>(target)(thread, attribute);
  if (result) return result;
  auto kernel = kern::Process::GetActive()->GetModule(base::StringRef("libkernel"));
  if (!kernel) return result;
  auto self = kernel->GetSymbolByNid("EotR8a3ASf4");
  if (!self) self = kernel->GetSymbolByNid("aI+OeCz8xrQ");
  if (!self || reinterpret_cast<void*(PS4ABI*)()>(self)() != thread) return result;
  void* destination = nullptr;
  if (!ReadMemory(&destination, attribute, sizeof(destination)) || !destination) return result;
  auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
  const auto low = reinterpret_cast<uintptr_t>(tib->StackLimit);
  const auto high = reinterpret_cast<uintptr_t>(tib->StackBase);
  // PS4 pthread_attr: five 32-bit policy/flag fields + alignment, then
  // stack address (+0x18), size (+0x20), guard size (+0x28).
  const uintptr_t stack[] = {low, high-low, 0};
  if (high > low && WriteMemory(static_cast<char*>(destination)+0x18, stack, sizeof(stack)))
    BASE_LOGI("threadstack", "native thread stack reported {:#x}..{:#x}", low, high);
  return result;
}
int PS4ABI PosixQuery(void* thread, void** attribute) { return QueryStack(thread, attribute, posixQuery.load()); }
int PS4ABI SceQuery(void* thread, void** attribute) { return QueryStack(thread, attribute, sceQuery.load()); }
}
uintptr_t WrapThreadAttributeQuery(const char* nid, uintptr_t target) {
  if (!target) return target;
  if (!std::strcmp(nid,"Ucsu-OK+els")) { posixQuery=target; return reinterpret_cast<uintptr_t>(&PosixQuery); }
  if (!std::strcmp(nid,"x1X76arYMxU")) { sceQuery=target; return reinterpret_cast<uintptr_t>(&SceQuery); }
  return target;
}
}
