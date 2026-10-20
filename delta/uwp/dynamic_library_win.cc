#include <windows.h>
#include "base/dynamic_library.h"
#include "base/filesystem/path.h"
namespace base {
bool DynamicLibrary::Load(const Path& path, bool should_free) {
  if (handle_) return false;
  handle_ = LoadPackagedLibrary(path.c_str(), 0);
  should_free_ = handle_ && should_free;
  return loaded();
}
bool DynamicLibrary::LoadExisting(const Path& path) {
  if (handle_) return false;
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path.c_str(), &module)) return false;
  handle_ = module; should_free_ = false;
  return true;
}
bool DynamicLibrary::Free() {
  if (!handle_ || !should_free_) return false;
  if (!FreeLibrary(static_cast<HMODULE>(handle_))) return false;
  handle_ = nullptr; should_free_ = false;
  return true;
}
void* DynamicLibrary::FindSymbolPointer(const char* name) const {
  return handle_ ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name)) : nullptr;
}
}
