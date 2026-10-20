# Prosperity UWP DLL

The emulator DLL builds with Clang in UCRT64, launched through
`C:\msys64\ucrt64.exe`. The XAML host builds with MSVC C++/CX. Their boundary
is `delta/uwp/prosperity_api.h`: ten C exports, ABI version 2, UTF-8 strings,
fixed-width values, opaque contexts and synchronous pixel callbacks. Allocations
never cross the compiler boundary.

## Embedding lifecycle and diagnostics

`prosperity_create` validates configuration, opens `prosperity.log` under the
host-provided LocalFolder and starts an owned control worker. Initialization,
title loading, the emulation session, pause changes and cleanup run outside the
UI thread. `prosperity_boot` queues a start; OK means accepted, not loaded.
`prosperity_pause` queues pause/resume. `prosperity_request_stop` and its
`prosperity_stop` alias signal the worker and return without joining it.

Observe progress with `on_state`, `prosperity_get_state` and
`prosperity_get_last_error`. States are INITIALIZING, READY, STARTING, RUNNING,
PAUSED, STOPPING, STOPPED and FAILED. Startup and worker errors produce FAILED
with an error result and UTF-8 diagnostic. `prosperity_wait(timeout_ms)` is an
explicitly blocking wait for READY/STOPPED/FAILED; it returns TIMEOUT when the
deadline expires. UINT32_MAX means infinite. Initialization failure is terminal;
startup failures may be retried after cleanup succeeds.

`prosperity_destroy` requests stop, joins the control worker and releases the
context only on OK. It must run off the UI thread. On cleanup failure, retain
the context and callback storage. The UWP host schedules destruction on its
thread pool and keeps frame/callback storage alive through cleanup. UI status
is refreshed through the XAML dispatcher.

Callbacks execute on core threads, never implicitly on the UI dispatcher.
They may query state/errors or request stop. Wait/destroy from a log, state or
frame callback return INVALID_STATE rather than deadlocking. Do not throw across
the C callback boundary. Callback strings/pixels are borrowed during the call.
The host must serialize final destruction against its other API calls.

Logging is synchronous to a serialized file/callback sink; there is no detached
logger thread and no console requirement. `on_log` receives severity 0..5
(trace, debug, info, warning, error, critical). Base diagnostics, standard-error
bridge and SDL logs use these sinks. The callback is detached and the file is
flushed/closed after workers join. Shader audit and syscall summaries use session
cleanup rather than process atexit handlers in UWP.

Controlled GPU/assert/check failures report to the embedding context rather
than aborting the process. Exceptions at guest and base worker boundaries become
session errors; guest exits use the native exit gate. Host exceptions are chained
to the previous exception filter, which is restored on destruction. This is not
a sandbox: arbitrary native memory corruption and CRT security fail-fast paths
cannot safely be converted into recoverable errors in an in-process DLL.

## Build

```powershell
./tools/build-uwp.ps1 -Configuration Release -Jobs 12
```

The build creates and copies these files to `Prosperity-UWP/Runtime/Release`:

- `prosperity.dll`, with static libstdc++, libgcc and winpthreads;
- `SDL3.dll`, built from `C:\Users\rodri\Dev1\Projetos\SDL3-uwp` with the
  MSVC Windows Store toolchain;
- `dxcompiler.dll` and `dxil.dll`, from Windows SDK 10.0.26100.0 x64.

CMake exposes `DELTA_DXC_DIRECTORY` for a different shader compiler distribution.
The UWP project deploys runtime DLLs at the package root. The MSVC UWP VCLibs
framework supplies SDL's App CRT dependencies; desktop DLL tests must explicitly
add an installed x64 VCLibs directory to their DLL search path.

Dependency snapshots live in `build/uwp-deps`; existing submodule worktrees are
preserved. The recipe pins equilibrium to
`3d1355b901f5c25dd08cfc26971aaf7290943030`, because the superproject's
`06c3b1039bc15eab1af5c27820b85b4549ea6c53` is absent locally and upstream
rejected fetching it. `-EquilibriumRevision` can explicitly override the pin.
Each extracted snapshot records its revision in `.prosperity-revision`.

## Platform integration

The UWP demand-memory strategy and comparison with GR2fork are documented in
[uwp-memory.md](uwp-memory.md).

The core selects native D3D12. The host loads it with `LoadPackagedLibrary`,
checks the ABI, supplies LocalFolder as writable storage, and copies callback
frames for Direct2D/SwapChainPanel presentation. Xbox application zoom, demo
cube and FPS UI have been removed; swapchain size follows panel pixel dimensions.
The controller bridge uses Windows.Gaming.Input for buttons, sticks, analog
triggers and vibration. Generic WGI gamepads do not expose RGB lighting.

