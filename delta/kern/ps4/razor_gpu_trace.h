#pragma once
#include "guest_abi.h"

namespace kern::ps4 {
// Optional Razor provider interface (NID eQnlGJJAFsA). No hardware tracing
// provider is installed, so callers take their guarded null branch.
void* PS4ABI RazorGpuThreadTraceGetInterface();
}
