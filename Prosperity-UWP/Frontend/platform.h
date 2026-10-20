#pragma once
#include "base/strings/xstring.h"
namespace uwp::frontend {
void SetPaused(bool paused);
void SetPresentationMode(bool immediate);
void OpenPicker(bool folder);
bool PickerPending();
base::String TakePickerPath();
void QueueTitle(const base::String& title);
}  // namespace uwp::frontend
