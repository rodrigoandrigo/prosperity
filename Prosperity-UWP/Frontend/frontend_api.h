#pragma once
#include "../../delta/uwp/prosperity_api.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct prosperity_frontend prosperity_frontend;
/* Input is queued, never delivered directly to ImGui. Coordinates are DIPs;
 * keys use Windows VirtualKey values. Overflow resets held input safely. */
typedef struct prosperity_input {
  uint32_t type; /* 0 focus, 1 position, 2 button, 3 wheel, 4 key, 5 Unicode */
  uint32_t code;
  float x, y;
} prosperity_input;
PROSPERITY_API prosperity_result PROSPERITY_CALL
prosperity_frontend_create(prosperity_context*, prosperity_frontend**);
PROSPERITY_API void PROSPERITY_CALL
prosperity_frontend_input(prosperity_frontend*, const prosperity_input*);
/* Picker completion is copied into a bounded mailbox. Null means cancelled. */
PROSPERITY_API void PROSPERITY_CALL
prosperity_frontend_path(prosperity_frontend*, const char*);
/* Sole render owner, serialized with resize/device loss. COM interfaces only;
 * no C++ or ImGui objects cross this ABI. Returns picker/exit actions: 1/2/4.
 * Bits 32/64 request the PS4/PS5 module archive picker.
 * Bit 8 asks the host for immediate presentation (vsync off). */
PROSPERITY_API uint32_t PROSPERITY_CALL
prosperity_frontend_render(prosperity_frontend*,
                           void* device,
                           void* immediate_context,
                           void* target,
                           uint32_t width,
                           uint32_t height,
                           float logical_width,
                           float logical_height,
                           float delta_seconds,
                           uint32_t has_frame);
/* After the last render, before destroying the emulator context. */
PROSPERITY_API void PROSPERITY_CALL
prosperity_frontend_destroy(prosperity_frontend*);
typedef struct prosperity_frontend_info {
  uint32_t size, queued_events, atlas_bytes, games, vertices, indices;
  uint32_t held_keys, focused, input_blocked;
  char error[2048];
} prosperity_frontend_info;
/* Read on the render owner, between render calls. */
PROSPERITY_API prosperity_result PROSPERITY_CALL
prosperity_frontend_get_info(prosperity_frontend*, prosperity_frontend_info*);
#ifdef __cplusplus
}
#endif
