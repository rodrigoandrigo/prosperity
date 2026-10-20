/*
 * UTL : The universal utility library
 *
 * Copyright 2019-2020 Force67.
 * For information regarding licensing see LICENSE
 * in the root of the source tree.
 */

#include "io/path.h"

#ifdef _WIN32
#include <Windows.h>
#include <cstring>
#include <cwchar>
#if defined(DELTA_UWP)
#include <mutex>
#include <string>
#include <stdexcept>
#endif
#else
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <cstring>
#include "options/options.h"

namespace {
DELTA_OPTION(const char*, kDataDir, "DELTA_DATA_DIR", nullptr);
}  // namespace
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#endif

namespace io {

#ifdef _WIN32
#if defined(DELTA_UWP)
namespace {
std::mutex g_directory_mutex;
base::String g_directory;
}
void SetDataDirectory(const char* directory) {
  if (!directory || !*directory) throw std::invalid_argument("Missing UWP data directory");
  std::lock_guard lock(g_directory_mutex);
  g_directory = directory;
  if (g_directory.back() != '/' && g_directory.back() != '\\') g_directory += "/";
}
base::String MakeAbsPath(const base::String& relative) {
  std::lock_guard lock(g_directory_mutex);
  if (g_directory.empty()) throw std::logic_error("UWP data directory has not been configured");
  base::String path = g_directory;
  path += relative;
  return path;
}
base::StringW MakeAbsPath(const base::StringW& relative) {
  const auto directory = MakeAbsPath(base::String());
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      directory.data(), static_cast<int>(directory.size()), nullptr, 0);
  if (!count) throw std::runtime_error("Invalid UTF-8 data directory");
  base::StringW path;
  path.resize(count);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, directory.data(),
      static_cast<int>(directory.size()), path.data(), count);
  path += relative;
  return path;
}
#else
base::StringW MakeAbsPath(const base::StringW& rel) {
  static base::StringW filePath;
  if (filePath.empty()) {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wchar_t* dirPtr = std::wcsrchr(buf, L'\\');
    if (dirPtr)
      dirPtr[1] = L'\0';
    filePath = base::StringW(buf);
  }

  base::StringW out = filePath;
  out += rel;

  // backslashes everywhere
  for (auto& c : out)
    if (c == L'/')
      c = L'\\';
  return out;
}

base::String MakeAbsPath(const base::String& rel) {
  static base::String filePath;
  if (filePath.empty()) {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    char* dirPtr = std::strrchr(buf, '\\');
    if (dirPtr)
      dirPtr[1] = '\0';
    filePath = base::String(buf);
  }

  base::String out = filePath;
  out += rel;

  for (auto& c : out)
    if (c == '/')
      c = '\\';
  return out;
}

#endif
#else  // POSIX

static const base::String& ExeDir() {
  static base::String file_path;
  if (file_path.empty()) {
    // DELTA_DATA_DIR overrides where modules/ and host assets live. The Android
    // app sets it to the activity's external files dir; the binary itself sits
    // in a read-only lib/ dir, so /proc/self/exe is no use there.
    if (const char* dd = kDataDir; dd && *dd) {
      file_path = base::String(dd);
      if (file_path.back() != '/')
        file_path += "/";
      return file_path;
    }
    char buf[PATH_MAX]{};
    ssize_t n = ::readlink("/proc/self/exe", buf, PATH_MAX - 1);
    if (n > 0) {
      buf[n] = '\0';
      char* slash = std::strrchr(buf, '/');
      if (slash)
        slash[1] = '\0';
      file_path = base::String(buf);
    } else {
      file_path = base::String("./");
    }
  }
  return file_path;
}

base::String MakeAbsPath(const base::String& rel) {
  base::String out = ExeDir();
  out += rel;
  return out;
}

base::StringW MakeAbsPath(const base::StringW& rel) {
  const auto& dir = ExeDir();
  base::StringW out;
  out.reserve(static_cast<base::StringW::size_type>(dir.size() + rel.size()));
  for (const char* p = dir.c_str(); *p; ++p)
    out.push_back(static_cast<wchar_t>(*p));
  out += rel;
  return out;
}

#endif

}  // namespace io
