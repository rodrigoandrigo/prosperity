#pragma once
#include <unknwn.h>
// APP-partition interfaces from Windows SDK windowsstoragecom.h. Keep this
// minimal declaration isolated: SDK 26100 imports desktop shell proxy
// declarations with missing types when that header is used in a UWP target.
// No desktop API or partition override is needed for these COM interfaces.
namespace ProsperityStorageInterop {
enum HANDLE_OPTIONS : unsigned { HO_RANDOM_ACCESS = 0x10000000, HO_OVERLAPPED = 0x40000000 };
enum HANDLE_ACCESS_OPTIONS { HAO_READ = 0x120089, HAO_WRITE = 0x120116 };
enum HANDLE_SHARING_OPTIONS { HSO_SHARE_READ = 1, HSO_SHARE_WRITE = 2, HSO_SHARE_DELETE = 4 };
enum HANDLE_CREATION_OPTIONS { HCO_CREATE_ALWAYS = 2, HCO_OPEN_EXISTING = 3, HCO_OPEN_ALWAYS = 4, HCO_TRUNCATE_EXISTING = 5 };
struct IOplockBreakingHandler;
MIDL_INTERFACE("5CA296B2-2C25-4D22-B785-B885C8201E6A")
IStorageItemHandleAccess : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Create(HANDLE_ACCESS_OPTIONS, HANDLE_SHARING_OPTIONS,
      HANDLE_OPTIONS, IOplockBreakingHandler*, HANDLE*) = 0;
};
MIDL_INTERFACE("DF19938F-5462-48A0-BE65-D2A3271A08D6")
IStorageFolderHandleAccess : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Create(LPCWSTR, HANDLE_CREATION_OPTIONS, HANDLE_ACCESS_OPTIONS,
      HANDLE_SHARING_OPTIONS, HANDLE_OPTIONS, IOplockBreakingHandler*, HANDLE*) = 0;
};
}