The Windows memory adapter supplies demand commit, named shared sections,
16 KiB aliases, partial unmapping, logical page protections and reservation
release. GPU direct-memory discovery reads this registry instead of `/proc`.
MMIO uses Windows CONTEXT. The UWP-supported top-level exception filter handles
memory/MMIO faults; it is a last-chance handler and debugger behavior differs.
Pause suspends only threads observed in registered guest code. Stop revokes
execution of their current guest page and redirects the resulting exception to
the exit gate. Native guest setjmp uses a null unwind frame, because guest
frames have no Windows unwind metadata.

UnRAR's actual decoder is retained, including Mark of the Web support. Unicode
comparison/casing uses app APIs. Desktop profile/configuration search, CWD
changes, arbitrary ACL restoration, DOS short-name manipulation, hard-link
creation, privilege elevation and shutdown are unavailable in this embedded
configuration. Requested unsupported operations fail explicitly. Linux signal
research probes and evdev/uinput passthrough also fail explicitly when enabled;
the normal host controller bridge is used instead.

## Validation and limits

Release x64 DLL and MSVC UWP host builds pass. Standalone PC checks cover:

- seven MMIO context cases;
- demand commit, aliases, remapping, protected copies and reservation release;
- UDP loopback, POSIX poll bits, select and nonblocking errors;
- pause/resume/stop of a native loop without host calls;
- DLL exports, context lifetime, ABI rejection, error buffers and C++ exceptions.
- nonblocking commands, explicit wait timeout/cancellation, callback reentry,
  LocalFolder log persistence and restoration of host/SDL handlers;
- controlled worker/GPU/check failures returning through the host boundary.

Run `bash tools/test-uwp-platform.sh` inside UCRT64 after building Release.
Run `python tools/tests/uwp_abi_test.py --vclibs <installed-x64-VCLibs-directory>`
from Windows Python to check the deployed DLL without installing or booting a game.
Logs are under `build/uwp-Release`, `build/uwp-platform-tests.log` and
`build/uwp-host-Release.log`.

These are build and standalone desktop results, not AppContainer/gameplay/Xbox
validation. Executable protection through FromApp still needs a packaged
`codeGeneration` probe: the standalone execution check returned error 87, so the
platform suite explicitly reports that step as skipped. Private file mappings
(copy-on-write) and discard/shrink semantics remain unsupported. Hardware input,
rendered game frames, packaged DXC operation and device lifecycle still require
validation in the UWP app. The Clang/GCC static runtime link emits duplicate RTTI
section-size warnings; the ABI test exercises exception handling but does not
establish exhaustive runtime compatibility. No app was installed or game booted.


## ImGui frontend

The UWP host now calls the frontend exported by prosperity.dll. See
[frontend implementation and compatibility matrix](../Prosperity-UWP/Frontend/README.md).
The desktop home, theme/font, settings, pause, transition, log and busy UI code
is shared directly. Native pickers and a bounded input mailbox are supplied by
UWP. The twelve-game artwork cache uses one capped atlas instead of growing
per-cover descriptors. Run `tools/test-uwp-frontend.ps1` after the DLL build for
standalone D3D11 readbacks and concurrency/device/DPI/settings checks.

## Brokered files and folders

The C ABI now accepts native handles, offset-based streams and mounted storage
providers through `prosperity_import_file_handle`,
`prosperity_import_file_stream` and `prosperity_mount_storage`. The existing
embedding ABI remains version 2; the independently sized storage table is
version 1. `shared/io/storage_api.h` defines ownership, callbacks, flags and
statuses without passing WinRT/C++/CRT objects across the DLL boundary.

The UWP picker retains its actual StorageFile/StorageFolder in an agile adapter
and registers that adapter before returning the path to the frontend. Saved
FutureAccessList entries are restored and mounted before the render loop reads
the library. A raw path is never treated as a substitute for a broker grant.
Pathless provider items receive a stable logical path derived from their
persistent token. The previous host-imposed 32-grant cap has been removed;
the OS FutureAccessList capacity still applies.
A single-file grant does not authorize siblings: choose the game folder when
the executable requires adjacent metadata, modules or assets.

The adapter prefers IStorageItemHandleAccess/IStorageFolderHandleAccess with
overlapped native handles. Unsupported providers fall back to
IRandomAccessStream and GetInputStreamAt/ReadAsync. ImportBrokeredStream also
adapts a directly supplied WinRT stream. Reads use caller buffers, with an extra
copy only when a stream provider returns a different buffer. Folder enumeration
uses lazy 128-entry pages, including nested folders; this page size is not a
directory or file-count limit. No whole-file staging, byte-rate throttle or
artificial file-size limit is introduced. Natural filesystem, signed 64-bit
offset, WinRT paging and provider limits remain. Actual bandwidth depends on
the device/provider; it has not been benchmarked. Emulation timing is unchanged.

Loaders, covers, ZIP/PKG, the read-only UnRAR adapter and guest VFS share the
FileBase broker path. UWP stat/opendir/readdir are routed to the same mounts.
Provider failures are authoritative: a denied broker open does not fall back
to opening the raw filesystem path. Local app data retains native access.
The public file API provides read_at/write_at/get_size/flush/close and storage
stat/list. Native/stream imports are read-only; a provider can authorize writes.
CREATE preserves existing data; TRUNCATE must be requested explicitly.

