#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "uwp/memory_api.h"
namespace uwp {
struct MemoryMap { uintptr_t begin, end; uint64_t offset; int protection; bool direct; };
std::vector<MemoryMap> MemoryMaps();
int FindMemoryFile(const char* name, uint64_t& size);
bool HandleMemoryFault(uintptr_t address, bool write, bool execute);
bool PrepareWriteMemory(void* destination, size_t size);
bool ReadMemory(void* destination, const void* source, size_t size);
bool WriteMemory(void* destination, const void* source, size_t size);
bool InSafeMemoryCopy();
bool ConfigureMemory(const prosperity_memory_policy&);
prosperity_memory_info MemoryInfo();
void ClearMemoryPolicy();
const char* MemoryFaultError();
}
LPVOID WINAPI ProsperityVirtualAlloc(LPVOID address, SIZE_T size, DWORD flags, DWORD protection);
BOOL WINAPI ProsperityVirtualProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD previous);
BOOL WINAPI ProsperityVirtualFree(LPVOID address, SIZE_T size, DWORD flags);
