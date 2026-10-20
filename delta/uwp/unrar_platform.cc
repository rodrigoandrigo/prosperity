#include "unrar/rar.hpp"
// The package's minimum OS is Windows 10. Use its conservative pathname rules
// on newer versions as well, without desktop WMI/version probing.
DWORD WinNT() { return WNT_W10; }
bool IsWindows11OrGreater() { return false; }
