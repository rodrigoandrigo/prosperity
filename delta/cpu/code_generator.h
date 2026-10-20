#pragma once
#include <xbyak.h>
#if defined(DELTA_UWP)
#include "uwp/memory_win.h"
#include <sys/mman.h>
#include <map>
#include <mutex>
namespace cpu {
// Generated code must live in the managed executable address space. The
// default Xbyak allocator uses CRT heap pages, which cannot be protected by
// ProsperityVirtualProtect and must never be made executable as heap pages.
class CodeAllocator final : public Xbyak::Allocator {
 public:
  uint8_t* alloc(size_t size) override {
    auto* p = prosperity_mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    std::lock_guard lock(mutex_);
    allocations_.emplace(p, size);
    return static_cast<uint8_t*>(p);
  }
  void free(uint8_t* p) override {
    std::lock_guard lock(mutex_);
    auto it = allocations_.find(p);
    if (it == allocations_.end()) return;
    prosperity_munmap(p, it->second);
    allocations_.erase(it);
  }
 private:
  std::mutex mutex_;
  std::map<void*, size_t> allocations_;
};
inline CodeAllocator& GetCodeAllocator() { static CodeAllocator allocator; return allocator; }
class CodeGenerator : public Xbyak::CodeGenerator {
 public:
  explicit CodeGenerator(size_t size = 4096, void* buffer = nullptr)
      : Xbyak::CodeGenerator(size, buffer, &GetCodeAllocator()) {}
};
}
#else
namespace cpu { using CodeGenerator = Xbyak::CodeGenerator; }
#endif
