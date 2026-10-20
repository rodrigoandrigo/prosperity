#pragma once
#include "prosperity_api.h"
#ifdef __cplusplus
extern "C" {
#endif
// Bits match Windows.Gaming.Input.GamepadButtons; sticks [-1,1], +Y up,
// triggers [0,1]. Refresh within 250 ms or input becomes neutral.
typedef struct prosperity_gamepad_state {
  uint32_t size, connected, buttons;
  float left_x, left_y, right_x, right_y, left_trigger, right_trigger;
} prosperity_gamepad_state;
typedef struct prosperity_vibration {
  uint32_t size, large_motor, small_motor; // DS4 amplitudes [0,255]
} prosperity_vibration;
// Thread-safe. Stop the host poller before destroying the context.
PROSPERITY_API prosperity_result PROSPERITY_CALL prosperity_set_gamepad(prosperity_context*, const prosperity_gamepad_state*);
PROSPERITY_API prosperity_result PROSPERITY_CALL prosperity_get_vibration(prosperity_context*, prosperity_vibration*);
#ifdef __cplusplus
}
#endif
