// Linux signal-based research probes are deliberately unavailable on UWP.
// Reject attempts to arm one instead of planting an unhandled INT3 or silently
// pretending that a diagnostic/bring-up patch was installed.
#include "kern/probe/probe_arm.h"
#include <stdexcept>
#include <string>
#include <windows.h>
namespace kern::probe {
void OnFatal() { OutputDebugStringA("Prosperity guest fault\n");
}
void SetAllocTrace(uintptr_t addr, u64 min_size) { throw std::runtime_error("UWP does not support the Linux signal probe: SetAllocTrace");
}
void SetHeapProf(uintptr_t addr, bool count_only) { throw std::runtime_error("UWP does not support the Linux signal probe: SetHeapProf");
}
void SetHeapProfScope(uintptr_t tls_slot_global, u64 depth_offset) { throw std::runtime_error("UWP does not support the Linux signal probe: SetHeapProfScope");
}
void SetCntTrace(uintptr_t addr) { throw std::runtime_error("UWP does not support the Linux signal probe: SetCntTrace");
}
void SetFatalTrace(uintptr_t addr) { throw std::runtime_error("UWP does not support the Linux signal probe: SetFatalTrace");
}
void SetHdrTrace(uintptr_t addr) { throw std::runtime_error("UWP does not support the Linux signal probe: SetHdrTrace");
}
void SetRdoffFix(uintptr_t addr) { throw std::runtime_error("UWP does not support the Linux signal probe: SetRdoffFix");
}
void SetSkipFn(uintptr_t addr) { throw std::runtime_error("UWP does not support the Linux signal probe: SetSkipFn");
}
void MarkManifestFd(u32 fd, bool v) { // Used only by the unavailable RdoffFix trap; no trap is installed.
}
void SetCallSkip(uintptr_t addr, long rax_val, int insn_len) { throw std::runtime_error("UWP does not support the Linux signal probe: SetCallSkip");
}
void SetOrderTrace(uintptr_t addr, const char* label) { throw std::runtime_error("UWP does not support the Linux signal probe: SetOrderTrace");
}
void SetRetTrace(
    uintptr_t addr,
    const char* label,
    RetTraceInstruction instruction) { throw std::runtime_error("UWP does not support the Linux signal probe: SetRetTrace");
}
void SetFnWatch(uintptr_t addr, const char* label) { throw std::runtime_error("UWP does not support the Linux signal probe: SetFnWatch");
}
void StartFnWatchPrinter() { throw std::runtime_error("UWP does not support the Linux signal probe: StartFnWatchPrinter");
}
void StartPopcntPrinter(uintptr_t addr, size_t bytes, unsigned every_ms) { throw std::runtime_error("UWP does not support the Linux signal probe: StartPopcntPrinter");
}
void StartWriteWatch(uintptr_t addr,
                     size_t bytes,
                     unsigned every_ms,
                     bool trap_reads,
                     bool single_step) { throw std::runtime_error("UWP does not support the Linux signal probe: StartWriteWatch");
}
void StartWriteHist(uintptr_t addr, size_t bytes, unsigned every_ms) { throw std::runtime_error("UWP does not support the Linux signal probe: StartWriteHist");
}
void StartSumWatchPrinter(uintptr_t slot,
                          size_t off,
                          size_t stride,
                          int count,
                          unsigned every_ms) { throw std::runtime_error("UWP does not support the Linux signal probe: StartSumWatchPrinter");
}
void StartPoolMap(uintptr_t addr, size_t bytes, unsigned every_ms) { throw std::runtime_error("UWP does not support the Linux signal probe: StartPoolMap");
}
void StartPoolCensus(unsigned every_ms) { throw std::runtime_error("UWP does not support the Linux signal probe: StartPoolCensus");
}
void StartMemDump(uintptr_t addr,
                  size_t bytes,
                  unsigned after_ms,
                  const char* path) { throw std::runtime_error("UWP does not support the Linux signal probe: StartMemDump");
}
void SetFnArgs(uintptr_t addr,
               const char* label,
               const u64* offsets,
               int noffsets) { throw std::runtime_error("UWP does not support the Linux signal probe: SetFnArgs");
}
}
