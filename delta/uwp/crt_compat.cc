// Adapt legacy API references embedded in the static MinGW C/C++ runtime.
// No desktop loader or process/file escape is introduced by these adapters.
#include "uwp/winapi_compat.h"
#include "io/path.h"
#include <cstring>
extern "C" __declspec(dllimport) BOOL WINAPI ReplaceFileFromAppW(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
namespace {
BOOL WINAPI Protect(LPVOID address, SIZE_T size, DWORD flags, PDWORD old) {
  return VirtualProtectFromApp(address, size, flags, old);
}
DWORD WINAPI AttributesA(LPCSTR path) { return ProsperityGetFileAttributesW(ProsperityWide(path, CP_ACP).c_str()); }
BOOL WINAPI Rename(LPCWSTR source, LPCWSTR destination, DWORD flags) {
  if (flags & ~MOVEFILE_REPLACE_EXISTING) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
  if (MoveFileFromAppW(source, destination)) return TRUE;
  if (!(flags & MOVEFILE_REPLACE_EXISTING) ||
      (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS)) return FALSE;
  return ReplaceFileFromAppW(destination, source, nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
}
BOOL WINAPI HardLink(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES) {
  SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
}
DWORD WINAPI TempPath(DWORD capacity, LPWSTR output) {
  auto path = io::MakeAbsPath(base::StringW(L"tmp"));
  if (!CreateDirectoryFromAppW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
  const size_t length = path.size();
  if (capacity <= length + 1) return static_cast<DWORD>(length + 2);
  std::memcpy(output, path.c_str(), length * sizeof(wchar_t));
  output[length] = L'\\'; output[length + 1] = 0;
  return static_cast<DWORD>(length + 1);
}
HLOCAL WINAPI FreeLocal(HLOCAL memory) {
  return !memory || HeapFree(GetProcessHeap(), 0, memory) ? nullptr : memory;
}
DWORD WINAPI FormatA(DWORD flags, LPCVOID source, DWORD message, DWORD language, LPSTR output, DWORD capacity, va_list* arguments) {
  // The runtime uses FROM_SYSTEM | IGNORE_INSERTS, with optional ALLOCATE_BUFFER.
  if (!output || !(flags & FORMAT_MESSAGE_FROM_SYSTEM) || !(flags & FORMAT_MESSAGE_IGNORE_INSERTS) || source) {
    SetLastError(ERROR_NOT_SUPPORTED); return 0;
  }
  auto* wide = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, 65536));
  if (!wide) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  const auto count = FormatMessageW(flags & ~FORMAT_MESSAGE_ALLOCATE_BUFFER, nullptr, message, language, wide, 32768, arguments);
  if (!count) { HeapFree(GetProcessHeap(), 0, wide); return 0; }
  const int bytes = WideCharToMultiByte(CP_ACP, 0, wide, count, nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) { HeapFree(GetProcessHeap(), 0, wide); return 0; }
  if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
    auto** destination = reinterpret_cast<char**>(output);
    output = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<size_t>(bytes) + 1));
    if (!output) { HeapFree(GetProcessHeap(), 0, wide); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    *destination = output;
    capacity = static_cast<DWORD>(bytes + 1);
  }
  const auto result = bytes > 0 && capacity > static_cast<DWORD>(bytes) ?
      WideCharToMultiByte(CP_ACP, 0, wide, count, output, bytes, nullptr, nullptr) : 0;
  if (result) output[result] = 0;
  HeapFree(GetProcessHeap(), 0, wide);
  return result;
}
BOOL WINAPI Terminate(HANDLE process, UINT status) {
  if (process != GetCurrentProcess()) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
  EXCEPTION_RECORD record{};
  record.ExceptionCode = STATUS_STACK_BUFFER_OVERRUN;
  record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
  record.NumberParameters = 1; record.ExceptionInformation[0] = status;
  RaiseFailFastException(&record, nullptr, 0);
  return FALSE;
}
}
extern "C" {
decltype(&ProsperityCreateFileW) __imp_CreateFileW = &ProsperityCreateFileW;
decltype(&DeleteFileFromAppW) __imp_DeleteFileW = &DeleteFileFromAppW;
decltype(&RemoveDirectoryFromAppW) __imp_RemoveDirectoryW = &RemoveDirectoryFromAppW;
decltype(&ProsperityGetFileAttributesW) __imp_GetFileAttributesW = &ProsperityGetFileAttributesW;
decltype(&AttributesA) __imp_GetFileAttributesA = &AttributesA;
decltype(&Rename) __imp_MoveFileExW = &Rename;
decltype(&HardLink) __imp_CreateHardLinkW = &HardLink;
decltype(&TempPath) __imp_GetTempPathW = &TempPath;
decltype(&Protect) __imp_VirtualProtect = &Protect;
decltype(&FreeLocal) __imp_LocalFree = &FreeLocal;
decltype(&FormatA) __imp_FormatMessageA = &FormatA;
decltype(&Terminate) __imp_TerminateProcess = &Terminate;
// Fast-fail instructions are supported by the Windows 10 minimum platform.
BOOL WINAPI IsProcessorFeaturePresent(DWORD feature) { return feature == PF_FASTFAIL_AVAILABLE; }
}
