#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <cmath>
#include "uwp/gamepad_api.h"
#include "uwp/host_bridge.h"
#include "host/window.h"
#include "host/controller_input.h"
#include "kern/ps4/controller_ports.h"
#include "guest/session.h"
// Session scheduling/context ownership are supplied by this standalone host.
// The production WGI bridge, controller conversion and kernel ports are linked.
namespace uwp { bool IsActiveContext(prosperity_context* value) { return value == reinterpret_cast<prosperity_context*>(1); } }
namespace guest {
bool Paused() { return false; }
void RegisterSessionReset(void (*)()) {}
bool SpawnThread(base::StringRef, base::Function<void()>, size_t) { return false; }
void SleepForMilliseconds(u64) {}
}
namespace host { void SetAudioPaused(bool) {} }
namespace base { void WriteStandardError(const char* text, mem_size length) noexcept { std::fwrite(text, 1, length, stderr); } }
namespace base::detail {
[[noreturn]] void FatalCheckFailure(const char*, int, const char* message) { throw std::runtime_error(message); }
}
void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
int main() {
  try {
    auto* ctx = reinterpret_cast<prosperity_context*>(1);
    kern::SetControllerSource(host::PollControllerInput);
    kern::SetControllerRumble(host::SetRumble);
    const auto handle = kern::AllocateControllerHandle();
    Check(handle > 0 && kern::BindControllerPort(handle, {1, 0, 0, 0, 0}) == 0, "bind actual kernel port");
    uwp::SetForeground(true); uwp::SetFrontendInput(false, {});
    prosperity_gamepad_state input{}; input.size = sizeof(input); input.connected = 1;
    Check(prosperity_set_gamepad(ctx, &input) == 0, "neutral input");
    guest::ControllerState state{};
    Check(kern::ReadControllerPort(handle, state) && !state.buttons && state.lx == 128 && state.ly == 128, "neutral guest");
    const uint32_t expected[] = {8, 0x100000, 0x4000, 0x2000, 0x8000, 0x1000, 0x10, 0x40, 0x80, 0x20, 0x400, 0x800, 2, 4};
    for (unsigned bit = 0; bit < 14; ++bit) {
      input.buttons = 1u << bit; Check(prosperity_set_gamepad(ctx, &input) == 0, "button snapshot");
      Check(kern::ReadControllerPort(handle, state) && state.buttons == expected[bit], "WGI button to guest HID mapping");
    }
    input.buttons = 0; input.left_x = -1; input.left_y = 1; input.right_x = 1; input.right_y = -1;
    input.left_trigger = .5f; input.right_trigger = 1;
    prosperity_set_gamepad(ctx, &input); kern::ReadControllerPort(handle, state);
    Check(state.lx == 0 && state.ly == 0 && state.rx == 255 && state.ry == 255, "stick direction/range");
    Check(state.l2 == 128 && state.r2 == 255 && (state.buttons & 0x300) == 0x300, "analog trigger values in guest");
    input.left_x = input.left_y = input.right_x = input.right_y = .1f;
    prosperity_set_gamepad(ctx, &input); kern::ReadControllerPort(handle, state);
    Check(state.lx == 128 && state.ly == 128, "deadzone");
    prosperity_vibration motors{}; motors.size = sizeof(motors);
    Check(kern::VibrateControllerPort(handle, 123, 231) == 0 && prosperity_get_vibration(ctx, &motors) == 0 && motors.large_motor == 123 && motors.small_motor == 231, "guest rumble return path");
    uwp::SetForeground(false); kern::ReadControllerPort(handle, state); prosperity_get_vibration(ctx, &motors);
    Check(!state.buttons && state.lx == 128 && !state.l2 && !motors.large_motor, "focus loss neutralizes input/haptics");
    uwp::SetForeground(true); input.buttons = 4; prosperity_set_gamepad(ctx, &input); kern::ReadControllerPort(handle, state);
    Check(!state.buttons, "held button suppressed after focus return");
    input = {}; input.size = sizeof(input); input.connected = 1; prosperity_set_gamepad(ctx, &input); kern::ReadControllerPort(handle, state);
    input.buttons = 4; prosperity_set_gamepad(ctx, &input); kern::ReadControllerPort(handle, state); Check(state.buttons == 0x4000, "fresh button after neutral");
    uwp::SetFrontendInput(true, {}); kern::ReadControllerPort(handle, state); Check(!state.buttons, "menu capture");
    uwp::SetFrontendInput(false, {});
    input.connected = 0; prosperity_set_gamepad(ctx, &input); kern::ReadControllerPort(handle, state); Check(!state.buttons, "disconnect releases held input");
    input.connected = 1; input.buttons = 0; prosperity_set_gamepad(ctx, &input); kern::ReadControllerPort(handle, state);
    input.buttons = 4; prosperity_set_gamepad(ctx, &input);
    std::this_thread::sleep_for(std::chrono::milliseconds(280)); kern::ReadControllerPort(handle, state); Check(!state.buttons, "stale input timeout");
    input.left_x = NAN; Check(prosperity_set_gamepad(ctx, &input) == PROSPERITY_INVALID_ARGUMENT, "nonfinite rejected");
    std::atomic<bool> writing{true};
    std::thread producer([&] {
      prosperity_gamepad_state sample{}; sample.size = sizeof(sample); sample.connected = 1;
      for (unsigned i = 0; i < 10000; ++i) { sample.buttons = i & 1 ? 0x3fff : 0; prosperity_set_gamepad(ctx, &sample); }
      writing = false;
    });
    uint32_t all = 0; for (auto mask : expected) all |= mask;
    bool coherent = true;
    while (writing) { kern::ReadControllerPort(handle, state); coherent &= state.buttons == 0 || state.buttons == all; }
    producer.join(); Check(coherent, "concurrent snapshots cannot tear guest button masks");
    kern::ReleaseControllerHandle(handle);
    puts("PASS WGI -> actual kernel controller port: 14 buttons, sticks, analog triggers, rumble, focus/menu/disconnect/timeout"); return 0;
  } catch (const std::exception& e) { fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
