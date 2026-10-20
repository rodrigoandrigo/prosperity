#pragma once
#include "../delta/uwp/gamepad_api.h"
#include <memory>
namespace Prosperity_UWP {
class GamepadInput {
 public:
  GamepadInput();
  ~GamepadInput();
  bool Start(prosperity_context*, decltype(&prosperity_set_gamepad), decltype(&prosperity_get_vibration));
  void Focus(bool);
  void Stop();
 private:
  struct State;
  std::shared_ptr<State> state;
};
}
