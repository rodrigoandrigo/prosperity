#pragma once
#include "io/file.h"
#include "io/storage_api.h"
#include <memory>
#include <string>
namespace io {
// Process composition root installs mounts before reading titles. Open files
// and iterators retain their provider independently of registry replacement.
int32_t MountStorage(const char*, const prosperity_storage&);
int32_t MountStorageHandle(const char*, void* handle);
int32_t MountStorageStream(const char*, const prosperity_io_stream&);
int32_t UnmountStorage(const char*);
void ClearStorage();
base::UniquePointer<FileBase> OpenStorage(const char*, FileMode, bool& handled);
base::UniquePointer<FileBase> OpenStorageFlags(const char*, uint32_t, bool& handled);
// Takes ownership of a successfully opened native handle.
base::UniquePointer<FileBase> AdoptStorageHandle(void*, uint32_t);
int32_t StatStorage(const char*, prosperity_file_info&, bool& handled);
class StorageDirectory {
 public:
  virtual ~StorageDirectory() = default;
  virtual int32_t Next(prosperity_directory_emit, void*) = 0;
};
std::unique_ptr<StorageDirectory> ListStorage(const char*, bool& handled, int32_t& result);
// Used only by Win32 consumers (e.g. UnRAR) which cannot consume FileBase.
// An authoritative stream-only mount returns UNSUPPORTED, never raw-path fallback.
void* OpenStorageHandle(const char*, uint32_t flags, bool& handled);
}
