#include "uwp/memory_win.h"
#include <sys/mman.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>
#include <cstdio>
#include <io.h>
#undef close
#undef ftruncate
#undef VirtualAlloc
#undef VirtualProtect
#undef VirtualFree

// Keep the native protection entry point for mapped executable sections,
// matching the working GR2fork UWP memory implementation.
extern "C" __declspec(dllimport) BOOL WINAPI VirtualProtect(LPVOID, SIZE_T, DWORD, PDWORD);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateFileMappingW(HANDLE, SECURITY_ATTRIBUTES*, DWORD, DWORD, DWORD, LPCWSTR);
extern "C" __declspec(dllimport) PVOID WINAPI MapViewOfFile3(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

namespace {
constexpr uintptr_t kPage = 4096;
constexpr uint64_t kBackingLimit = 1ull << 40;
constexpr uint64_t kChunkSize = 16ull << 20;
constexpr uint64_t kCommitSize = 64ull << 10;
std::atomic<uint64_t> committedBytes{0}, ramChunks{0}, rejectedCommits{0};
struct RamChunk {
  HANDLE section = nullptr;
  void* canonical = nullptr;
  uint64_t committed = 0;
  ~RamChunk() {
    if (canonical) { UnmapViewOfFile(canonical); --ramChunks; }
    if (section) CloseHandle(section);
    committedBytes -= committed;
  }
};
struct Backing {
  std::string name;
  std::map<uint64_t, std::shared_ptr<RamChunk>> chunks;
  uint64_t size = 0;
  bool executable = false;
};
struct Mapping {
  uintptr_t end;
  std::shared_ptr<Backing> backing;
  uint64_t offset = 0;
  int protection = 0;
  bool present = true;
  std::map<uintptr_t, int> overrides;
  bool mapped = false;
};
struct State {
  std::recursive_mutex mutex;
  std::map<uintptr_t, Mapping> maps;
  std::map<int, std::shared_ptr<Backing>> descriptors;
  std::map<uintptr_t, uintptr_t> reservations;
  int next_descriptor = 0x1000000;
  prosperity_memory_policy policy{sizeof(prosperity_memory_policy), nullptr, nullptr, 5120ull << 20, 256ull << 20};
};
State& Registry() { static State state; return state; }
thread_local bool t_safe_copy = false;
thread_local char t_memory_error[256]{};
uintptr_t Down(uintptr_t value) { return value & ~(kPage - 1); }
uintptr_t Up(uintptr_t value) { return (value + kPage - 1) & ~(kPage - 1); }
bool Span(void* address, size_t size) {
  return size && size <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(address) - (kPage - 1);
}
int Failure(DWORD error = GetLastError()) {
  errno = error == ERROR_ACCESS_DENIED ? EACCES : error == ERROR_INVALID_PARAMETER ? EINVAL : ENOMEM;
  return -1;
}
DWORD NativeProtection(int protection, bool execution = false) {
  if (!protection) return PAGE_NOACCESS;
  if ((protection & PROT_EXEC) && (execution || !(protection & PROT_WRITE))) return PAGE_EXECUTE_READ;
  return (protection & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
}
int At(const Mapping& mapping, uintptr_t page) {
  auto it = mapping.overrides.upper_bound(page);
  return it == mapping.overrides.begin() ? mapping.protection : (--it)->second;
}
auto Find(State& state, uintptr_t address) {
  auto it = state.maps.upper_bound(address);
  if (it != state.maps.begin()) --it;
  if (it == state.maps.end() || it->first > address || it->second.end <= address)
    return state.maps.end();
  return it;
}
bool SplitPlaceholder(uintptr_t begin, uintptr_t end) {
  MEMORY_BASIC_INFORMATION information{};
  if (!VirtualQuery(reinterpret_cast<void*>(begin), &information, sizeof(information)) ||
      information.State != MEM_RESERVE) return false;
  // VirtualQuery can report a subregion inside one placeholder after a mapped
  // section was committed. Splitting must start at the reservation boundary.
  const auto first = reinterpret_cast<uintptr_t>(information.AllocationBase);
  if (first < begin && !VirtualFree(reinterpret_cast<void*>(first), begin - first,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return false;
  if (!VirtualQuery(reinterpret_cast<void*>(begin), &information, sizeof(information))) return false;
  const auto last = reinterpret_cast<uintptr_t>(information.BaseAddress) + information.RegionSize;
  if (end < last && !VirtualFree(reinterpret_cast<void*>(begin), end - begin,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return false;
  return true;
}
bool MapSection(uintptr_t begin, Mapping& mapping) {
  const auto index = mapping.offset / kChunkSize;
  auto& chunk = mapping.backing->chunks[index];
  if (!chunk) {
    auto candidate = std::make_shared<RamChunk>();
    candidate->section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, static_cast<DWORD>(kChunkSize), nullptr);
    if (!candidate->section) return false;
    candidate->canonical = MapViewOfFileFromApp(candidate->section, FILE_MAP_READ | FILE_MAP_WRITE, 0, kChunkSize);
    if (!candidate->canonical) return false;
    ++ramChunks;
    chunk = std::move(candidate);
  }
  if (!SplitPlaceholder(begin, mapping.end)) return false;
  // The reserved view must retain the section's execute capability. Actual
  // committed pages receive their logical RW/RX/guard protection below and
  // at demand commit, before the faulting guest instruction resumes.
  auto* view = MapViewOfFile3(chunk->section, GetCurrentProcess(),
      reinterpret_cast<void*>(begin), mapping.offset % kChunkSize, mapping.end - begin,
      MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE,
      nullptr, 0);
  if (view != reinterpret_cast<void*>(begin)) return false;
  mapping.mapped = true;
  // Remapping a section restores committed pages. Reapply logical per-page
  // protections; otherwise splitting a view would silently make guards writable.
  auto cursor = begin;
  while (cursor < mapping.end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) return false;
    auto next = std::min(mapping.end, reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize);
    const auto change = mapping.overrides.upper_bound(cursor);
    if (change != mapping.overrides.end()) next = std::min(next, change->first);
    if (info.State == MEM_COMMIT) {
      DWORD previous;
      if (!VirtualProtect(reinterpret_cast<void*>(cursor), next - cursor,
          NativeProtection(At(mapping, cursor)), &previous)) return false;
    }
    cursor = next;
  }
  return true;
}
// Retain the underlying placeholder for a hole. This permits 16 KiB fixed
// guest remaps even though new Windows reservations require 64 KiB alignment.
bool RemoveRange(State& state, uintptr_t begin, uintptr_t end) {
  auto it = state.maps.upper_bound(begin);
  if (it != state.maps.begin()) --it;
  std::vector<std::pair<uintptr_t, Mapping>> survivors;
  while (it != state.maps.end() && it->first < end) {
    const auto first = it->first;
    auto mapping = it->second;
    if (mapping.end <= begin) { ++it; continue; }
    if (mapping.mapped && !UnmapViewOfFile2(GetCurrentProcess(),
        reinterpret_cast<void*>(first), MEM_PRESERVE_PLACEHOLDER)) return false;
    it = state.maps.erase(it);
    if (first < begin) {
      auto left = mapping;
      left.end = begin;
      if (left.mapped) { left.mapped = false; if (!MapSection(first, left)) return false; }
      survivors.emplace_back(first, std::move(left));
    }
    if (mapping.end > end) {
      auto right = mapping;
      right.offset += end - first;
      if (right.mapped) { right.mapped = false; if (!MapSection(end, right)) return false; }
      survivors.emplace_back(end, std::move(right));
    }
    const uintptr_t hole_begin = std::max(first, begin), hole_end = std::min(mapping.end, end);
    if (!SplitPlaceholder(hole_begin, hole_end)) return false;
    survivors.emplace_back(hole_begin, Mapping{hole_end, nullptr, 0, 0, false, {}});
  }
  for (auto& entry : survivors) state.maps.insert_or_assign(entry.first, std::move(entry.second));
  return true;
}
bool ReleaseEmptyReservations(State& state) {
  for (auto reservation = state.reservations.begin(); reservation != state.reservations.end();) {
    const auto begin = reservation->first, end = reservation->second;
    auto mapping = state.maps.lower_bound(begin);
    bool live = false;
    for (auto it = mapping; it != state.maps.end() && it->first < end; ++it)
      live |= it->second.present;
    if (live) { ++reservation; continue; }
    auto cursor = begin;
    while (cursor < end) {
      MEMORY_BASIC_INFORMATION info{};
      if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) return false;
      const auto next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
      if (info.State != MEM_FREE && !VirtualFree(info.AllocationBase, 0, MEM_RELEASE)) return false;
      cursor = next;
    }
    state.maps.erase(mapping, state.maps.lower_bound(end));
    reservation = state.reservations.erase(reservation);
  }
  return true;
}
std::shared_ptr<Backing> NewBacking(bool executable) {
  auto backing = std::make_shared<Backing>();
  backing->executable = executable;
  return backing;
}
}

extern "C" void* prosperity_mmap(void* address, size_t length, int protection,
    int flags, int descriptor, int64_t offset) {
  if (!Span(address, length) || offset < 0 || (offset & (kPage - 1)) ||
      (protection & ~7) || ((flags & (MAP_SHARED | MAP_PRIVATE)) == 0) ||
      ((flags & (MAP_SHARED | MAP_PRIVATE)) == (MAP_SHARED | MAP_PRIVATE))) {
    errno = EINVAL; return MAP_FAILED;
  }
  auto& state = Registry();
  std::lock_guard lock(state.mutex);
  uintptr_t begin = reinterpret_cast<uintptr_t>(address);
  const size_t size = Up(length);
  if (!(flags & MAP_ANONYMOUS) && (flags & MAP_PRIVATE)) {
    // Shared section aliases are implemented; silently sharing a private file
    // mapping would corrupt the backing store. Copy-on-write remains pending.
    errno = ENOTSUP; return MAP_FAILED;
  }
  if (begin & (kPage - 1)) { errno = EINVAL; return MAP_FAILED; }
  bool owned = begin && Find(state, begin) != state.maps.end();
  if (flags & MAP_FIXED_NOREPLACE) {
    auto cursor = begin;
    while (cursor < begin + size) {
      auto it = Find(state, cursor);
      if (it == state.maps.end()) break;
      if (it->second.present) { errno = EEXIST; return MAP_FAILED; }
      cursor = it->second.end;
    }
  }
  if (!owned || !(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) {
    // Windows picks an address unless the caller explicitly requires its hint.
    auto* requested = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE) ? address : nullptr;
    void* reserved = VirtualAlloc2FromApp(GetCurrentProcess(), requested, size,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!reserved) { Failure(); return MAP_FAILED; }
    begin = reinterpret_cast<uintptr_t>(reserved);
    state.reservations.emplace(begin, begin + size);
    state.maps.emplace(begin, Mapping{begin + size, nullptr, 0, 0, false, {}});
  } else if (!RemoveRange(state, begin, begin + size)) {
    Failure(); return MAP_FAILED;
  }
  // A range can span placeholders split by earlier guest mappings.
  if (!SplitPlaceholder(begin, begin + size)) { Failure(); return MAP_FAILED; }
  MEMORY_BASIC_INFORMATION information{};
  VirtualQuery(reinterpret_cast<void*>(begin), &information, sizeof(information));
  if (information.RegionSize != size &&
      !VirtualFree(reinterpret_cast<void*>(begin), size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
    Failure(); return MAP_FAILED;
  }
  std::shared_ptr<Backing> backing;
  if (!(flags & MAP_ANONYMOUS)) {
    auto found = state.descriptors.find(descriptor);
    if (found == state.descriptors.end() || static_cast<uint64_t>(offset) + size > kBackingLimit) {
      errno = EBADF; return MAP_FAILED;
    }
    backing = found->second;
    if (backing->chunks.empty()) backing->executable = (protection & PROT_EXEC) != 0;
  } else if (protection) {
    backing = NewBacking((protection & PROT_EXEC) != 0);
    if (!backing) { Failure(); return MAP_FAILED; }
  }
  for (auto it = state.maps.lower_bound(begin); it != state.maps.end() && it->first < begin + size;)
    it = state.maps.erase(it);
  auto cursor = begin;
  while (cursor < begin + size) {
    const uint64_t physical = uint64_t(offset) + cursor - begin;
    const auto next = backing ? std::min(begin + size, cursor + uintptr_t(kChunkSize - physical % kChunkSize)) : begin + size;
    if (!SplitPlaceholder(cursor, next)) { Failure(); return MAP_FAILED; }
    state.maps.emplace(cursor, Mapping{next, backing, physical, protection, true, {}});
    cursor = next;
  }
  return reinterpret_cast<void*>(begin);
}
extern "C" int prosperity_munmap(void* address, size_t length) {
  if (!Span(address, length) || (reinterpret_cast<uintptr_t>(address) & (kPage - 1))) { errno = EINVAL; return -1; }
  auto& state = Registry();
  std::lock_guard lock(state.mutex);
  const auto begin = reinterpret_cast<uintptr_t>(address);
  return RemoveRange(state, begin, begin + Up(length)) && ReleaseEmptyReservations(state) ? 0 : Failure();
}
extern "C" int prosperity_mprotect(void* address, size_t length, int protection) {
  if (!Span(address, length) || (reinterpret_cast<uintptr_t>(address) & (kPage - 1)) || (protection & ~7)) {
    errno = EINVAL; return -1;
  }
  auto& state = Registry();
  std::lock_guard lock(state.mutex);
  const auto end = reinterpret_cast<uintptr_t>(address) + Up(length);
  auto cursor = reinterpret_cast<uintptr_t>(address);
  while (cursor < end) {
    auto it = Find(state, cursor);
    if (it == state.maps.end() || !it->second.present) { errno = ENOMEM; return -1; }
    auto& mapping = it->second;
    const auto last = std::min(end, mapping.end);
    const int old_end = last < mapping.end ? At(mapping, last) : mapping.protection;
    mapping.overrides.erase(mapping.overrides.lower_bound(cursor), mapping.overrides.lower_bound(last));
    mapping.overrides[cursor] = protection;
    if (last < mapping.end) mapping.overrides[last] = old_end;
    while (cursor < last) {
      MEMORY_BASIC_INFORMATION information{};
      if (!VirtualQuery(reinterpret_cast<void*>(cursor), &information, sizeof(information))) return Failure();
      const auto next = std::min(last, reinterpret_cast<uintptr_t>(information.BaseAddress) + information.RegionSize);
      if (information.State == MEM_COMMIT) {
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(cursor), next - cursor,
            NativeProtection(protection), &previous)) return Failure();
      }
      cursor = next;
    }
  }
  return 0;
}
extern "C" int prosperity_mincore(void* address, size_t length, unsigned char* output) {
  if (!Span(address, length) || !output) { errno = EINVAL; return -1; }
  auto& state = Registry();
  std::lock_guard lock(state.mutex);
  auto cursor = reinterpret_cast<uintptr_t>(address);
  const auto end = cursor + Up(length);
  for (size_t index = 0; cursor < end; cursor += kPage, ++index) {
    auto it = Find(state, cursor);
    if (it != state.maps.end() && !it->second.present) { errno = ENOMEM; return -1; }
    MEMORY_BASIC_INFORMATION information{};
    if (!VirtualQuery(reinterpret_cast<void*>(cursor), &information, sizeof(information)) || information.State == MEM_FREE) {
      errno = ENOMEM; return -1;
    }
    output[index] = information.State == MEM_COMMIT ? 1 : 0;
  }
  return 0;
}
extern "C" int prosperity_madvise(void* address, size_t length, int advice) {
  if (!Span(address, length)) { errno = EINVAL; return -1; }
  if (advice == MADV_DONTDUMP) return 0; // AppContainer has no guest-controlled process dumps.
  if (advice != MADV_DONTNEED && advice != MADV_FREE) { errno = EINVAL; return -1; }
  // MADV_FREE may retain contents; only DONTNEED promises zero-filled pages.
  if (advice == MADV_FREE) { errno = ENOTSUP; return -1; }
  const auto begin = reinterpret_cast<uintptr_t>(address);
  if (begin & (kPage - 1)) { errno = EINVAL; return -1; }
  auto& state = Registry(); std::lock_guard lock(state.mutex);
  const auto end = begin + Up(length);
  std::vector<std::pair<uintptr_t, Mapping>> replacements;
  std::vector<std::shared_ptr<Backing>> old_backings;
  for (auto cursor = begin; cursor < end;) {
    auto it = Find(state, cursor);
    if (it == state.maps.end() || !it->second.present || !it->second.backing) { errno = ENOMEM; return -1; }
    const auto& original = it->second;
    if (!original.backing->name.empty()) { errno = ENOTSUP; return -1; }
    // A descriptor-backed section can have aliases outside this view. Never
    // discard its contents as though it were private anonymous memory.
    for (const auto& [fd, backing] : state.descriptors)
      if (backing == original.backing) { errno = ENOTSUP; return -1; }
    const auto next = std::min(end, original.end);
    auto replacement = original;
    replacement.end = next;
    replacement.offset = 0;
    replacement.mapped = false;
    replacement.backing = NewBacking(original.backing->executable);
    replacement.backing->size = next - cursor;
    replacements.emplace_back(cursor, std::move(replacement));
    if (std::find(old_backings.begin(), old_backings.end(), original.backing) == old_backings.end())
      old_backings.push_back(original.backing);
    cursor = next;
  }
  if (!RemoveRange(state, begin, end)) return Failure();
  for (auto& entry : replacements) state.maps.insert_or_assign(entry.first, std::move(entry.second));
  // Release whole physical chunks no longer used by a surviving view. Partial
  // chunks remain alive for their neighbours; new discarded ranges are lazy.
  for (const auto& backing : old_backings) {
    for (auto chunk = backing->chunks.begin(); chunk != backing->chunks.end();) {
      bool live = false;
      const auto lo = chunk->first * kChunkSize, hi = lo + kChunkSize;
      for (const auto& [first, mapping] : state.maps)
        if (mapping.present && mapping.backing == backing && mapping.offset < hi &&
            mapping.offset + mapping.end - first > lo) { live = true; break; }
      if (live) ++chunk; else chunk = backing->chunks.erase(chunk);
    }
  }
  return 0;
}
extern "C" int prosperity_memfd_create(const char* name, unsigned) {
  auto& state = Registry();
  std::lock_guard lock(state.mutex);
  const int descriptor = state.next_descriptor++;
  state.descriptors.emplace(descriptor, std::make_shared<Backing>());
  state.descriptors[descriptor]->name = name ? name : "";
  return descriptor;
}
extern "C" int prosperity_ftruncate(int descriptor, int64_t length) {
  auto& state = Registry();
  std::lock_guard lock(state.mutex);
  auto it = state.descriptors.find(descriptor);
  if (it == state.descriptors.end()) return _chsize_s(descriptor, length) == 0 ? 0 : -1;
  if (length < 0 || static_cast<uint64_t>(length) > kBackingLimit) { errno = EINVAL; return -1; }
  it->second->size = length;
  return 0;
}
extern "C" int prosperity_close(int descriptor) {
  auto& state = Registry();
  std::lock_guard lock(state.mutex);
  if (state.descriptors.erase(descriptor)) return 0;
  return _close(descriptor);
}
namespace uwp {
bool ConfigureMemory(const prosperity_memory_policy& policy) {
  if (policy.size != sizeof(policy) || !policy.cap_bytes || policy.reserve_bytes >= policy.cap_bytes) return false;
  auto& state = Registry(); std::lock_guard lock(state.mutex); state.policy = policy; return true;
}
void ClearMemoryPolicy() {
  auto& state = Registry(); std::lock_guard lock(state.mutex);
  state.policy = {sizeof(prosperity_memory_policy), nullptr, nullptr, 5120ull << 20, 256ull << 20};
}
prosperity_memory_info MemoryInfo() {
  auto& state = Registry(); std::lock_guard lock(state.mutex);
  return {sizeof(prosperity_memory_info), committedBytes.load(), ramChunks.load(), kChunkSize,
      kCommitSize, state.policy.cap_bytes, state.policy.reserve_bytes, rejectedCommits.load()};
}
bool QueryApplicationMemory(uint64_t& usage, uint64_t& limit) {
  auto& state = Registry(); std::lock_guard lock(state.mutex);
  usage = limit = 0;
  return state.policy.query && state.policy.query(state.policy.user, &usage, &limit) == 0;
}
int FindMemoryFile(const char* name, uint64_t& size) {
  auto& state = Registry(); std::lock_guard lock(state.mutex);
  for (const auto& [descriptor, backing] : state.descriptors) if (backing->name == name) {
    size = backing->size; return descriptor;
  }
  return -1;
}
std::vector<MemoryMap> MemoryMaps() {
  auto& state = Registry(); std::lock_guard lock(state.mutex);
  std::vector<MemoryMap> result;
  for (const auto& [begin, mapping] : state.maps) {
    if (!mapping.present) continue;
    auto cursor = begin;
    while (cursor < mapping.end) {
      auto next = mapping.end;
      const auto change = mapping.overrides.upper_bound(cursor);
      if (change != mapping.overrides.end()) next = std::min(next, change->first);
      result.push_back({cursor, next, mapping.offset + cursor - begin, At(mapping, cursor),
          mapping.backing && mapping.backing->name == "delta_dmem"});
      cursor = next;
    }
  }
  return result;
}
bool HandleMemoryFault(uintptr_t address, bool write, bool execute) {
  t_memory_error[0] = 0;
  try {
  auto& state = Registry();
  std::lock_guard lock(state.mutex);
  auto it = Find(state, address);
  if (it == state.maps.end() || !it->second.present || !it->second.backing) return false;
  const int protection = At(it->second, Down(address));
  if ((write && !(protection & PROT_WRITE)) || (execute && !(protection & PROT_EXEC)) ||
      (!write && !execute && !(protection & PROT_READ))) return false;
  auto& mapping = it->second;
  if (!mapping.mapped && !MapSection(it->first, mapping)) return false;
  const auto physical = mapping.offset + address - it->first;
  auto chunk = mapping.backing->chunks.at(physical / kChunkSize);
  const auto local = (physical % kChunkSize) & ~(kCommitSize - 1);
  auto* canonical = static_cast<unsigned char*>(chunk->canonical) + local;
  MEMORY_BASIC_INFORMATION physicalInfo{};
  if (!VirtualQuery(canonical, &physicalInfo, sizeof(physicalInfo))) return false;
  if (physicalInfo.State != MEM_COMMIT) {
    uint64_t usage = committedBytes.load(), limit = state.policy.cap_bytes;
    bool known = true;
    if (state.policy.query) {
      uint64_t appLimit = 0;
      known = state.policy.query(state.policy.user, &usage, &appLimit) == 0 && appLimit;
      limit = std::min(limit, appLimit);
    }
    if (!known || usage >= limit || kCommitSize > limit - usage || state.policy.reserve_bytes > limit - usage - kCommitSize) {
      std::snprintf(t_memory_error, sizeof(t_memory_error),
          "UWP RAM budget exhausted/unavailable: usage=%llu limit=%llu reserve=%llu request=%llu; swap disabled",
          static_cast<unsigned long long>(usage), static_cast<unsigned long long>(limit),
          static_cast<unsigned long long>(state.policy.reserve_bytes), static_cast<unsigned long long>(kCommitSize));
      ++rejectedCommits; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false;
    }
    if (!VirtualAllocFromApp(canonical, kCommitSize, MEM_COMMIT, PAGE_READWRITE)) return false;
    chunk->committed += kCommitSize; committedBytes += kCommitSize;
    const auto commitBegin = physical & ~(kCommitSize - 1), commitEnd = commitBegin + kCommitSize;
    for (auto& [base, alias] : state.maps) {
      if (!alias.mapped || alias.backing != mapping.backing) continue;
      const auto first = std::max(commitBegin, alias.offset);
      const auto last = std::min(commitEnd, alias.offset + alias.end - base);
      if (first >= last) continue;
      for (auto page = base + first - alias.offset; page < base + last - alias.offset; page += kPage) {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(page), kPage, NativeProtection(At(alias, page)), &old)) return false;
      }
    }
  }
  MEMORY_BASIC_INFORMATION information{};
  if (!VirtualQuery(reinterpret_cast<void*>(address), &information, sizeof(information))) return false;
  void* page = reinterpret_cast<void*>(Down(address));
  if (information.State != MEM_COMMIT) return false;
  DWORD previous;
  const auto native = NativeProtection(protection, execute);
  if (!VirtualProtect(page, kPage, native, &previous)) return false;
  if (execute) FlushInstructionCache(GetCurrentProcess(), page, kPage);
  return true;
  } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
}
const char* MemoryFaultError() { return t_memory_error[0] ? t_memory_error : nullptr; }
bool InSafeMemoryCopy() { return t_safe_copy; }
namespace {
bool PrepareCopyRange(const void* pointer, size_t size, bool write) {
  auto cursor = reinterpret_cast<uintptr_t>(pointer);
  if (size > UINTPTR_MAX - cursor) return false;
  const auto end = cursor + size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) return false;
    auto protection = info.Protect & 0xff;
    const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const bool readable = writable || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) || !(write ? writable : readable)) {
      if (!HandleMemoryFault(cursor, write, false)) return false;
      continue;
    }
    cursor = std::min(end, reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize);
  }
  return true;
}
LONG SafeCopyException(EXCEPTION_POINTERS* exception) {
  const auto* record = exception->ExceptionRecord;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
      HandleMemoryFault(record->ExceptionInformation[1], record->ExceptionInformation[0] == 1,
          record->ExceptionInformation[0] == 8)) return EXCEPTION_CONTINUE_EXECUTION;
  return EXCEPTION_EXECUTE_HANDLER;
}
}
bool PrepareWriteMemory(void* destination, size_t size) { return PrepareCopyRange(destination, size, true); }
bool ReadMemory(void* destination, const void* source, size_t size) {
  if (!PrepareCopyRange(source, size, false) || !PrepareCopyRange(destination, size, true)) return false;
  const bool previous = t_safe_copy;
  t_safe_copy = true;
  bool result = true;
  __try { std::memcpy(destination, source, size); }
  __except (SafeCopyException(GetExceptionInformation())) { result = false; }
  t_safe_copy = previous;
  return result;
}
bool WriteMemory(void* destination, const void* source, size_t size) { return ReadMemory(destination, source, size); }
}

