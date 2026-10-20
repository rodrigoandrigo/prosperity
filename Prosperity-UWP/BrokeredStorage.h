#pragma once
#include "../delta/uwp/prosperity_api.h"
#include <string>
namespace Prosperity_UWP {
int32_t ImportBrokeredStream(prosperity_context*, decltype(&prosperity_import_file_stream),
    Windows::Storage::Streams::IRandomAccessStream^, const char* path);
// Registration only retains agile StorageItem references. Resolution and I/O
// occur when the DLL calls back from a worker, never synchronously on XAML.
int32_t MountBrokeredStorage(prosperity_context*, decltype(&prosperity_mount_storage),
    Windows::Storage::IStorageItem^, Platform::String^ token, std::string& mountedPath);
}
