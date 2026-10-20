#include <windows.h>
#include <sys/mman.h>
#include "uwp/memory_win.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <thread>
#include <array>
#include "cpu/code_generator.h"

uint64_t budgetUsage = 0, budgetLimit = 2ull << 20;
bool budgetKnown = true;
int32_t PROSPERITY_CALL QueryBudget(void*, uint64_t* usage, uint64_t* limit) {
  *usage = budgetUsage; *limit = budgetLimit; return budgetKnown ? 0 : -1;
}

LONG CALLBACK Fault(EXCEPTION_POINTERS* exception) {
  const auto* record = exception->ExceptionRecord;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
      uwp::HandleMemoryFault(record->ExceptionInformation[1], record->ExceptionInformation[0] == 1,
          record->ExceptionInformation[0] == 8)) return EXCEPTION_CONTINUE_EXECUTION;
  std::printf("Unhandled mapping fault access=%llu address=%llx win=%lu\n",record->ExceptionInformation[0],record->ExceptionInformation[1],GetLastError());
  return EXCEPTION_CONTINUE_SEARCH;
}
#define REQUIRE(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s (win=%lu errno=%d)\n", __LINE__, #condition, GetLastError(), errno); return 1; } } while (0)
int RunTests(bool execution) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::puts("Starting Windows mapping checks");
  auto* memory = static_cast<unsigned char*>(mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  REQUIRE(memory != MAP_FAILED);
  std::puts("Anonymous view reserved");
  REQUIRE(memory[0] == 0);
  memory[123] = 77;
  REQUIRE(memory[123] == 77);
  std::puts("Demand read/write passed");
  REQUIRE(mprotect(memory, 4096, PROT_READ) == 0);
  unsigned char changed = 12;
  REQUIRE(!uwp::WriteMemory(memory + 123, &changed, 1));
  std::puts("Protected copy rejected");
  REQUIRE(memory[123] == 77);
  REQUIRE(mprotect(memory, 4096, PROT_READ | PROT_WRITE) == 0);
  const int backing = memfd_create("alias", 0);
  REQUIRE(backing >= 0 && ftruncate(backing, 0x100000) == 0);
  auto* first = static_cast<unsigned char*>(mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, backing, 0));
  auto* second = static_cast<unsigned char*>(mmap(nullptr, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, backing, 0x4000));
  REQUIRE(first != MAP_FAILED && second != MAP_FAILED);
  first[0x4010] = 99;
  REQUIRE(second[16] == 99);
  second[17] = 13;
  REQUIRE(first[0x4011] == 13);
  REQUIRE(close(backing) == 0);
  REQUIRE(mprotect(memory, 4096, PROT_READ) == 0);
  REQUIRE(munmap(memory + 0x4000, 0x4000) == 0);
  REQUIRE(!uwp::WriteMemory(memory + 123, &changed, 1));
  unsigned char residency;
  REQUIRE(mincore(memory + 0x4000, 4096, &residency) == -1);
  auto* middle = mmap(memory + 0x4000, 0x4000, PROT_READ | PROT_WRITE,
      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(middle == memory + 0x4000);
  REQUIRE(memory[123] == 77 && memory[0x4000] == 0);
  if (execution) {
  auto* code = static_cast<unsigned char*>(mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  REQUIRE(code != MAP_FAILED);
  const unsigned char program[] = {0xb8, 7, 0, 0, 0, 0xc3};
  std::memcpy(code, program, sizeof(program));
  auto run = reinterpret_cast<int(*)()>(code);
  REQUIRE(run() == 7);
  code[1] = 9;
  REQUIRE(run() == 9);
  REQUIRE(munmap(code, 0x10000) == 0);
  // ELF images start writable and receive executable protection only after
  // loading. Verify both the late promotion and real Xbyak thunk allocation.
  auto* image = static_cast<unsigned char*>(mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  REQUIRE(image != MAP_FAILED);
  std::memcpy(image, program, sizeof(program));
  REQUIRE(mprotect(image, 4096, PROT_READ | PROT_EXEC) == 0);
  REQUIRE(reinterpret_cast<int(*)()>(image)() == 7);
  REQUIRE(munmap(image, 0x10000) == 0);
  struct Generated : cpu::CodeGenerator { Generated() : CodeGenerator(128) { mov(eax, 37); ret(); } };
  Generated generated;
  REQUIRE(generated.getCode<int(*)()>()() == 37);
  }
  REQUIRE(munmap(first, 0x10000) == 0);
  REQUIRE(munmap(second, 0x4000) == 0);
  REQUIRE(munmap(memory, 0x10000) == 0);
  MEMORY_BASIC_INFORMATION released{};
  REQUIRE(VirtualQuery(memory, &released, sizeof(released)) != 0 && released.State == MEM_FREE);
  std::puts("Windows mappings: demand commit, 16 KiB shared aliases, partial remap and protection passed");
  std::puts(execution ? "W^X execution passed" : "W^X execution skipped: mapping-only run");
  return 0;
}
int DemandBudgetTests() {
  const auto initial = uwp::MemoryInfo();
  auto* ioTarget = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(ioTarget != MAP_FAILED);
  HANDLE ioFile = CreateFile2(L"build/uwp-platform-tests/read-target.bin", GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
  REQUIRE(ioFile != INVALID_HANDLE_VALUE);
  char contents[4096]; std::memset(contents, 0x5a, sizeof(contents)); DWORD transferred = 0;
  REQUIRE(WriteFile(ioFile, contents, sizeof(contents), &transferred, nullptr));
  LARGE_INTEGER start{}; REQUIRE(SetFilePointerEx(ioFile, start, nullptr, FILE_BEGIN));
  REQUIRE(uwp::PrepareWriteMemory(ioTarget, sizeof(contents)));
  REQUIRE(ReadFile(ioFile, ioTarget, sizeof(contents), &transferred, nullptr));
  REQUIRE(transferred == sizeof(contents) && !std::memcmp(ioTarget, contents, sizeof(contents)));
  CloseHandle(ioFile); DeleteFileW(L"build/uwp-platform-tests/read-target.bin");
  REQUIRE(munmap(ioTarget, 0x10000) == 0);
  std::puts("Native I/O: read into prepared demand-committed guest pages passed");
  constexpr size_t chunk = 16ull << 20, commit = 64ull << 10;
  const int fd = memfd_create("segmented", 0);
  REQUIRE(fd >= 0 && ftruncate(fd, chunk * 3) == 0);
  auto* first = static_cast<unsigned char*>(mmap(nullptr, chunk * 3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  auto* alias = static_cast<unsigned char*>(mmap(nullptr, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, chunk + 0x4000));
  REQUIRE(first != MAP_FAILED && alias != MAP_FAILED);
  REQUIRE(uwp::MemoryInfo().ram_chunks == initial.ram_chunks);
  REQUIRE(mprotect(first + chunk + 0x8000, 4096, PROT_READ) == 0);
  first[chunk + 0x4010] = 79;
  REQUIRE(alias[16] == 79);
  auto stats = uwp::MemoryInfo();
  REQUIRE(stats.ram_chunks == initial.ram_chunks + 1 && stats.committed_bytes == initial.committed_bytes + commit);
  unsigned char value = 5;
  REQUIRE(!uwp::WriteMemory(first + chunk + 0x8000, &value, 1));
  first[0] = 1; first[chunk * 2] = 2;
  REQUIRE(uwp::MemoryInfo().ram_chunks == initial.ram_chunks + 3);
  std::array<std::thread, 8> workers;
  for (size_t i = 0; i < workers.size(); ++i) workers[i] = std::thread([=] { first[commit + i * 4096] = 42; });
  for (auto& worker : workers) worker.join();
  REQUIRE(uwp::MemoryInfo().committed_bytes == initial.committed_bytes + 4 * commit);
  prosperity_memory_policy policy{sizeof(policy), QueryBudget, nullptr, budgetLimit, 256ull << 10};
  REQUIRE(uwp::ConfigureMemory(policy));
  budgetUsage = budgetLimit - policy.reserve_bytes - commit;
  REQUIRE(uwp::WriteMemory(first + commit * 2, &value, 1));
  const auto used = uwp::MemoryInfo();
  budgetUsage += commit;
  REQUIRE(!uwp::WriteMemory(first + commit * 3, &value, 1));
  REQUIRE(uwp::MemoryInfo().committed_bytes == used.committed_bytes);
  REQUIRE(uwp::MemoryInfo().rejected_commits == used.rejected_commits + 1);
  REQUIRE(alias[16] == 79);
  budgetKnown = false;
  REQUIRE(!uwp::WriteMemory(first + commit * 4, &value, 1));
  uwp::ClearMemoryPolicy();
  REQUIRE(munmap(first + chunk - 0x4000, 0x8000) == 0);
  auto* replacement = mmap(first + chunk - 0x4000, 0x8000, PROT_READ | PROT_WRITE,
      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(replacement == first + chunk - 0x4000);
  REQUIRE(alias[16] == 79 && first[chunk + 0x4010] == 79);
  REQUIRE(!uwp::WriteMemory(first + chunk + 0x8000, &value, 1));
  REQUIRE(close(fd) == 0 && munmap(first, chunk * 3) == 0 && munmap(alias, 0x4000) == 0);
  REQUIRE(uwp::MemoryInfo().committed_bytes == initial.committed_bytes && uwp::MemoryInfo().ram_chunks == initial.ram_chunks);
  std::puts("Demand RAM: lazy 16 MiB chunks, unique 64 KiB commits, aliases/guards, concurrent faults, exact budget/headroom, unavailable budget, cross-chunk remap and release passed");
  return 0;
}
int main(int argc, char** argv) {
  const bool execution = !(argc == 2 && std::strcmp(argv[1], "--mapping-only") == 0);
  auto previous = SetUnhandledExceptionFilter(Fault);
  int result = RunTests(execution);
  if (!result) result = DemandBudgetTests();
  SetUnhandledExceptionFilter(previous);
  return result;
}
