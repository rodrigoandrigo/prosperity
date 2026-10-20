#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/uwp-io-tests
clang++ -std=c++20 -O2 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DOS_WIN \
  -D_WIN32_WINNT=0x0A00 -DWINAPI_FAMILY=WINAPI_FAMILY_APP \
  -I delta -I build/uwp-deps/equilibrium tools/tests/uwp_audio_test.cc \
  -Wl,--undefined=__imp_GetLastError,--undefined=__imp_VirtualProtect,--undefined=__imp_VirtualQuery,--undefined=__imp_EnterCriticalSection,--undefined=__imp_LeaveCriticalSection,--undefined=__imp_InitializeCriticalSection,--undefined=__imp_DeleteCriticalSection,--undefined=__imp_TlsGetValue \
  -lxaudio2_9 -lole32 -lkernel32 -lwindowsapp -o build/uwp-io-tests/audio-test.exe
./build/uwp-io-tests/audio-test.exe
clang++ -std=c++20 -O2 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DOS_WIN -DPROSPERITY_EXPORTS \
  -I delta -I shared -I build/uwp-deps/equilibrium \
  tools/tests/uwp_gamepad_test.cc delta/uwp/host_bridge.cc delta/host/controller_input.cc \
  delta/kern/ps4/controller_ports.cc delta/host/gameplay_state.cc \
  build/uwp-Release/core/libeq_base.a -lsynchronization -lbcrypt -lws2_32 \
  -o build/uwp-io-tests/gamepad-test.exe
./build/uwp-io-tests/gamepad-test.exe
