#include <windows.h>
#include <wrl.h>
#include <agile.h>
#include <collection.h>
#include "BrokeredStorage.h"
#include "StorageHandleInterop.h"
#include <windows.storage.streams.h>
#include <robuffer.h>
#include <roapi.h>
#include <ppltasks.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::Foundation::Collections;
using namespace concurrency;
using Microsoft::WRL::ComPtr;
using namespace ProsperityStorageInterop;
namespace {
std::string Utf8(Platform::String^ text) {
  if (!text || !text->Length()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text->Data(), text->Length(), nullptr, 0, nullptr, nullptr);
  std::string result(length, 0);
  if (length) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text->Data(), text->Length(), &result[0], length, nullptr, nullptr);
  return result;
}
Platform::String^ Wide(const char* text) {
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
  if (length <= 0) throw ref new Platform::InvalidArgumentException();
  std::wstring result(length, 0); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, &result[0], length);
  std::replace(result.begin(), result.end(), L'/', L'\\');
  return ref new Platform::String(result.c_str());
}
int32_t Error(HRESULT hr) {
  if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) return PROSPERITY_IO_DENIED;
  if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) return PROSPERITY_IO_NOT_FOUND;
  if (hr == E_NOTIMPL || hr == E_NOINTERFACE) return PROSPERITY_IO_UNSUPPORTED;
  if (hr == E_INVALIDARG) return PROSPERITY_IO_INVALID;
  if (hr == E_ABORT || hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)) return PROSPERITY_IO_CANCELLED;
  return PROSPERITY_IO_FAILED;
}
struct Apartment {
  HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);
  ~Apartment() { if (SUCCEEDED(result)) RoUninitialize(); }
  Apartment() {
    // A callback must not synchronously wait on a XAML/STA thread.
    if (FAILED(result)) throw ref new Platform::COMException(result);
  }
};
template<class Action> int32_t Guard(Action action) {
  try { Apartment apartment; return action(); }
  catch (Platform::Exception^ error) { return Error(error->HResult); }
  catch (...) { return PROSPERITY_IO_FAILED; }
}
// An IBuffer over caller-owned memory: stream fallback does not allocate or
// copy a second buffer for normal providers. The operation is awaited before
// returning, so the memory remains valid for the entire async read/write.
class CallerBuffer final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>,
    ABI::Windows::Storage::Streams::IBuffer, IBufferByteAccess, Microsoft::WRL::FtmBase> {
  InspectableClass(L"Prosperity.CallerBuffer", BaseTrust);
 public:
  CallerBuffer(BYTE* bytes, UINT32 capacity, UINT32 length) : bytes_(bytes), capacity_(capacity), length_(length) {}
  STDMETHODIMP get_Capacity(UINT32* value) override { if (!value) return E_POINTER; *value = capacity_; return S_OK; }
  STDMETHODIMP get_Length(UINT32* value) override { if (!value) return E_POINTER; *value = length_; return S_OK; }
  STDMETHODIMP put_Length(UINT32 value) override { if (value > capacity_) return E_INVALIDARG; length_ = value; return S_OK; }
  STDMETHODIMP Buffer(BYTE** value) override { if (!value) return E_POINTER; *value = bytes_; return S_OK; }
 private:
  BYTE* bytes_; UINT32 capacity_; std::atomic<UINT32> length_;
};
struct Stream {
  std::atomic<uint32_t> refs{1};
  Platform::Agile<IRandomAccessStream> stream;
  explicit Stream(IRandomAccessStream^ value) : stream(value) {}
};
void PROSPERITY_CALL RetainStream(void* user) { ++static_cast<Stream*>(user)->refs; }
void PROSPERITY_CALL ReleaseStream(void* user) { auto* value = static_cast<Stream*>(user); if (--value->refs == 0) delete value; }
int32_t PROSPERITY_CALL ReadStream(void* user, uint64_t offset, void* bytes, uint64_t size, uint64_t* count) {
  if (!count || (size && !bytes)) return PROSPERITY_IO_INVALID; *count = 0;
  return Guard([&]() -> int32_t {
    auto stream = static_cast<Stream*>(user)->stream.Get();
    if (!stream->CanRead) return PROSPERITY_IO_DENIED;
    const auto end = stream->Size;
    if (offset >= end) return PROSPERITY_IO_END;
    size = (std::min)(size, end - offset);
    while (*count < size) {
      const auto length = static_cast<UINT32>(std::min<uint64_t>(size - *count, UINT32_MAX));
      auto* destination = static_cast<BYTE*>(bytes) + static_cast<size_t>(*count);
      auto native = Microsoft::WRL::Make<CallerBuffer>(destination, length, 0);
      auto buffer = reinterpret_cast<IBuffer^>(native.Get());
      auto input = stream->GetInputStreamAt(offset + *count);
      auto result = create_task(input->ReadAsync(buffer, length, InputStreamOptions::None)).get();
      if (result->Length > length) return PROSPERITY_IO_FAILED;
      ComPtr<IBufferByteAccess> access;
      auto hr = reinterpret_cast<IInspectable*>(result)->QueryInterface(IID_PPV_ARGS(&access));
      if (FAILED(hr)) return Error(hr);
      BYTE* source = nullptr; if (FAILED(hr = access->Buffer(&source))) return Error(hr);
      if (source != destination) std::memcpy(destination, source, result->Length);
      *count += result->Length;
      if (!result->Length) break;
    }
    return *count ? PROSPERITY_IO_OK : PROSPERITY_IO_END;
  });
}
int32_t PROSPERITY_CALL WriteStream(void* user, uint64_t offset, const void* bytes, uint64_t size, uint64_t* count) {
  if (!count || (size && !bytes)) return PROSPERITY_IO_INVALID; *count = 0;
  return Guard([&]() -> int32_t {
    auto stream = static_cast<Stream*>(user)->stream.Get(); if (!stream->CanWrite) return PROSPERITY_IO_DENIED;
    while (*count < size) {
      const auto length = static_cast<UINT32>(std::min<uint64_t>(size - *count, UINT32_MAX));
      auto native = Microsoft::WRL::Make<CallerBuffer>(const_cast<BYTE*>(static_cast<const BYTE*>(bytes)) + size_t(*count), length, length);
      auto buffer = reinterpret_cast<IBuffer^>(native.Get());
      const auto written = create_task(stream->GetOutputStreamAt(offset + *count)->WriteAsync(buffer)).get();
      if (!written || written > length) return PROSPERITY_IO_FAILED; *count += written;
    }
    return PROSPERITY_IO_OK;
  });
}
int32_t PROSPERITY_CALL SizeStream(void* user, uint64_t* size) {
  if (!size) return PROSPERITY_IO_INVALID;
  return Guard([&]() -> int32_t { *size = static_cast<Stream*>(user)->stream.Get()->Size; return PROSPERITY_IO_OK; });
}
int32_t PROSPERITY_CALL FlushStream(void* user) {
  return Guard([&]() -> int32_t {
    auto stream = static_cast<Stream*>(user)->stream.Get();
    return !stream->CanWrite || create_task(stream->FlushAsync()).get() ? PROSPERITY_IO_OK : PROSPERITY_IO_FAILED;
  });
}
struct Broker {
  std::atomic<uint32_t> refs{1};
  Platform::Agile<StorageFolder> folder;
  Platform::Agile<StorageFile> file;
  bool directory;
  explicit Broker(IStorageItem^ item) : directory(item->IsOfType(StorageItemTypes::Folder)) {
    if (directory) folder = Platform::Agile<StorageFolder>(safe_cast<StorageFolder^>(item));
    else file = Platform::Agile<StorageFile>(safe_cast<StorageFile^>(item));
  }
};
void PROSPERITY_CALL RetainBroker(void* user) { ++static_cast<Broker*>(user)->refs; }
void PROSPERITY_CALL ReleaseBroker(void* user) { auto* value = static_cast<Broker*>(user); if (--value->refs == 0) delete value; }
std::vector<std::wstring> Components(const char* path) {
  const std::wstring input(Wide(path)->Data()); std::vector<std::wstring> result;
  for (size_t start = 0; start < input.size();) {
    const auto end = input.find(L'\\', start); auto name = input.substr(start, end == std::wstring::npos ? end : end - start);
    if (name.empty() || name == L".." || name == L"." || name.find(L':') != std::wstring::npos) throw ref new Platform::InvalidArgumentException();
    result.push_back(name); if (end == std::wstring::npos) break; start = end + 1;
  }
  return result;
}
StorageFolder^ Folder(Broker* broker, const std::vector<std::wstring>& parts, size_t count) {
  if (!broker->directory) throw ref new Platform::COMException(HRESULT_FROM_WIN32(ERROR_DIRECTORY));
  auto folder = broker->folder.Get();
  for (size_t i = 0; i < count; ++i) folder = create_task(folder->GetFolderAsync(ref new Platform::String(parts[i].c_str()))).get();
  return folder;
}
HRESULT NativeOpen(Broker* broker, const char* relative, uint32_t flags, HANDLE* handle) {
  *handle = nullptr;
  const auto access = static_cast<HANDLE_ACCESS_OPTIONS>((flags & PROSPERITY_IO_WRITE) ? HAO_READ | HAO_WRITE : HAO_READ);
  const auto sharing = static_cast<HANDLE_SHARING_OPTIONS>(HSO_SHARE_READ | HSO_SHARE_WRITE | HSO_SHARE_DELETE);
  const auto options = static_cast<HANDLE_OPTIONS>(HO_RANDOM_ACCESS | HO_OVERLAPPED);
  if (broker->directory) {
    auto item = broker->folder.Get(); ComPtr<IStorageFolderHandleAccess> native;
    auto hr = reinterpret_cast<IInspectable*>(item)->QueryInterface(IID_PPV_ARGS(&native)); if (FAILED(hr)) return hr;
    const auto creation = flags & PROSPERITY_IO_TRUNCATE ? (flags & PROSPERITY_IO_CREATE ? HCO_CREATE_ALWAYS : HCO_TRUNCATE_EXISTING) : flags & PROSPERITY_IO_CREATE ? HCO_OPEN_ALWAYS : HCO_OPEN_EXISTING;
    return native->Create(Wide(relative)->Data(), creation, access, sharing, options, nullptr, handle);
  }
  if (*relative) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  if (flags & (PROSPERITY_IO_CREATE | PROSPERITY_IO_TRUNCATE)) return E_NOTIMPL;
  auto item = broker->file.Get(); ComPtr<IStorageItemHandleAccess> native;
  auto hr = reinterpret_cast<IInspectable*>(item)->QueryInterface(IID_PPV_ARGS(&native)); if (FAILED(hr)) return hr;
  return native->Create(access, sharing, options, nullptr, handle);
}
int32_t PROSPERITY_CALL Open(void* user, const char* relative, uint32_t flags, prosperity_opened_file* output) {
  if (!relative || !output || output->size != sizeof(*output)) return PROSPERITY_IO_INVALID;
  return Guard([&]() -> int32_t {
    auto* broker = static_cast<Broker*>(user);
    const auto parts = Components(relative); // Reject traversal before the native fast path.
    HANDLE handle = nullptr;
    auto hr = NativeOpen(broker, relative, flags, &handle);
    if (SUCCEEDED(hr)) { output->native_handle = handle; return PROSPERITY_IO_OK; }
    StorageFile^ file;
    if (!broker->directory) { if (*relative) return PROSPERITY_IO_NOT_FOUND; file = broker->file.Get(); }
    else {
      if (parts.empty()) return PROSPERITY_IO_DENIED;
      auto parent = Folder(broker, parts, parts.size() - 1);
      auto name = ref new Platform::String(parts.back().c_str());
      if (flags & PROSPERITY_IO_CREATE) file = create_task(parent->CreateFileAsync(name,
          flags & PROSPERITY_IO_TRUNCATE ? CreationCollisionOption::ReplaceExisting : CreationCollisionOption::OpenIfExists)).get();
      else file = create_task(parent->GetFileAsync(name)).get();
    }
    auto stream = create_task(file->OpenAsync((flags & PROSPERITY_IO_WRITE) ? FileAccessMode::ReadWrite : FileAccessMode::Read)).get();
    if (flags & PROSPERITY_IO_TRUNCATE) stream->Size = 0;
    auto* state = new Stream(stream);
    auto& api = output->stream; api.size = sizeof(api); api.user = state;
    api.retain = RetainStream; api.release = ReleaseStream; api.read_at = ReadStream;
    api.write_at = WriteStream; api.get_size = SizeStream; api.flush = FlushStream;
    return PROSPERITY_IO_OK;
  });
}
void Info(IStorageItem^ item, prosperity_file_info* info) {
  info->flags = PROSPERITY_IO_READ;
  if (item->IsOfType(StorageItemTypes::Folder)) { info->flags |= PROSPERITY_IO_DIRECTORY; info->file_size = 0; }
  auto properties = create_task(item->GetBasicPropertiesAsync()).get();
  if (!(info->flags & PROSPERITY_IO_DIRECTORY)) info->file_size = properties->Size;
  info->modified_unix_seconds = properties->DateModified.UniversalTime / 10000000ll - 11644473600ll;
}
int32_t PROSPERITY_CALL Stat(void* user, const char* relative, prosperity_file_info* info) {
  if (!relative || !info || info->size != sizeof(*info)) return PROSPERITY_IO_INVALID;
  return Guard([&]() -> int32_t {
    auto* broker = static_cast<Broker*>(user); const auto parts = Components(relative);
    if (parts.empty()) {
      IStorageItem^ item = broker->directory ? static_cast<IStorageItem^>(broker->folder.Get()) : static_cast<IStorageItem^>(broker->file.Get());
      Info(item, info); return PROSPERITY_IO_OK;
    }
    if (!broker->directory) return PROSPERITY_IO_NOT_FOUND;
    HANDLE handle = nullptr;
    if (SUCCEEDED(NativeOpen(broker, relative, PROSPERITY_IO_READ, &handle))) {
      FILE_STANDARD_INFO standard{}; FILE_BASIC_INFO basic{};
      const bool ok = GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) &&
          GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic));
      CloseHandle(handle);
      if (ok) {
        info->flags = PROSPERITY_IO_READ | (standard.Directory ? PROSPERITY_IO_DIRECTORY : 0);
        info->file_size = standard.EndOfFile.QuadPart;
        info->modified_unix_seconds = basic.LastWriteTime.QuadPart / 10000000ll - 11644473600ll;
        return PROSPERITY_IO_OK;
      }
    }
    auto parent = Folder(broker, parts, parts.size() - 1);
    Info(create_task(parent->GetItemAsync(ref new Platform::String(parts.back().c_str()))).get(), info);
    return PROSPERITY_IO_OK;
  });
}
struct Cursor {
  Platform::Agile<StorageFolder> folder;
  Platform::Agile<IVectorView<IStorageItem^>> page;
  uint32_t offset = 0, index = 0;
  bool ended = false;
  explicit Cursor(StorageFolder^ value) : folder(value) {}
};
int32_t PROSPERITY_CALL Begin(void* user, const char* relative, void** out) {
  if (!relative || !out) return PROSPERITY_IO_INVALID; *out = nullptr;
  return Guard([&]() -> int32_t {
    auto* broker = static_cast<Broker*>(user); if (!broker->directory) return PROSPERITY_IO_NOT_DIRECTORY;
    const auto parts = Components(relative); auto folder = Folder(broker, parts, parts.size());
    *out = new Cursor(folder); return PROSPERITY_IO_OK;
  });
}
int32_t PROSPERITY_CALL Next(void* user, prosperity_directory_emit emit, void* sink) {
  if (!user || !emit) return PROSPERITY_IO_INVALID;
  return Guard([&]() -> int32_t {
    auto* cursor = static_cast<Cursor*>(user); if (cursor->ended) return PROSPERITY_IO_END;
    auto page = cursor->page.Get();
    if (!page || cursor->index == page->Size) {
      page = create_task(cursor->folder.Get()->GetItemsAsync(cursor->offset, 128)).get(); cursor->index = 0;
      cursor->page = Platform::Agile<IVectorView<IStorageItem^>>(page);
      if (!page->Size) { cursor->ended = true; return PROSPERITY_IO_END; }
      if (page->Size > UINT32_MAX - cursor->offset) return PROSPERITY_IO_FAILED;
      cursor->offset += page->Size;
    }
    auto item = page->GetAt(cursor->index++); const auto name = Utf8(item->Name);
    prosperity_file_info info{}; info.size = sizeof(info); Info(item, &info);
    return emit(sink, name.c_str(), &info);
  });
}
void PROSPERITY_CALL CloseCursor(void* user) { delete static_cast<Cursor*>(user); }
}
namespace Prosperity_UWP {
int32_t ImportBrokeredStream(prosperity_context* context, decltype(&prosperity_import_file_stream) import,
    IRandomAccessStream^ stream, const char* path) {
  if (!context || !import || !stream || !path) return PROSPERITY_IO_INVALID;
  try {
    std::unique_ptr<Stream, decltype(&ReleaseStream)> owner(new Stream(stream), ReleaseStream);
    prosperity_io_stream api{}; api.size = sizeof(api); api.user = owner.get();
    api.retain = RetainStream; api.release = ReleaseStream; api.read_at = ReadStream;
    api.write_at = WriteStream; api.get_size = SizeStream; api.flush = FlushStream;
    return import(context, path, &api);
  } catch (Platform::Exception^ error) { return Error(error->HResult); }
  catch (...) { return PROSPERITY_IO_FAILED; }
}
int32_t MountBrokeredStorage(prosperity_context* context, decltype(&prosperity_mount_storage) mount,
    IStorageItem^ item, Platform::String^ token, std::string& path) {
  if (!context || !mount || !item || !token) return PROSPERITY_IO_INVALID;
  try {
    std::unique_ptr<Broker, decltype(&ReleaseBroker)> brokerOwner(new Broker(item), ReleaseBroker); auto* broker = brokerOwner.get();
    path = Utf8(item->Path);
    if (path.empty()) path = Utf8(ApplicationData::Current->LocalFolder->Path) + "/broker/" + Utf8(token) + "/" + Utf8(item->Name);
    prosperity_storage api{}; api.size = sizeof(api); api.version = PROSPERITY_STORAGE_VERSION;
    api.user = broker; api.retain = RetainBroker; api.release = ReleaseBroker;
    api.open = Open; api.stat = Stat;
    if (broker->directory) { api.list_begin = Begin; api.list_next = Next; api.list_close = CloseCursor; }
    const auto result = mount(context, path.c_str(), &api); return result;
  } catch (Platform::Exception^ error) { return Error(error->HResult); }
  catch (...) { return PROSPERITY_IO_FAILED; }
}
}
