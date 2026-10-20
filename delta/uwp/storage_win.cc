#include "uwp/storage_bridge.h"
#include "io/storage.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#undef stat

namespace {
int Errno(int32_t result) {
  switch (result) {
    case PROSPERITY_IO_NOT_FOUND: return ENOENT;
    case PROSPERITY_IO_DENIED: return EACCES;
    case PROSPERITY_IO_NOT_DIRECTORY: return ENOTDIR;
    case PROSPERITY_IO_INVALID: return EINVAL;
    case PROSPERITY_IO_UNSUPPORTED: return ENOTSUP;
    default: return EIO;
  }
}
int32_t Status() {
  return errno == ENOENT ? PROSPERITY_IO_NOT_FOUND : errno == EACCES ? PROSPERITY_IO_DENIED : PROSPERITY_IO_FAILED;
}
std::wstring Wide(const char* path) { return ProsperityWide(path, -1, CP_UTF8); }
int64_t UnixTime(FILETIME value) {
  ULARGE_INTEGER ticks{}; ticks.LowPart = value.dwLowDateTime; ticks.HighPart = value.dwHighDateTime;
  return int64_t(ticks.QuadPart / 10000000ull) - 11644473600ll;
}
int32_t NativeStat(const char* path, prosperity_file_info* info) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExFromAppW(Wide(path).c_str(), GetFileExInfoStandard, &data)) {
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? PROSPERITY_IO_NOT_FOUND :
        error == ERROR_ACCESS_DENIED ? PROSPERITY_IO_DENIED : PROSPERITY_IO_FAILED;
  }
  info->flags = PROSPERITY_IO_READ | ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? PROSPERITY_IO_DIRECTORY : 0);
  info->file_size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  info->modified_unix_seconds = UnixTime(data.ftLastWriteTime); return PROSPERITY_IO_OK;
}
int32_t EmitNative(const WIN32_FIND_DATAW& data, prosperity_directory_emit emit, void* user) {
  const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data.cFileName, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return PROSPERITY_IO_FAILED;
  std::string name(size_t(bytes), 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data.cFileName, -1, name.data(), bytes, nullptr, nullptr);
  prosperity_file_info info{}; info.size = sizeof(info);
  info.flags = PROSPERITY_IO_READ | ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? PROSPERITY_IO_DIRECTORY : 0);
  info.file_size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  info.modified_unix_seconds = UnixTime(data.ftLastWriteTime);
  return emit(user, name.c_str(), &info);
}
}
int prosperity_stat(const char* path, struct stat* output) {
  if (!path || !output) { errno = EINVAL; return -1; }
  bool handled = false; prosperity_file_info info{}; info.size = sizeof(info);
  const auto result = io::StatStorage(path, info, handled);
  if (!handled) return ::stat(path, output);
  if (result != PROSPERITY_IO_OK) { errno = Errno(result); return -1; }
  std::memset(output, 0, sizeof(*output));
  output->st_mode = (info.flags & PROSPERITY_IO_DIRECTORY) ? S_IFDIR | 0555 : S_IFREG | 0444;
  output->st_size = info.file_size; output->st_mtime = info.modified_unix_seconds;
  output->st_nlink = 1; return 0;
}
struct prosperity_DIR {
  std::unique_ptr<io::StorageDirectory> broker;
  HANDLE native = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAW data{};
  bool first = true, ended = false;
  dirent entry{};
  ~prosperity_DIR() { if (native != INVALID_HANDLE_VALUE) FindClose(native); }
};
extern "C" DIR* opendir(const char* path) {
  if (!path) { errno = EINVAL; return nullptr; }
  try {
    auto result = std::make_unique<prosperity_DIR>(); bool handled = false; int32_t status = 0;
    result->broker = io::ListStorage(path, handled, status);
    if (handled) {
      if (!result->broker) { errno = Errno(status); return nullptr; }
      return result.release();
    }
    auto pattern = Wide(path); if (pattern.empty()) { errno = EINVAL; return nullptr; }
    if (pattern.back() != L'/' && pattern.back() != L'\\') pattern += L'\\'; pattern += L'*';
    result->native = FindFirstFileExFromAppW(pattern.c_str(), FindExInfoBasic, &result->data, FindExSearchNameMatch, nullptr, 0);
    if (result->native == INVALID_HANDLE_VALUE) { errno = ENOENT; return nullptr; }
    return result.release();
  } catch (...) { errno = EIO; return nullptr; }
}
extern "C" struct dirent* readdir(DIR* directory) {
  if (!directory || directory->ended) { if (!directory) errno = EINVAL; return nullptr; }
  try {
    directory->entry = {};
    auto emit = [](void* user, const char* name, const prosperity_file_info* info) -> int32_t {
      auto* result = static_cast<dirent*>(user);
      if (!name || !info || strlen(name) >= sizeof(result->d_name) || strchr(name, '/') || strchr(name, '\\')) return PROSPERITY_IO_INVALID;
      std::memcpy(result->d_name, name, strlen(name) + 1);
      result->d_type = (info->flags & PROSPERITY_IO_DIRECTORY) ? DT_DIR : DT_REG; return PROSPERITY_IO_OK;
    };
    int32_t status = PROSPERITY_IO_END;
    if (directory->broker) status = directory->broker->Next(emit, &directory->entry);
    else {
      if (!directory->first && !FindNextFileW(directory->native, &directory->data)) {
        directory->ended = true; errno = GetLastError() == ERROR_NO_MORE_FILES ? 0 : EIO; return nullptr;
      }
      directory->first = false;
      status = EmitNative(directory->data, emit, &directory->entry);
    }
    if (status != PROSPERITY_IO_OK) { directory->ended = true; errno = status == PROSPERITY_IO_END ? 0 : Errno(status); return nullptr; }
    return &directory->entry;
  } catch (...) { errno = EIO; return nullptr; }
}
extern "C" int closedir(DIR* directory) { if (!directory) { errno = EINVAL; return -1; } delete directory; return 0; }

