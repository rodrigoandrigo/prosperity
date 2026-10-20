#pragma once
#include "uwp/prosperity_api.h"
namespace host { struct PadKeys; }
namespace uwp {
void SetForeground(bool focused);
bool Foreground();
bool PollPhysicalPad(host::PadKeys& out);
void SetFrontendInput(bool blocked, const host::PadKeys& keys);
bool InFrameCallback();
void SetFrameCallback(prosperity_frame_callback callback, void* user_data);
}
