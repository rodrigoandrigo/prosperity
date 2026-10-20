#include "uwp/host_bridge.h"
#include "host/window.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include "uwp/gamepad_api.h"
#include "uwp/storage_bridge.h"
#include "host/audio_output.h"
#include "guest/pause.h"
#include <cmath>
#include <algorithm>

namespace {
std::mutex g_frame_mutex;
prosperity_frame_callback g_frame = nullptr;
void* g_user = nullptr;
std::atomic<bool> g_stop{false};
thread_local bool in_frame_callback = false;
std::mutex g_input_mutex;
host::PadKeys g_keys;
bool g_input_blocked = false, g_suppress_pad = false;
std::atomic<bool> g_foreground{true};
std::mutex g_pad_mutex;
prosperity_gamepad_state g_pad{};
uint64_t g_pad_time = 0;
std::atomic<uint32_t> g_vibration{0};
u8 Axis(float value, bool invert = false) {
  value = std::clamp(value, -1.f, 1.f);
  const float magnitude = std::abs(value);
  value = magnitude <= .15f ? 0.f : std::copysign((magnitude - .15f) / .85f, value);
  if (invert) value = -value;
  return static_cast<u8>(std::lround(128 + value * (value < 0 ? 128 : 127)));
}
}
namespace uwp {
bool InFrameCallback() { return in_frame_callback; }
void SetFrameCallback(prosperity_frame_callback callback, void* user) {
  std::lock_guard lock(g_frame_mutex);
  g_frame = callback;
  g_user = user;
  g_stop.store(false);
  if (!callback) { std::lock_guard input(g_pad_mutex); g_pad = {}; g_vibration = 0; }
}
}
namespace host {
bool CanPresent() {
  std::lock_guard lock(g_frame_mutex);
  return g_frame && !g_stop.load();
}
bool Init(const char*, u32, u32) { return CanPresent(); }
bool Ensure(const char*, u32, u32) { return CanPresent(); }
bool Available() { return CanPresent(); }
void Present(const void* pixels, u32 w, u32 h, u32 pitch, PixelFormat format) {
  std::lock_guard lock(g_frame_mutex);
  if (g_frame && !g_stop.load() && pixels && w && h) {
    in_frame_callback = true;
    try { g_frame(g_user, pixels, w, h, pitch ? pitch : w * 4,
            format == PixelFormat::kBgra8 ? 1 : 0); } catch (...) {}
    in_frame_callback = false;
  }
}
void RequestPresentStop() { g_stop.store(true); }
bool PumpEvents() { return !g_stop.load(); }
void RefreshFrame(bool) {}
bool CanPresentShared(const u8*) { return false; }
bool PresentShared(u32, int, u64, u32, u32, PixelFormat) { return false; }
bool PollKeyboardPad(PadKeys& out) {
  const bool connected = uwp::PollPhysicalPad(out);
  std::lock_guard lock(g_input_mutex);
  if (g_input_blocked || !g_foreground.load()) { out = {}; return true; }
  if (!connected) out = {};
  if (g_suppress_pad) {
    const bool neutral = !out.cross && !out.circle && !out.square && !out.triangle && !out.options && !out.touchpad &&
        !out.up && !out.down && !out.left && !out.right && !out.l1 && !out.r1 && !out.l2 && !out.r2 && !out.l3 && !out.r3 &&
        out.lx == 128 && out.ly == 128 && out.rx == 128 && out.ry == 128;
    if (neutral) g_suppress_pad = false;
    else out = {};
  }
  out.cross |= g_keys.cross; out.circle |= g_keys.circle;
  out.square |= g_keys.square; out.triangle |= g_keys.triangle;
  out.l1 |= g_keys.l1; out.r1 |= g_keys.r1; out.l2 |= g_keys.l2; out.r2 |= g_keys.r2;
  out.options |= g_keys.options; out.touchpad |= g_keys.touchpad;
  if (g_keys.lx != 128) out.lx = g_keys.lx;
  if (g_keys.ly != 128) out.ly = g_keys.ly;
  if (g_keys.rx != 128) out.rx = g_keys.rx;
  if (g_keys.ry != 128) out.ry = g_keys.ry;
  return true;
}

void SetRumble(u8 large, u8 small) {
  std::lock_guard lock(g_input_mutex);
  g_vibration = g_foreground && !g_input_blocked ? uint32_t(large) | (uint32_t(small) << 8) : 0;
}
void SetControllerLight(u8, u8, u8) {}
void SetTitle(const char*) {}
void SetIcon(const u8*, size_t) {}
void ShowSplash(base::Vector<u8>) {}
void ShowExitTransition() {}
void QueryVram(u64& used, u64& total) { used = total = 0; }
void ResetGuest() { SetRumble(0, 0); host::SetAudioPaused(!g_foreground || guest::Paused()); g_stop.store(false); }
void Shutdown() { SetRumble(0, 0); g_stop.store(true); }
}