struct prosperity_file {
  std::mutex mutex;
  io::File file;
};
int32_t PROSPERITY_CALL prosperity_mount_storage(prosperity_context* context, const char* root, const prosperity_storage* storage) {
  if (!uwp::IsActiveContext(context) || !root || !storage) return PROSPERITY_IO_INVALID;
  try { return io::MountStorage(root, *storage); } catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t PROSPERITY_CALL prosperity_import_file_handle(prosperity_context* context, const char* path, void* handle) {
  if (!uwp::IsActiveContext(context) || !path) return PROSPERITY_IO_INVALID;
  try { return io::MountStorageHandle(path, handle); } catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t PROSPERITY_CALL prosperity_import_file_stream(prosperity_context* context, const char* path, const prosperity_io_stream* stream) {
  if (!uwp::IsActiveContext(context) || !path || !stream) return PROSPERITY_IO_INVALID;
  try { return io::MountStorageStream(path, *stream); } catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t PROSPERITY_CALL prosperity_unmount_storage(prosperity_context* context, const char* root) {
  if (!uwp::IsActiveContext(context) || !root) return PROSPERITY_IO_INVALID;
  return io::UnmountStorage(root);
}
int32_t PROSPERITY_CALL prosperity_file_open(prosperity_context* context, const char* path, uint32_t flags, prosperity_file** out) {
  if (out) *out = nullptr;
  if (!uwp::IsActiveContext(context) || !path || !out || !(flags & (PROSPERITY_IO_READ | PROSPERITY_IO_WRITE)) ||
      ((flags & (PROSPERITY_IO_CREATE | PROSPERITY_IO_TRUNCATE | PROSPERITY_IO_APPEND)) && !(flags & PROSPERITY_IO_WRITE)) ||
      (flags & ~(PROSPERITY_IO_READ | PROSPERITY_IO_WRITE | PROSPERITY_IO_CREATE | PROSPERITY_IO_TRUNCATE | PROSPERITY_IO_APPEND))) return PROSPERITY_IO_INVALID;
  try {
    auto file = std::make_unique<prosperity_file>();
    bool handled = false;
    file->file.Reset(io::OpenStorageFlags(path, flags, handled));
    if (!handled) {
      const DWORD access = ((flags & PROSPERITY_IO_READ) ? GENERIC_READ : 0) | ((flags & PROSPERITY_IO_WRITE) ? GENERIC_WRITE : 0);
      const DWORD creation = flags & PROSPERITY_IO_TRUNCATE ? (flags & PROSPERITY_IO_CREATE ? CREATE_ALWAYS : TRUNCATE_EXISTING) :
          flags & PROSPERITY_IO_CREATE ? OPEN_ALWAYS : OPEN_EXISTING;
      auto handle = ProsperityCreateFileW(Wide(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          nullptr, creation, FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? PROSPERITY_IO_NOT_FOUND :
            error == ERROR_ACCESS_DENIED ? PROSPERITY_IO_DENIED : PROSPERITY_IO_FAILED;
      }
      file->file.Reset(io::AdoptStorageHandle(handle, flags));
    }
    if (!file->file.IsOpen()) return Status();
    *out = file.release(); return PROSPERITY_IO_OK;
  } catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t PROSPERITY_CALL prosperity_file_read_at(prosperity_file* file, uint64_t offset, void* buffer, uint64_t size, uint64_t* count) {
  if (count) *count = 0;
  if (!file || !count || (size && !buffer) || offset > INT64_MAX || size > SIZE_MAX || size > uint64_t(INT64_MAX) - offset) return PROSPERITY_IO_INVALID;
  try {
    std::lock_guard lock(file->mutex);
    if (file->file.Seek(offset, io::SeekMode::kSeekSet) != offset) return PROSPERITY_IO_FAILED;
    *count = file->file.Read(buffer, size);
    const auto error = file->file.LastError();
    return error < 0 ? error : size && !*count ? PROSPERITY_IO_END : PROSPERITY_IO_OK;
  } catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t PROSPERITY_CALL prosperity_file_write_at(prosperity_file* file, uint64_t offset, const void* buffer, uint64_t size, uint64_t* count) {
  if (count) *count = 0;
  if (!file || !count || (size && !buffer) || offset > INT64_MAX || size > SIZE_MAX || size > uint64_t(INT64_MAX) - offset) return PROSPERITY_IO_INVALID;
  try {
    std::lock_guard lock(file->mutex);
    if (file->file.Seek(offset, io::SeekMode::kSeekSet) != offset) return PROSPERITY_IO_FAILED;
    *count = file->file.Write(buffer, size); return file->file.LastError();
  } catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t PROSPERITY_CALL prosperity_file_get_size(prosperity_file* file, uint64_t* size) {
  if (!file || !size) return PROSPERITY_IO_INVALID;
  try { std::lock_guard lock(file->mutex); *size = file->file.GetSize(); return file->file.LastError(); } catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t PROSPERITY_CALL prosperity_file_flush(prosperity_file* file) {
  if (!file) return PROSPERITY_IO_INVALID;
  try { std::lock_guard lock(file->mutex); file->file.Flush(); return file->file.LastError(); } catch (...) { return PROSPERITY_IO_FAILED; }
}
void PROSPERITY_CALL prosperity_file_close(prosperity_file* file) { delete file; }
int32_t PROSPERITY_CALL prosperity_storage_stat(prosperity_context* context, const char* path, prosperity_file_info* info) {
  if (!uwp::IsActiveContext(context) || !path || !info || info->size != sizeof(*info)) return PROSPERITY_IO_INVALID;
  try {
    bool handled = false; auto result = io::StatStorage(path, *info, handled);
    return handled ? result : NativeStat(path, info);
  } catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t PROSPERITY_CALL prosperity_storage_list(prosperity_context* context, const char* path, prosperity_directory_emit emit, void* user) {
  if (!uwp::IsActiveContext(context) || !path || !emit) return PROSPERITY_IO_INVALID;
  try {
    bool handled = false; int32_t status = 0; auto directory = io::ListStorage(path, handled, status);
    if (handled) {
      if (!directory) return status;
      while ((status = directory->Next(emit, user)) == PROSPERITY_IO_OK) {}
      return status == PROSPERITY_IO_END ? PROSPERITY_IO_OK : status;
    }
    std::unique_ptr<DIR, decltype(&closedir)> native(opendir(path), closedir);
    if (!native) return Status();
    errno = 0;
    for (;;) {
      errno = 0;
      auto* entry = readdir(native.get()); if (!entry) break;
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      const auto child = std::string(path) + "/" + entry->d_name;
      prosperity_file_info info{}; info.size = sizeof(info); status = NativeStat(child.c_str(), &info);
      if (status != PROSPERITY_IO_OK) return status;
      status = emit(user, entry->d_name, &info); if (status != PROSPERITY_IO_OK) return status;
    }
    return errno ? Status() : PROSPERITY_IO_OK;
  } catch (...) { return PROSPERITY_IO_FAILED; }
}
