// Desktop harness for the production C++/CX broker adapter and real DLL.
// No application installation, package launch or guest execution.
#include <windows.h>
#include <roapi.h>
#include <ppltasks.h>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <string>
#include <stdexcept>
#include "../../Prosperity-UWP/BrokeredStorage.h"
using namespace Windows::Storage;
using namespace concurrency;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Function> Function Symbol(HMODULE module, const char* name) {
  auto result = reinterpret_cast<Function>(GetProcAddress(module, name)); Check(result != nullptr, name); return result;
}
int wmain(int argc, wchar_t** argv) {
  try {
    Check(argc == 4, "runtime vclibs test-directory");
    Check(SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED)), "RoInitialize");
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(argv[1]); AddDllDirectory(argv[2]);
    auto module = LoadLibraryExW((std::wstring(argv[1]) + L"/prosperity.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Check(module != nullptr, "load DLL");
#define API(name) auto name = Symbol<decltype(&prosperity_##name)>(module, "prosperity_" #name)
    API(create); API(wait); API(destroy); API(mount_storage); API(unmount_storage);
    API(file_open); API(file_read_at); API(file_close); API(storage_list);
    API(import_file_stream);
#undef API
    std::wstring root(argv[3]);
    std::replace(root.begin(), root.end(), L'/', L'\\');
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"/sub").c_str(), nullptr);
    { std::ofstream out(root + L"/sub/dados.bin", std::ios::binary); out << "brokered-storage-data"; }
    for (int i = 0; i < 260; ++i) {
      std::ofstream out(root + L"/entry-" + std::to_wstring(i) + L".bin", std::ios::binary); out << i;
    }
    const int dataBytes = WideCharToMultiByte(CP_UTF8, 0, root.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string data(dataBytes, 0); WideCharToMultiByte(CP_UTF8, 0, root.c_str(), -1, data.data(), dataBytes, nullptr, nullptr);
    prosperity_config config{}; config.size = sizeof(config); config.abi_version = PROSPERITY_ABI_VERSION;
    config.data_directory = data.c_str();
    config.on_frame = [](void*, const void*, uint32_t, uint32_t, uint32_t, uint32_t) {};
    prosperity_context* context = nullptr;
    Check(create(&config, &context) == 0 && wait(context, 5000) == 0, "context");
    auto folder = create_task(StorageFolder::GetFolderFromPathAsync(ref new Platform::String(root.c_str()))).get();
    std::string mounted;
    Check(Prosperity_UWP::MountBrokeredStorage(context, mount_storage, folder, "test-folder", mounted) == 0, "mount actual StorageFolder");
    folder = nullptr; // The adapter/DLL must retain the object after this scope.
    prosperity_file* file = nullptr;
    Check(file_open(context, (mounted + "/sub/dados.bin").c_str(), PROSPERITY_IO_READ, &file) == 0, "nested broker open");
    char bytes[64]{}; uint64_t count = 0;
    Check(file_read_at(file, 0, bytes, sizeof(bytes), &count) == 0 && count == 21 && std::string(bytes, count) == "brokered-storage-data", "broker read");
    file_close(file);
    uint32_t entries = 0;
    auto emit = [](void* user, const char*, const prosperity_file_info*) -> int32_t { ++*static_cast<uint32_t*>(user); return 0; };
    Check(storage_list(context, mounted.c_str(), emit, &entries) == 0 && entries >= 261, "folder pagination");
    Check(unmount_storage(context, mounted.c_str()) == 0, "unmount folder");
    auto storageFile = create_task(StorageFile::GetFileFromPathAsync(ref new Platform::String((root + L"\\sub\\dados.bin").c_str()))).get();
    Check(Prosperity_UWP::MountBrokeredStorage(context, mount_storage, storageFile, "test-file", mounted) == 0, "mount actual StorageFile");
    auto stream = create_task(storageFile->OpenReadAsync()).get();
    Check(Prosperity_UWP::ImportBrokeredStream(context, import_file_stream, stream, "Z:/stream-only") == 0, "import actual IRandomAccessStream");
    stream = nullptr; storageFile = nullptr;
    Check(file_open(context, mounted.c_str(), PROSPERITY_IO_READ, &file) == 0, "broker file open");
    Check(unmount_storage(context, mounted.c_str()) == 0, "unmount open file");
    Check(file_read_at(file, 9, bytes, 7, &count) == 0 && count == 7 && std::string(bytes, 7) == "storage", "retained broker file");
    file_close(file);
    Check(file_open(context, "Z:/stream-only", PROSPERITY_IO_READ, &file) == 0, "WinRT stream open");
    Check(file_read_at(file, 0, bytes, 21, &count) == 0 && count == 21 && std::string(bytes, count) == "brokered-storage-data", "WinRT ReadAsync fallback");
    file_close(file);
    Check(destroy(context) == 0, "destroy");
    std::cout << "PASS actual StorageFolder/StorageFile/IRandomAccessStream, nested reads, " << entries << " entries, retained lifetime\n";
    RoUninitialize(); return 0;
  } catch (Platform::Exception^ error) {
    std::wcerr << L"WinRT failure " << std::hex << error->HResult << L" " << error->Message->Data() << L"\n"; return 1;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