I/O APIs are synchronous and belong on workers; the WinRT adapter rejects an
STA wait. Callbacks must be safe for independent concurrent opens and must not
throw across the ABI. Mount tables are copied and retained. Unmount/replacement
affects subsequent opens; existing files/iterators retain the provider until
closed. File operations serialize only per public file object, never through a
global I/O lock. Close must wait for the caller's outstanding operations, and
provider callbacks must remain callable until their final release, including
files retained after context destruction. Do not destroy a context concurrently
with mount/open/stat/list calls.

Run `tools/test-uwp-storage.ps1` for actual deployed-DLL tests: 513 entries,
eight concurrent files, short stream reads, offsets above 4 GiB, an 8 GiB virtual
stream, native handle duplication, cancellation, lifetime after unmount/destroy,
and write/create preservation. Its separate C++/CX desktop harness exercises
real StorageFolder/StorageFile/IRandomAccessStream objects, nested reads and
multi-page enumeration. These desktop tests do not prove picker grants or
FutureAccessList persistence inside a packaged AppContainer or on Xbox.
No package is installed or application/game launched by these tests.

Permission model: https://learn.microsoft.com/en-us/windows/apps/develop/files/file-access-permissions

## XAudio2 and Windows.Gaming.Input

The UWP build selects `delta/host/audio_output_xaudio2.cc` instead of the SDL
audio backend. The existing launcher AudioSink connects the emulated audio
daemon's decoded PCM directly to XAudio2. S16/F32 mono, stereo and 7.1 use
WAVEFORMATEXTENSIBLE, with source-rate conversion by XAudio2 and a default
48 kHz mastering voice. Existing per-channel daemon gains remain in the core;
port gain uses source-voice volume. The DLL imports the system XAudio2_9 DLL.

PCM is copied into port-owned slots before submission, so guest memory may be
reused immediately. Buffer-end callbacks release slots without allocating,
blocking or taking the producer mutex. The live queue targets about 100 ms;
bounded waits happen on the audio producer, never in a XAudio2 callback.
Pause/focus loss stops playback and prevents accumulating queued audio.
DestroyVoice finishes callbacks before buffers are freed. Registry snapshots
keep ports alive during concurrent close. Critical errors rebuild the engine
and source voices on subsequent PCM submissions. A logical port survives a
missing output device and retries initialization with a 250 ms backoff.

`Prosperity-UWP/GamepadInput.cpp` polls WGI through a ThreadPoolTimer with a
nominal 8 ms interval, independent of presentation. A versioned POD snapshot
crosses the DLL boundary through `delta/uwp/gamepad_api.h`; no WinRT objects
cross into the UCRT64 core. PollPhysicalPad drives both ImGui navigation and the
existing PollControllerInput/kernel HID path. A/B/X/Y map to
Cross/Circle/Square/Triangle, Menu to Options and View to touchpad click.
Stick Y is inverted into guest coordinates, with a 15% deadzone and rescaling;
analog trigger values are preserved. Guest large/small motors map to WGI
left/right vibration motors. The first connected pad remains selected until
disconnected; the emulator currently exposes one local controller.

Input snapshots are synchronized and expire after 250 ms. Disconnect/focus
loss releases buttons, axes and vibration. Returning from menus/focus requires
a neutral sample before held buttons reach gameplay. Stop cancels the timer,
neutralizes input and clears the context under the same lock as callbacks.
Hardware vibration is stopped before context teardown. Generic WGI pads do
not provide touch coordinates, gyro or controller RGB through this adapter.

Validation commands:

- `bash tools/test-uwp-io.sh` in UCRT64: real XAudio2 playback callbacks for six
  PCM layouts, exact frame consumption, gain, pause/resume, 44.1 kHz input,
  concurrent close, engine recreation and injected critical-error recovery;
  production WGI snapshot mapping through the real kernel controller port,
  including all 14 buttons, analog triggers, rumble, focus/menu/disconnect,
  timeout and 10,000 concurrent snapshot updates.
- `tools/test-uwp-wgi.ps1`: actual Windows.Gaming.Input timer connected to the
  deployed DLL, including focus and callback lifetime after stop.
- Existing ABI, brokered-storage and frontend WARP tests remain applicable.

These are standalone PC results. The WGI runtime probe found zero physical
controllers, so hardware button/rumble and gameplay/AppContainer/Xbox checks
remain pending. XAudio2 callback completion proves device consumption, not an
acoustic recording of game output. No UWP application/game is launched by the
test scripts.

Microsoft references: [UWP audio](https://learn.microsoft.com/en-us/windows/uwp/gaming/working-with-audio-in-your-directx-game)
and [gamepad input/vibration](https://learn.microsoft.com/en-us/windows/uwp/gaming/gamepad-and-vibration).
