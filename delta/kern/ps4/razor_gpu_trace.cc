#include "kern/ps4/razor_gpu_trace.h"

namespace kern::ps4 {
void* PS4ABI RazorGpuThreadTraceGetInterface() {
  // Verified against libSceVrTracker: calls at +0xf0f64 and +0xf11a6 take no
  // arguments, test RAX for null, then call interface slots +0x38/+0x40 only
  // for a non-null provider. Returning an integer error here would be treated
  // as a pointer and crash. This does not claim to implement GPU tracing.
  return nullptr;
}
}