LPVOID WINAPI ProsperityVirtualAlloc(LPVOID address, SIZE_T size, DWORD flags, DWORD protection) {
  int prot = protection == PAGE_NOACCESS ? 0 : PROT_READ;
  if (protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE) prot |= PROT_WRITE;
  if (protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE) prot |= PROT_EXEC;
  if (!(flags & MEM_COMMIT)) prot = 0;
  void* result = prosperity_mmap(address, size, prot, MAP_PRIVATE | MAP_ANONYMOUS |
      (address ? ((flags & MEM_RESERVE) ? MAP_FIXED_NOREPLACE : MAP_FIXED) : 0), -1, 0);
  return result == MAP_FAILED ? nullptr : result;
}
BOOL WINAPI ProsperityVirtualProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD previous) {
  if (!previous) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  MEMORY_BASIC_INFORMATION information{};
  VirtualQuery(address, &information, sizeof(information));
  *previous = information.Protect;
  int prot = protection == PAGE_NOACCESS ? 0 : PROT_READ;
  if (protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE) prot |= PROT_WRITE;
  if (protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE) prot |= PROT_EXEC;
  return prosperity_mprotect(address, size, prot) == 0;
}
BOOL WINAPI ProsperityVirtualFree(LPVOID address, SIZE_T size, DWORD flags) {
  if (flags == MEM_RELEASE && !size) {
    auto& state = Registry();
    std::lock_guard lock(state.mutex);
    auto it = Find(state, reinterpret_cast<uintptr_t>(address));
    if (it == state.maps.end()) return FALSE;
    size = it->second.end - reinterpret_cast<uintptr_t>(address);
  }
  return prosperity_munmap(address, size) == 0;
}


