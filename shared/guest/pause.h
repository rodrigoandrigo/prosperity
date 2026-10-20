#pragma once

#include <stdint.h>

#include "base/arch.h"

namespace guest {

bool Paused();
void SetPaused(bool paused);
void PollPause();
void PausePoint();
bool OnGuestThread();
#if defined(_WIN32)
bool HandleStopFault(void* context);
#endif
const i32* PauseFlagAddress();
void RegisterCode(uintptr_t address, mem_size size);
void SetThreadExitHandler(void (*exit)());
void ResetCodeRanges();

class ThreadRegistration {
 public:
  explicit ThreadRegistration(bool (*is_guest_pc)(uintptr_t) = nullptr);
  ~ThreadRegistration();
  ThreadRegistration(const ThreadRegistration&) = delete;
  ThreadRegistration& operator=(const ThreadRegistration&) = delete;

 private:
  bool owner_ = false;
};

}  // namespace guest
