#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/uwp-platform-tests
clang++ -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -I delta -I build/uwp-deps/equilibrium \
  delta/cpu/mmio.cc tools/tests/uwp_mmio_test.cc \
  -o build/uwp-platform-tests/mmio-test.exe
./build/uwp-platform-tests/mmio-test.exe
# Compile against the App partition while keeping the test executable outside
# a package. Explicit CRT imports are needed by this MinGW installation.
clang++ -std=c++20 -fms-extensions -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -D_WIN32_WINNT=0x0A00 -DWINAPI_FAMILY=WINAPI_FAMILY_APP \
  -DDELTA_UWP=1 -DPROSPERITY_CORE=1 -include delta/uwp/winapi_compat.h -I build/uwp-deps/xbyak/xbyak -I delta/uwp/include -I delta delta/uwp/memory_win.cc tools/tests/uwp_memory_test.cc \
  -Wl,--undefined=__imp_GetLastError,--undefined=__imp_VirtualProtect,--undefined=__imp_EnterCriticalSection,--undefined=__imp_LeaveCriticalSection,--undefined=__imp_InitializeCriticalSection,--undefined=__imp_DeleteCriticalSection,--undefined=__imp_TlsGetValue \
  "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64/OneCore.lib" -lkernel32 -lwindowsapp -o build/uwp-platform-tests/memory-test.exe
./build/uwp-platform-tests/memory-test.exe
clang++ -std=c++20 -DDELTA_UWP=1 -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -D_WIN32_WINNT=0x0A00 -DWINAPI_FAMILY=WINAPI_FAMILY_APP \
  -I delta/uwp/include -I delta -I shared tools/tests/uwp_network_test.cc \
  -lws2_32 -o build/uwp-platform-tests/network-test.exe
./build/uwp-platform-tests/network-test.exe
clang++ -std=c++20 -fms-extensions -DDELTA_UWP=1 -DOS_WIN -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -D_WIN32_WINNT=0x0A00 -DWINAPI_FAMILY=WINAPI_FAMILY_APP \
  -I shared -I build/uwp-deps/equilibrium shared/guest/pause.cc tools/tests/uwp_pause_test.cc \
  build/uwp-Release/core/libeq_base.a \
  -Wl,--undefined=__imp_GetLastError,--undefined=__imp_VirtualProtect,--undefined=__imp_VirtualQuery,--undefined=__imp_EnterCriticalSection,--undefined=__imp_LeaveCriticalSection,--undefined=__imp_InitializeCriticalSection,--undefined=__imp_DeleteCriticalSection,--undefined=__imp_TlsGetValue \
  -lkernel32 -lwindowsapp -lsynchronization -o build/uwp-platform-tests/pause-test.exe
./build/uwp-platform-tests/pause-test.exe
clang++ -std=c++20 -fms-extensions -DDELTA_UWP=1 -DOS_WIN -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -D_WIN32_WINNT=0x0A00 -DWINAPI_FAMILY=WINAPI_FAMILY_APP \
  -I shared -I delta -I build/uwp-deps/equilibrium \
  delta/uwp/embedding_support.cc delta/uwp/check_win.cc delta/gpu/tests/uwp_embedding_failure_test.cc \
  build/uwp-Release/core/libeq_base.a \
  -Wl,--undefined=__imp_GetLastError,--undefined=__imp_VirtualProtect,--undefined=__imp_VirtualQuery,--undefined=__imp_EnterCriticalSection,--undefined=__imp_LeaveCriticalSection,--undefined=__imp_InitializeCriticalSection,--undefined=__imp_DeleteCriticalSection,--undefined=__imp_TlsGetValue \
  -lkernel32 -lwindowsapp -lsynchronization -o build/uwp-platform-tests/failure-test.exe
./build/uwp-platform-tests/failure-test.exe
# Executable transitions and generated code are covered above.
python -B delta/gpu/tests/check_layering.py .
python -B delta/tests/check_module_layering.py .

