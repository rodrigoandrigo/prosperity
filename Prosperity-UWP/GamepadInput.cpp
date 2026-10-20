#include <windows.h>
#include <agile.h>
#include <collection.h>
#include <mutex>
#include "GamepadInput.h"
using namespace Windows::Gaming::Input;
using namespace Windows::System::Threading;
using namespace Windows::Foundation;
namespace Prosperity_UWP {
struct GamepadInput::State {
  std::mutex mutex;
  prosperity_context* context = nullptr;
  decltype(&prosperity_set_gamepad) publish = nullptr;
  decltype(&prosperity_get_vibration) vibration = nullptr;
  Platform::Agile<Gamepad> active;
  ThreadPoolTimer^ timer = nullptr;
  bool focused = true;
  void Silence() noexcept {
    try { auto pad = active.Get(); if (pad) pad->Vibration = GamepadVibration{}; } catch (...) {}
  }
  void Neutral() {
    prosperity_gamepad_state input{}; input.size = sizeof(input);
    if (context && publish) publish(context, &input);
  }
  void Poll() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!context) return;
    if (!focused) { Neutral(); Silence(); return; }
    try {
      auto pads = Gamepad::Gamepads;
      auto selected = active.Get();
      unsigned index = 0;
      if (selected && !pads->IndexOf(selected, &index)) { Silence(); Neutral(); selected = nullptr; }
      if (!selected && pads->Size) selected = pads->GetAt(0);
      active = Platform::Agile<Gamepad>(selected);
      if (!selected) { Neutral(); return; }
      const auto reading = selected->GetCurrentReading();
      prosperity_gamepad_state input{}; input.size = sizeof(input); input.connected = 1;
      input.buttons = static_cast<uint32_t>(reading.Buttons);
      input.left_x = float(reading.LeftThumbstickX); input.left_y = float(reading.LeftThumbstickY);
      input.right_x = float(reading.RightThumbstickX); input.right_y = float(reading.RightThumbstickY);
      input.left_trigger = float(reading.LeftTrigger); input.right_trigger = float(reading.RightTrigger);
      publish(context, &input);
      prosperity_vibration motors{}; motors.size = sizeof(motors);
      if (vibration(context, &motors) == PROSPERITY_OK) {
        GamepadVibration output{};
        output.LeftMotor = motors.large_motor / 255.0; output.RightMotor = motors.small_motor / 255.0;
        selected->Vibration = output;
      }
    } catch (...) { Neutral(); Silence(); active = Platform::Agile<Gamepad>(); }
  }
};
GamepadInput::GamepadInput() : state(std::make_shared<State>()) {}
GamepadInput::~GamepadInput() { Stop(); }
bool GamepadInput::Start(prosperity_context* context, decltype(&prosperity_set_gamepad) publish,
    decltype(&prosperity_get_vibration) vibration) {
  if (!context || !publish || !vibration) return false;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->context) return true;
  state->context = context; state->publish = publish; state->vibration = vibration;
  std::weak_ptr<State> weak(state);
  try {
    TimeSpan interval{}; interval.Duration = 80000;
    state->timer = ThreadPoolTimer::CreatePeriodicTimer(ref new TimerElapsedHandler([weak](ThreadPoolTimer^) {
      if (auto current = weak.lock()) current->Poll();
    }), interval);
    return true;
  } catch (...) { state->context = nullptr; return false; }
}
void GamepadInput::Focus(bool value) {
  std::lock_guard<std::mutex> lock(state->mutex); state->focused = value;
  if (!value) { state->Neutral(); state->Silence(); }
}
void GamepadInput::Stop() {
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->timer) { state->timer->Cancel(); state->timer = nullptr; }
  state->Neutral(); state->Silence(); state->context = nullptr;
  state->active = Platform::Agile<Gamepad>();
}
}

