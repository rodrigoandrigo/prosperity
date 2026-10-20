#include "io/storage.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace io {
namespace {
std::string Normalize(const char* path) {
  if (!path || !*path) throw std::invalid_argument("empty storage path");
  std::string input(path);
  std::replace(input.begin(), input.end(), '\\', '/');
  if (input.starts_with("//?/")) input.erase(0, 4);
  std::string result;
  std::vector<size_t> components;
  for (size_t start = 0; start < input.size();) {
    auto end = input.find('/', start);
    if (end == std::string::npos) end = input.size();
    const auto component = input.substr(start, end - start);
    if (component == "..") {
      if (components.empty()) throw std::invalid_argument("storage path escapes its root");
      result.resize(components.back()); components.pop_back();
    } else if (!component.empty() && component != ".") {
      const auto previous = result.size();
      if (!result.empty() || input.front() == '/') result += '/';
      result += component; components.push_back(previous);
    }
    start = end + 1;
  }
  if (result.empty()) result = "/";
  // Windows path comparisons are insensitive to ASCII case. Preserve UTF-8
  // spelling of each relative component for the provider itself.
  return result;
}
bool Prefix(const std::string& path, const std::string& root) {
  if (path.size() < root.size()) return false;
  for (size_t i = 0; i < root.size(); ++i) {
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
    if (lower(path[i]) != lower(root[i])) return false;
  }
  return path.size() == root.size() || root == "/" || path[root.size()] == '/';
}
struct Mount {
  std::string root;
  prosperity_storage api{};
  bool retained = false;
  ~Mount() { if (retained) { try { api.release(api.user); } catch (...) {} } }
};
struct Registry {
  std::mutex mutex;
  std::vector<std::shared_ptr<Mount>> mounts;
};
Registry& GetRegistry() { static Registry registry; return registry; }
std::shared_ptr<Mount> Find(const char* path, std::string& relative) {
  auto normalized = Normalize(path);
  std::shared_ptr<Mount> best;
  auto& registry = GetRegistry();
  {
    std::lock_guard lock(registry.mutex);
    for (auto& mount : registry.mounts)
      if (Prefix(normalized, mount->root) && (!best || mount->root.size() > best->root.size())) best = mount;
  }
  if (best) {
    relative = normalized.substr(best->root.size());
    if (!relative.empty() && relative.front() == '/') relative.erase(0, 1);
  }
  return best;
}
bool ValidStream(const prosperity_io_stream& stream) {
  return stream.size == sizeof(stream) && stream.retain && stream.release && stream.read_at && stream.get_size;
}
void CloseOpened(prosperity_opened_file& file) noexcept {
#if defined(_WIN32)
  if (file.native_handle && file.native_handle != INVALID_HANDLE_VALUE) CloseHandle(file.native_handle);
#endif
  if (file.stream.release) { try { file.stream.release(file.stream.user); } catch (...) {} }
  file = {};
}
int32_t NativeError() {
#if defined(_WIN32)
  switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return PROSPERITY_IO_NOT_FOUND;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return PROSPERITY_IO_DENIED;
    case ERROR_HANDLE_EOF: return PROSPERITY_IO_END;
    case ERROR_OPERATION_ABORTED: return PROSPERITY_IO_CANCELLED;
  }
#endif
  return PROSPERITY_IO_FAILED;
}
class BrokerFile final : public FileBase {
 public:
  BrokerFile(std::shared_ptr<Mount> mount, prosperity_opened_file opened, uint32_t flags)
      : mount_(std::move(mount)), opened_(opened), flags_(flags) {
    if (flags & PROSPERITY_IO_APPEND) position_ = GetSize();
  }
  ~BrokerFile() override { CloseOpened(opened_); }
  bool IsOpen() override { return opened_.native_handle || opened_.stream.read_at; }
  uint64_t Read(void* buffer, size_t bytes) override { return Transfer(buffer, bytes, false); }
  uint64_t Write(const void* buffer, size_t bytes) override { return Transfer(const_cast<void*>(buffer), bytes, true); }
  uint64_t Seek(int64_t offset, SeekMode mode) override {
    error_ = 0;
    const uint64_t base = mode == SeekMode::kSeekCur ? position_ : mode == SeekMode::kSeekEnd ? GetSize() : 0;
    if (error_ || base > INT64_MAX || (offset < 0 && uint64_t(-(offset + 1)) + 1 > base) ||
        (offset > 0 && uint64_t(offset) > uint64_t(INT64_MAX) - base)) {
      error_ = PROSPERITY_IO_INVALID; return UINT64_MAX;
    }
    position_ = uint64_t(int64_t(base) + offset); return position_;
  }
  uint64_t Tell() override { return position_; }
  uint64_t GetSize() override {
    error_ = 0; uint64_t size = 0;
#if defined(_WIN32)
    if (opened_.native_handle) {
      LARGE_INTEGER value{};
      if (!GetFileSizeEx(opened_.native_handle, &value)) error_ = NativeError();
      else size = uint64_t(value.QuadPart);
      return size;
    }
#endif
    error_ = opened_.stream.get_size(opened_.stream.user, &size); return size;
  }
  void Flush() override {
    error_ = 0;
#if defined(_WIN32)
    if (opened_.native_handle) {
      if ((flags_ & PROSPERITY_IO_WRITE) && !FlushFileBuffers(opened_.native_handle)) error_ = NativeError();
      return;
    }
#endif
    if (opened_.stream.flush) error_ = opened_.stream.flush(opened_.stream.user);
  }
  // NativeHandle historically exposes FILE* to savedata code. A brokered
  // source must never be miscast to FILE*; it is consumed through FileBase.
  NativeHandle GetNativeHandle() override { return nullptr; }
  int32_t LastError() const override { return error_ < 0 ? error_ : 0; }
 private:
  uint64_t Transfer(void* buffer, uint64_t bytes, bool write) {
    error_ = 0;
    if ((bytes && !buffer) || bytes > uint64_t(SIZE_MAX) || position_ > INT64_MAX || bytes > uint64_t(INT64_MAX) - position_) {
      error_ = PROSPERITY_IO_INVALID; return 0;
    }
    if (!(flags_ & (write ? PROSPERITY_IO_WRITE : PROSPERITY_IO_READ))) {
      error_ = PROSPERITY_IO_DENIED; return 0;
    }
    uint64_t total = 0;
    while (total < bytes) {
      uint64_t transferred = 0;
#if defined(_WIN32)
      if (opened_.native_handle) {
        // Explicit offsets avoid the shared seek pointer of DuplicateHandle.
        // Each operation owns its event, supporting synchronous and overlapped
        // handles without serializing independent files or caller threads.
        OVERLAPPED operation{}; const auto offset = position_ + total;
        operation.Offset = DWORD(offset); operation.OffsetHigh = DWORD(offset >> 32);
        operation.hEvent = CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
        if (!operation.hEvent) { error_ = NativeError(); break; }
        DWORD count = 0; const DWORD requested = DWORD(std::min<uint64_t>(bytes - total, 0x7ffff000u));
        auto* data = static_cast<unsigned char*>(buffer) + size_t(total);
        BOOL ok = write ? WriteFile(opened_.native_handle, data, requested, &count, &operation)
                        : ReadFile(opened_.native_handle, data, requested, &count, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(opened_.native_handle, &operation, &count, TRUE);
        const auto result = ok ? PROSPERITY_IO_OK : NativeError();
        CloseHandle(operation.hEvent); error_ = result;
        transferred = count;
      } else
#endif
      {
        auto& stream = opened_.stream;
        if (write && !stream.write_at) { error_ = PROSPERITY_IO_DENIED; break; }
        error_ = write ? stream.write_at(stream.user, position_ + total, static_cast<unsigned char*>(buffer) + size_t(total), bytes - total, &transferred)
                       : stream.read_at(stream.user, position_ + total, static_cast<unsigned char*>(buffer) + size_t(total), bytes - total, &transferred);
        if (transferred > bytes - total) { error_ = PROSPERITY_IO_FAILED; break; }
      }
      total += transferred;
      if (error_ != PROSPERITY_IO_OK || !transferred) break;
    }
    position_ += total; return total;
  }
  std::shared_ptr<Mount> mount_;
  prosperity_opened_file opened_{};
  uint32_t flags_;
  uint64_t position_ = 0;
  int32_t error_ = 0;
};
uint32_t Flags(FileMode mode) {
  switch (mode) {
    case FileMode::kRead: return PROSPERITY_IO_READ;
    case FileMode::kReadWrite: return PROSPERITY_IO_READ | PROSPERITY_IO_WRITE;
    case FileMode::kAppend: return PROSPERITY_IO_READ | PROSPERITY_IO_WRITE | PROSPERITY_IO_CREATE | PROSPERITY_IO_APPEND;
    default: return PROSPERITY_IO_READ | PROSPERITY_IO_WRITE | PROSPERITY_IO_CREATE | PROSPERITY_IO_TRUNCATE;
  }
}
class BrokerDirectory final : public StorageDirectory {
 public:
  BrokerDirectory(std::shared_ptr<Mount> mount, void* cursor) : mount_(std::move(mount)), cursor_(cursor) {}
  ~BrokerDirectory() override { try { mount_->api.list_close(cursor_); } catch (...) {} }
  int32_t Next(prosperity_directory_emit emit, void* user) override { return mount_->api.list_next(cursor_, emit, user); }
 private:
  std::shared_ptr<Mount> mount_; void* cursor_;
};
struct ImportedFile {
  std::atomic<uint32_t> refs{1};
  prosperity_opened_file opened{};
  ~ImportedFile() { CloseOpened(opened); }
};
void PROSPERITY_CALL RetainImported(void* user) { ++static_cast<ImportedFile*>(user)->refs; }
void PROSPERITY_CALL ReleaseImported(void* user) { auto* file = static_cast<ImportedFile*>(user); if (--file->refs == 0) delete file; }
int32_t PROSPERITY_CALL OpenImported(void* user, const char* relative, uint32_t flags, prosperity_opened_file* out) {
  if (*relative) return PROSPERITY_IO_NOT_FOUND;
  if (flags != PROSPERITY_IO_READ) return PROSPERITY_IO_DENIED;
  auto& source = static_cast<ImportedFile*>(user)->opened;
  out->size = sizeof(*out);
#if defined(_WIN32)
  if (source.native_handle) {
    return DuplicateHandle(GetCurrentProcess(), source.native_handle, GetCurrentProcess(), &out->native_handle,
        0, FALSE, DUPLICATE_SAME_ACCESS) ? PROSPERITY_IO_OK : NativeError();
  }
#endif
  out->stream = source.stream; out->stream.retain(out->stream.user); return PROSPERITY_IO_OK;
}
int32_t PROSPERITY_CALL StatImported(void* user, const char* relative, prosperity_file_info* info) {
  if (*relative) return PROSPERITY_IO_NOT_FOUND;
  auto& source = static_cast<ImportedFile*>(user)->opened;
  info->flags = PROSPERITY_IO_READ;
#if defined(_WIN32)
  if (source.native_handle) {
    LARGE_INTEGER size{}; if (!GetFileSizeEx(source.native_handle, &size)) return NativeError();
    info->file_size = uint64_t(size.QuadPart); return PROSPERITY_IO_OK;
  }
#endif
  return source.stream.get_size(source.stream.user, &info->file_size);
}
int32_t MountImported(const char* path, ImportedFile* file) {
  prosperity_storage api{}; api.size = sizeof(api); api.version = PROSPERITY_STORAGE_VERSION;
  api.user = file; api.retain = RetainImported; api.release = ReleaseImported; api.open = OpenImported; api.stat = StatImported;
  const auto result = MountStorage(path, api); ReleaseImported(file); return result;
}
}
int32_t MountStorage(const char* root, const prosperity_storage& api) {
  if (api.size != sizeof(api) || api.version != PROSPERITY_STORAGE_VERSION || !api.retain || !api.release || !api.open || !api.stat ||
      ((api.list_begin || api.list_next || api.list_close) && !(api.list_begin && api.list_next && api.list_close))) return PROSPERITY_IO_INVALID;
  try {
    auto mount = std::make_shared<Mount>(); mount->root = Normalize(root); mount->api = api;
    api.retain(api.user); mount->retained = true;
    auto& registry = GetRegistry(); std::shared_ptr<Mount> retired;
    {
      std::lock_guard lock(registry.mutex);
      for (auto& previous : registry.mounts) if (Prefix(previous->root, mount->root) && previous->root.size() == mount->root.size()) {
        retired = std::move(previous); previous = std::move(mount); return PROSPERITY_IO_OK;
      }
      registry.mounts.push_back(std::move(mount));
    }
    return PROSPERITY_IO_OK;
  } catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t MountStorageHandle(const char* path, void* handle) {
#if defined(_WIN32)
  if (!handle || handle == INVALID_HANDLE_VALUE) return PROSPERITY_IO_INVALID;
  auto file = std::make_unique<ImportedFile>(); file->opened.size = sizeof(file->opened);
  if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &file->opened.native_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) return NativeError();
  return MountImported(path, file.release());
#else
  return PROSPERITY_IO_UNSUPPORTED;
#endif
}
int32_t MountStorageStream(const char* path, const prosperity_io_stream& stream) {
  if (!ValidStream(stream)) return PROSPERITY_IO_INVALID;
  auto file = std::make_unique<ImportedFile>(); file->opened.size = sizeof(file->opened); file->opened.stream = stream;
  stream.retain(stream.user); return MountImported(path, file.release());
}
int32_t UnmountStorage(const char* root) {
  try {
    auto normalized = Normalize(root); auto& registry = GetRegistry(); std::shared_ptr<Mount> retired;
    {
      std::lock_guard lock(registry.mutex);
      auto it = std::find_if(registry.mounts.begin(), registry.mounts.end(), [&](const auto& mount) { return mount->root.size() == normalized.size() && Prefix(normalized, mount->root); });
      if (it == registry.mounts.end()) return PROSPERITY_IO_NOT_FOUND;
      retired = std::move(*it); registry.mounts.erase(it);
    }
    return PROSPERITY_IO_OK;
  } catch (...) { return PROSPERITY_IO_INVALID; }
}
void ClearStorage() {
  std::vector<std::shared_ptr<Mount>> retired; auto& registry = GetRegistry();
  { std::lock_guard lock(registry.mutex); retired.swap(registry.mounts); }
}
base::UniquePointer<FileBase> OpenStorage(const char* path, FileMode mode, bool& handled) {
  return OpenStorageFlags(path, Flags(mode), handled);
}
base::UniquePointer<FileBase> AdoptStorageHandle(void* handle, uint32_t flags) {
  prosperity_opened_file opened{}; opened.size = sizeof(opened); opened.native_handle = handle;
  try { return base::MakeUnique<BrokerFile>(nullptr, opened, flags); }
  catch (...) { CloseOpened(opened); throw; }
}
base::UniquePointer<FileBase> OpenStorageFlags(const char* path, uint32_t flags, bool& handled) {
  handled = false;
  try {
    std::string relative; auto mount = Find(path, relative); if (!mount) return {};
    handled = true; prosperity_opened_file opened{}; opened.size = sizeof(opened); opened.stream.size = sizeof(opened.stream);
    const auto result = mount->api.open(mount->api.user, relative.c_str(), flags, &opened);
    if (result != PROSPERITY_IO_OK) { CloseOpened(opened); errno = result == PROSPERITY_IO_NOT_FOUND ? ENOENT : EACCES; return {}; }
    if (opened.size != sizeof(opened) || opened.native_handle == reinterpret_cast<void*>(intptr_t(-1)) ||
        (opened.native_handle && opened.stream.release) || (!opened.native_handle && !ValidStream(opened.stream))) {
      CloseOpened(opened); errno = EIO; return {};
    }
    try { return base::MakeUnique<BrokerFile>(std::move(mount), opened, flags); }
    catch (...) { CloseOpened(opened); throw; }
  } catch (...) { handled = true; errno = EIO; return {}; }
}
int32_t StatStorage(const char* path, prosperity_file_info& info, bool& handled) {
  handled = false;
  try {
    std::string relative; auto mount = Find(path, relative); if (!mount) return PROSPERITY_IO_NOT_FOUND;
    handled = true; info = {}; info.size = sizeof(info); return (mount->api.stat)(mount->api.user, relative.c_str(), &info);
  } catch (...) { handled = true; return PROSPERITY_IO_FAILED; }
}
std::unique_ptr<StorageDirectory> ListStorage(const char* path, bool& handled, int32_t& result) {
  handled = false;
  try {
    std::string relative; auto mount = Find(path, relative); if (!mount) { result = PROSPERITY_IO_NOT_FOUND; return {}; }
    handled = true; if (!mount->api.list_begin) { result = PROSPERITY_IO_NOT_DIRECTORY; return {}; }
    void* cursor = nullptr; result = mount->api.list_begin(mount->api.user, relative.c_str(), &cursor);
    if (result != PROSPERITY_IO_OK) return {};
    try { return std::make_unique<BrokerDirectory>(std::move(mount), cursor); }
    catch (...) { mount->api.list_close(cursor); throw; }
  } catch (...) { handled = true; result = PROSPERITY_IO_FAILED; return {}; }
}
void* OpenStorageHandle(const char* path, uint32_t flags, bool& handled) {
  handled = false;
  try {
    std::string relative; auto mount = Find(path, relative); if (!mount) return nullptr;
    handled = true; prosperity_opened_file opened{}; opened.size = sizeof(opened); opened.stream.size = sizeof(opened.stream);
    const auto result = mount->api.open(mount->api.user, relative.c_str(), flags, &opened);
    if (result != PROSPERITY_IO_OK || !opened.native_handle) { CloseOpened(opened); return nullptr; }
    return opened.native_handle;
  } catch (...) { handled = true; return nullptr; }
}
}
