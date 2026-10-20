#pragma once
#if defined(DELTA_UWP)
#include <windows.h>
extern "C" __declspec(dllimport) BOOL WINAPI ReplaceFileFromAppW(
    LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
namespace io {
inline bool ReplaceFromApp(const wchar_t* source, const wchar_t* destination) {
  if (ReplaceFileFromAppW(destination, source, nullptr, 0, nullptr, nullptr)) return true;
  // First save: there is no existing destination to replace.
  if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
  return MoveFileFromAppW(source, destination) != FALSE;
}
}
#endif
