# Prosperity frontend for UWP

The host renders the desktop home screen, settings, pause menu, transitions,
controls legend, session log and shader activity toast through the copied
ImGui and a D3D11 renderer. The shared implementations remain in `delta/ui`;
they are compiled into `prosperity.dll` by `cmake/UwpBuild.cmake`. This keeps
ImGui, its context and allocations on the UCRT64 side of the C embedding ABI.
Only COM interface pointers, POD input events and copied UTF-8 paths cross it.
`ProsperityHost` resolves the frontend exports from the packaged DLL.

The XAML thread queues pointer, keyboard, text and focus events. The serialized
render owner alone consumes events, polls controllers and calls ImGui. The
queue holds at most 512 events, coalesces consecutive pointer movements and
resets held input on overflow. A sticky focus value survives overflow. Guest
input uses a separately locked snapshot and an atomic foreground gate; held
menu buttons must be released before reaching the guest. Core title changes
also travel through a mailbox instead of mutating the pause screen on a worker.

The recent library holds at most twelve games. Its icons and backdrops share
one font/artwork atlas, with a 32 MiB upload limit. PNG headers and dimensions
are checked before decoding. Rebuilding the library invalidates the preceding
atlas before creating its replacement. Device changes release the old D3D11
resources and upload the existing atlas to the new device, preserving UI state.
There is no growing heap of one descriptor per cover. Font glyphs, spacing,
colors, layouts and animation timings come from the desktop sources. The
procedural background uses the desktop SPIR-V translated through SPIRV-Cross.

Settings retained: native D3D12, hardware graphics device selection, emulated
display timing, vertical sync, async presentation and all four background
choices. Save/discard/defaults use the desktop panel. Graphics changes apply
after restart; the background and host vertical sync update when saved.
Unavailable Vulkan/OpenGL cards retain the desktop disabled appearance.
Vulkan validation and desktop relative mouse capture/sensitivity/invert-Y
controls are excluded. The performance overlay remains excluded as requested.
Keyboard and Xbox controller mappings are supported; F1/F2 toggle the desktop
controls/log panels. View + Menu or Ctrl opens pause after the first game frame.

Settings, recent games, artwork and logs live in ApplicationData.LocalFolder.
The UWP file/folder pickers persist and restore broker grants through the Future
Access List. Select an unpacked game's folder to grant access to its siblings.
Decrypted firmware modules default to `modules/ps4` and `modules/ps5` beneath
LocalFolder; explicit `DELTA_PS4_MODULES`/`DELTA_PS5_MODULES` options take priority.
Multiple PS5 module paths use `;` on UWP, preserving Windows drive letters.

Build the DLL with `tools/build-uwp.ps1` (uses `C:\msys64\ucrt64.exe`), then build
the UWP host. Run `tools/test-uwp-frontend.ps1` for standalone PC D3D11 WARP
readbacks, settings persistence, concurrent input, resizing, DPI scaling and
device replacement. This test does not install or launch the UWP app or boot
games. Empty-library procedural scenarios use reduced physical resolution in
WARP while keeping the desktop logical layout; these are correctness checks,
not performance measurements. Actual AppContainer presentation, picker behavior, gameplay transitions
and Xbox appearance still require runtime validation; build and WARP readbacks
alone do not establish pixel-for-pixel parity on those targets.

## Third-party provenance

- `third_party/imgui` was copied from the user-specified
  `GR2fork/shadPS4-UWP/third_party/imgui`, preserving its files and MIT license.
- That copy provides only a DX12 renderer. The additional DX11 backend comes
  from official Dear ImGui tag `v1.89.5`, matching the copied 1.89.5 headers:
  https://github.com/ocornut/imgui/tree/v1.89.5/backends.
  Its viewport and scissors were adapted to honor `FramebufferScale` for UWP.
- `DroidSans.hpp` is the exact desktop compressed font from Prosperity's pinned
  equilibrium/Tracy revision `a8511d357650282c6915f7aa9775e9c4945e42b5`:
  https://github.com/wolfpld/tracy/blob/a8511d357650282c6915f7aa9775e9c4945e42b5/profiler/src/DroidSans.hpp.
- `third_party/stb_image.h` comes from `GR2fork/externals/stb`, retaining the
  license at its end. Only PNG decoding is enabled.
