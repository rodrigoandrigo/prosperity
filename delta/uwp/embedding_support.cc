#include "embedding/host.h"
#include "guest/session.h"
#include "guest/pause.h"
#include "cpu/backend.h"
#include <atomic>
#include <stdexcept>
namespace embedding {
namespace { std::atomic<FailureHandler> handler{nullptr}; }
void SetFailureHandler(FailureHandler value) { handler.store(value); }
void ReportFailure(const char* message) noexcept {
  try { if (auto callback = handler.load()) callback(message); } catch (...) {}
}
[[noreturn]] void Fail(const char* message) {
  ReportFailure(message);
  if (guest::OnGuestThread()) {
    guest::RequestStop();
    cpu::ExitGuestThread();
  }
  throw std::runtime_error(message ? message : "Embedded core failure");
}
}
