#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fileapi.h>
#include <string>
#include <vector>
#include <cwchar>
extern "C" __declspec(dllimport) HANDLE WINAPI CreateFile2FromAppW(
    LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
extern "C" __declspec(dllimport) BOOL WINAPI MoveFileFromAppW(LPCWSTR, LPCWSTR);
extern "C" __declspec(dllimport) BOOL WINAPI CreateDirectoryFromAppW(LPCWSTR, LPSECURITY_ATTRIBUTES);
extern "C" __declspec(dllimport) BOOL WINAPI DeleteFileFromAppW(LPCWSTR);
extern "C" __declspec(dllimport) BOOL WINAPI RemoveDirectoryFromAppW(LPCWSTR);
extern "C" __declspec(dllimport) BOOL WINAPI GetFileAttributesExFromAppW(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
extern "C" __declspec(dllimport) BOOL WINAPI SetFileAttributesFromAppW(LPCWSTR, DWORD);
extern "C" __declspec(dllimport) HANDLE WINAPI FindFirstFileExFromAppW(LPCWSTR,
    FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);

// Preserve desktop open/share semantics using the AppContainer file entry point.
inline HANDLE ProsperityCreateFileW(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE template_file) {
  CREATEFILE2_EXTENDED_PARAMETERS parameters{};
  parameters.dwSize = sizeof(parameters);
  parameters.dwFileAttributes = flags & 0xffff;
  if (!parameters.dwFileAttributes) parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  parameters.dwFileFlags = flags & 0xfff00000;
  parameters.dwSecurityQosFlags = flags & 0x000f0000;
  parameters.lpSecurityAttributes = security;
  parameters.hTemplateFile = template_file;
  return CreateFile2FromAppW(path, access, share, disposition, &parameters);
}
inline DWORD ProsperityProcessorCount(WORD) {
  SYSTEM_INFO information{};
  GetNativeSystemInfo(&information);
  return information.dwNumberOfProcessors ? information.dwNumberOfProcessors : 1;
}
#define CreateFileW ProsperityCreateFileW
#define GetActiveProcessorCount ProsperityProcessorCount

inline std::wstring ProsperityWide(const char* text, int length = -1, UINT code_page = CP_ACP) {
  if (!text) return {};
  const int size = MultiByteToWideChar(code_page, 0, text, length, nullptr, 0);
  if (size <= 0) return {};
  std::wstring value(size, 0);
  MultiByteToWideChar(code_page, 0, text, length, value.data(), size);
  if (length == -1) value.resize(size - 1);
  return value;
}
inline HANDLE ProsperityCreateFileA(LPCSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE template_file) {
  return ProsperityCreateFileW(ProsperityWide(path).c_str(), access, share, security, disposition, flags, template_file);
}
inline BOOL ProsperityOemToCharBuffA(LPCSTR input, LPSTR output, DWORD size) {
  const auto text = ProsperityWide(input, static_cast<int>(size), CP_OEMCP);
  return WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), output,
      static_cast<int>(size), nullptr, nullptr) > 0;
}
inline BOOL ProsperityOemToCharA(LPCSTR input, LPSTR output) {
  return ProsperityOemToCharBuffA(input, output, static_cast<DWORD>(strlen(input) + 1));
}
inline BOOL ProsperityCharToOemA(LPCSTR input, LPSTR output) {
  const auto text = ProsperityWide(input);
  return WideCharToMultiByte(CP_OEMCP, 0, text.c_str(), -1, output,
      static_cast<int>(strlen(input) + 1), nullptr, nullptr) > 0;
}
inline BOOL ProsperityCharToOemBuffW(LPCWSTR input, LPSTR output, DWORD size) {
  return WideCharToMultiByte(CP_OEMCP, 0, input, static_cast<int>(size), output,
      static_cast<int>(size), nullptr, nullptr) > 0;
}
inline int ProsperityCompareStringA(LCID, DWORD flags, LPCSTR first, int first_size,
    LPCSTR second, int second_size) {
  const auto a = ProsperityWide(first, first_size);
  const auto b = ProsperityWide(second, second_size);
  return CompareStringEx(LOCALE_NAME_USER_DEFAULT, flags, a.data(), static_cast<int>(a.size()),
      b.data(), static_cast<int>(b.size()), nullptr, nullptr, 0);
}
inline DWORD ProsperityGetFileAttributesW(LPCWSTR path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  return GetFileAttributesExFromAppW(path, GetFileExInfoStandard, &data) ? data.dwFileAttributes : INVALID_FILE_ATTRIBUTES;
}
inline HANDLE ProsperityFindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
  return FindFirstFileExFromAppW(path, FindExInfoBasic, data, FindExSearchNameMatch, nullptr, 0);
}
#define CreateFileA ProsperityCreateFileA
#define OemToCharBuffA ProsperityOemToCharBuffA
#define OemToCharA ProsperityOemToCharA
#define CharToOemA ProsperityCharToOemA
#define CharToOemBuffW ProsperityCharToOemBuffW
#define CompareStringA ProsperityCompareStringA
#define MoveFileW MoveFileFromAppW
#define CreateDirectoryW CreateDirectoryFromAppW
#define DeleteFileW DeleteFileFromAppW
#define RemoveDirectoryW RemoveDirectoryFromAppW
#define GetFileAttributesW ProsperityGetFileAttributesW
#define SetFileAttributesW SetFileAttributesFromAppW
#define FindFirstFileW ProsperityFindFirstFileW
#undef MoveFile
#define MoveFile MoveFileFromAppW
inline int ProsperityCompareStringW(LCID locale, DWORD flags, LPCWSTR first, int first_size,
    LPCWSTR second, int second_size) {
  wchar_t name[LOCALE_NAME_MAX_LENGTH];
  if (!LCIDToLocaleName(locale, name, LOCALE_NAME_MAX_LENGTH, 0)) return 0;
  return CompareStringEx(name, flags, first, first_size, second, second_size, nullptr, nullptr, 0);
}
inline LPWSTR ProsperityCaseString(LPWSTR text, DWORD flags) {
  const auto value = reinterpret_cast<uintptr_t>(text);
  if (value <= 0xffff) {
    const wchar_t input = static_cast<wchar_t>(value);
    wchar_t output = input;
    if (!LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, &input, 1, &output, 1, nullptr, nullptr, 0)) return text;
    return reinterpret_cast<LPWSTR>(static_cast<uintptr_t>(output));
  }
  const int count = static_cast<int>(std::wcslen(text));
  if (!count) return text;
  const std::wstring input(text, count);
  LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, input.data(), count, text, count, nullptr, nullptr, 0);
  return text;
}
#define CompareStringW ProsperityCompareStringW
#define CharLower(text) ProsperityCaseString(text, LCMAP_LOWERCASE)
#define CharUpper(text) ProsperityCaseString(text, LCMAP_UPPERCASE)

#if defined(PROSPERITY_CORE)
#undef near
#undef far
#include "uwp/posix_runtime.h"
#include "uwp/memory_win.h"
#define VirtualAlloc ProsperityVirtualAlloc
#define VirtualProtect ProsperityVirtualProtect
#define VirtualFree ProsperityVirtualFree
#endif