namespace uwp {
bool Foreground() { return g_foreground.load(); }
void SetForeground(bool focused) {
  g_foreground = focused;
  { std::lock_guard lock(g_input_mutex); if (!focused) { g_keys = {}; g_vibration = 0; } else g_suppress_pad = true; }
}
void SetFrontendInput(bool blocked, const host::PadKeys& keys) {
  std::lock_guard lock(g_input_mutex);
  if (g_input_blocked && !blocked) g_suppress_pad = true;
  g_input_blocked = blocked; g_keys = keys;
  if (blocked) g_vibration = 0;
}
bool PollPhysicalPad(host::PadKeys& out) {
  std::lock_guard lock(g_pad_mutex);
  out = {};
  if (!g_pad.connected || GetTickCount64() - g_pad_time > 250) { g_vibration = 0; return false; }
  const auto b = g_pad.buttons;
  out.options = b & 1; out.touchpad = b & 2;
  out.cross = b & 4; out.circle = b & 8; out.square = b & 16; out.triangle = b & 32;
  out.up = b & 64; out.down = b & 128; out.left = b & 256; out.right = b & 512;
  out.l1 = b & 1024; out.r1 = b & 2048; out.l3 = b & 4096; out.r3 = b & 8192;
  out.l2_axis = u8(std::lround(std::clamp(g_pad.left_trigger, 0.f, 1.f) * 255));
  out.r2_axis = u8(std::lround(std::clamp(g_pad.right_trigger, 0.f, 1.f) * 255));
  out.l2 = out.l2_axis > 30; out.r2 = out.r2_axis > 30;
  out.lx = Axis(g_pad.left_x); out.ly = Axis(g_pad.left_y, true);
  out.rx = Axis(g_pad.right_x); out.ry = Axis(g_pad.right_y, true);
  return true;
}
}
prosperity_result PROSPERITY_CALL prosperity_set_gamepad(prosperity_context* context, const prosperity_gamepad_state* state) {
  if (!uwp::IsActiveContext(context) || !state || state->size != sizeof(*state) ||
      !std::isfinite(state->left_x) || !std::isfinite(state->left_y) || !std::isfinite(state->right_x) ||
      !std::isfinite(state->right_y) || !std::isfinite(state->left_trigger) || !std::isfinite(state->right_trigger)) return PROSPERITY_INVALID_ARGUMENT;
  std::lock_guard lock(g_pad_mutex); g_pad = *state; g_pad_time = GetTickCount64();
  if (!state->connected) g_vibration = 0;
  return PROSPERITY_OK;
}
prosperity_result PROSPERITY_CALL prosperity_get_vibration(prosperity_context* context, prosperity_vibration* value) {
  if (!uwp::IsActiveContext(context) || !value || value->size != sizeof(*value)) return PROSPERITY_INVALID_ARGUMENT;
  std::lock_guard lock(g_pad_mutex);
  const uint32_t motors = g_foreground && g_pad.connected && GetTickCount64() - g_pad_time <= 250 ? g_vibration.load() : 0;
  value->large_motor = motors & 255; value->small_motor = (motors >> 8) & 255; return PROSPERITY_OK;
}
