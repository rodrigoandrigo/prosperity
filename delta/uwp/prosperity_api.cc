#include "uwp/prosperity_api.h"
#include "uwp/host_bridge.h"
#include "host/audio_output.h"
#include "uwp/logging.h"
#include "embedding/host.h"
#include "base/environment_variables.h"
#include "base/option.h"
#include "base/option_file.h"
#include "base/allocator/memory_error.h"
#include "cpu/backend.h"
#include "guest/pause.h"
#include "guest/session.h"
#include "kern/crash.h"
#include "kern/guest_va_space.h"
#include "main/launcher.h"
#include "io/path.h"
#include "io/storage.h"
#include "logger/logger.h"
#include "options/options.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <array>

struct prosperity_context {
  std::mutex mutex;
  std::condition_variable changed;
  std::thread worker;
  std::unique_ptr<Launcher> launcher;
  std::array<char, 2048> error{};
  std::string pending_path;
  prosperity_state state = PROSPERITY_INITIALIZING;
  prosperity_result result = PROSPERITY_OK;
  prosperity_state_callback on_state = nullptr;
  void* user = nullptr;
  bool start = false, closing = false, initialized = false, cleanup_failed = false;
  std::atomic<bool> stop{false}, fatal{false}, pause{false};
};
namespace {
std::atomic<bool> created{false};
std::atomic<prosperity_context*> active{nullptr};
thread_local prosperity_context* worker_context = nullptr;
thread_local bool state_callback = false;
bool Terminal(prosperity_state state) {
  return state == PROSPERITY_READY || state == PROSPERITY_STOPPED || state == PROSPERITY_FAILED;
}
void Error(prosperity_context* context, const char* message, prosperity_result result) {
  std::lock_guard lock(context->mutex);
  std::snprintf(context->error.data(), context->error.size(), "%s", message ? message : "Core failure");
  context->result = result;
}
void Notify(prosperity_context* context, prosperity_state state) {
  std::array<char, 2048> message{}; prosperity_result result;
  { std::lock_guard lock(context->mutex); context->state = state; message = context->error; result = context->result; }
  context->changed.notify_all();
  char line[2304];
  std::snprintf(line, sizeof(line), "[embedding] state=%u result=%d %s",
      static_cast<unsigned>(state), static_cast<int>(result), message.data());
  uwp::WriteHostLog(2, line);
  if (context->on_state) {
    state_callback = true;
    try { context->on_state(context->user, state, result, message.data()); } catch (...) {}
    state_callback = false;
  }
}
void Failure(const char* message) {
  if (auto* context = active.load()) {
    Error(context, message, PROSPERITY_INTERNAL_ERROR);
    context->fatal = true; context->stop = true; context->changed.notify_all();
    uwp::WriteHostLog(5, message);
  }
}
void Run(prosperity_context* context) noexcept {
  worker_context = context;
  try {
    cpu::EarlyInit(); options::Init();
    std::filesystem::create_directories(std::filesystem::u8path(io::MakeAbsPath(base::String("modules/ps4")).c_str()));
    std::filesystem::create_directories(std::filesystem::u8path(io::MakeAbsPath(base::String("modules/ps5")).c_str()));
    for (const auto& entry : {std::pair{"DELTA_PS4_MODULES", "modules/ps4"},
                              std::pair{"DELTA_PS5_MODULES", "modules/ps5"}}) {
      if (auto* option = base::FindOption(entry.first); option && !option->overridden())
        base::SetOptionValue(entry.first, io::MakeAbsPath(base::String(entry.second)));
    }
    context->launcher = std::make_unique<Launcher>();
    if (!context->launcher->Init()) throw std::runtime_error("Core initialization failed");
    kern::ReserveGuestVaSpace(); kern::InstallCrashHandler();
    { std::lock_guard lock(context->mutex); context->initialized = true; }
    Notify(context, PROSPERITY_READY);
    for (;;) {
      std::string path;
      { std::unique_lock lock(context->mutex);
        context->changed.wait(lock, [&] { return context->closing || context->start || context->fatal; });
        if (context->closing) break;
        if (!context->start && context->fatal.exchange(false)) {
          lock.unlock(); Notify(context, PROSPERITY_FAILED); continue;
        }
        path.swap(context->pending_path); context->start = false;
      }
      bool boot_entered = false;
      Notify(context, PROSPERITY_STARTING);
      try {
        if (context->stop) {
          // Cancellation accepted before loading must not start a new session.
        } else if (([&] { struct stat info{}; return ::stat(path.c_str(), &info) != 0; }())) {
          Error(context, "Title path does not exist", PROSPERITY_IO_ERROR);
          context->fatal = true;
        } else if (!context->stop) {
          boot_entered = true; context->launcher->Boot(path);
          if (guest::Stopping()) {
            Error(context, "The title could not be loaded", PROSPERITY_IO_ERROR); context->fatal = true;
          } else {
            Notify(context, PROSPERITY_RUNNING);
            while (!context->stop && !context->fatal && !guest::Stopping()) {
              const bool paused = context->pause.load() || !uwp::Foreground();
              if (paused != guest::Paused()) {
                guest::SetPaused(paused);
                Notify(context, paused ? PROSPERITY_PAUSED : PROSPERITY_RUNNING);
              }
              // Voice control belongs on this worker; focus events on XAML
              // only publish an atomic flag and never wait on audio I/O.
              host::SetAudioPaused(paused || !uwp::Foreground());
              std::unique_lock lock(context->mutex);
              context->changed.wait_for(lock, std::chrono::milliseconds(10));
            }
          }
        }
      } catch (const std::exception& error) {
        Error(context, error.what(), PROSPERITY_INTERNAL_ERROR); context->fatal = true;
      } catch (...) { Error(context, "Unhandled core startup failure", PROSPERITY_INTERNAL_ERROR); context->fatal = true; }
      if (boot_entered) {
        Notify(context, PROSPERITY_STOPPING);
        try { context->launcher->Stop(); }
        catch (const std::exception& error) { Error(context, error.what(), PROSPERITY_INTERNAL_ERROR); context->fatal = true; context->cleanup_failed = true; }
        catch (...) { Error(context, "Core cleanup failed", PROSPERITY_INTERNAL_ERROR); context->fatal = true; context->cleanup_failed = true; }
      }
      const bool failed = context->fatal.exchange(false);
      Notify(context, failed ? PROSPERITY_FAILED : PROSPERITY_STOPPED);
    }
  } catch (const std::exception& error) {
    Error(context, error.what(), PROSPERITY_INTERNAL_ERROR); Notify(context, PROSPERITY_FAILED);
  } catch (...) { Error(context, "Unhandled core initialization failure", PROSPERITY_INTERNAL_ERROR); Notify(context, PROSPERITY_FAILED); }
  worker_context = nullptr;
}
}
uint32_t PROSPERITY_CALL prosperity_get_abi_version() { return PROSPERITY_ABI_VERSION; }
prosperity_result PROSPERITY_CALL prosperity_create(const prosperity_config* config, prosperity_context** output) {
  if (!output) return PROSPERITY_INVALID_ARGUMENT;
  *output = nullptr;
  if (!config || config->size != sizeof(*config) || config->abi_version != PROSPERITY_ABI_VERSION ||
      !config->data_directory || !*config->data_directory || !config->on_frame) return PROSPERITY_INVALID_ARGUMENT;
  bool expected = false;
  if (!created.compare_exchange_strong(expected, true)) return PROSPERITY_INVALID_STATE;
  try {
    auto context = std::make_unique<prosperity_context>();
    context->user = config->user_data; context->on_state = config->on_state;
    std::filesystem::create_directories(std::filesystem::u8path(config->data_directory));
    io::SetDataDirectory(config->data_directory);
    base::SetEnvironmentVariable(u8"DELTA_DATA_DIR", reinterpret_cast<const char8_t*>(config->data_directory));
    base::SetEnvironmentVariable(u8"DELTA_GPU_BACKEND", u8"d3d12");
    uwp::ConfigureLogging(config->data_directory, config->on_log, config->user_data);
    logger::RouteBaseLogging();
    base::SetOutOfMemoryHandler([](void*, const char* message) { embedding::Fail(message); }, nullptr);
    uwp::SetFrameCallback(config->on_frame, config->user_data);
    active = context.get(); embedding::SetFailureHandler(Failure);
    context->worker = std::thread(Run, context.get());
    *output = context.release(); return PROSPERITY_OK;
  } catch (...) {
    active = nullptr; embedding::SetFailureHandler(nullptr);
    uwp::SetFrameCallback(nullptr, nullptr); uwp::CloseLogging(); created = false;
    return PROSPERITY_INTERNAL_ERROR;
  }
}
prosperity_result PROSPERITY_CALL prosperity_boot(prosperity_context* context, const char* path) {
  if (!context || !path || !*path) return PROSPERITY_INVALID_ARGUMENT;
  try {
    { std::lock_guard lock(context->mutex);
      if (!Terminal(context->state) || context->closing || !context->initialized) return PROSPERITY_INVALID_STATE;
      context->pending_path = path; context->start = true;
      context->stop = false; context->fatal = false; context->pause = false;
      context->error[0] = 0; context->result = PROSPERITY_OK; context->state = PROSPERITY_STARTING;
    }
    context->changed.notify_all(); return PROSPERITY_OK;
  } catch (...) { return PROSPERITY_INTERNAL_ERROR; }
}
prosperity_result PROSPERITY_CALL prosperity_pause(prosperity_context* context, uint32_t paused) {
  if (!context) return PROSPERITY_INVALID_ARGUMENT;
  std::lock_guard lock(context->mutex);
  if (context->state != PROSPERITY_RUNNING && context->state != PROSPERITY_PAUSED) return PROSPERITY_INVALID_STATE;
  context->pause = paused != 0; context->changed.notify_all(); return PROSPERITY_OK;
}
prosperity_result PROSPERITY_CALL prosperity_request_stop(prosperity_context* context) {
  if (!context) return PROSPERITY_INVALID_ARGUMENT;
  context->stop = true; context->changed.notify_all(); return PROSPERITY_OK;
}
prosperity_result PROSPERITY_CALL prosperity_stop(prosperity_context* context) { return prosperity_request_stop(context); }
prosperity_state PROSPERITY_CALL prosperity_get_state(prosperity_context* context) {
  if (!context) return PROSPERITY_FAILED;
  std::lock_guard lock(context->mutex); return context->state;
}
prosperity_result PROSPERITY_CALL prosperity_wait(prosperity_context* context, uint32_t timeout) {
  if (!context) return PROSPERITY_INVALID_ARGUMENT;
  if (worker_context == context || state_callback || uwp::InLogCallback() || uwp::InFrameCallback()) return PROSPERITY_INVALID_STATE;
  std::unique_lock lock(context->mutex);
  const auto done = [&] { return Terminal(context->state) && !context->start; };
  if (timeout == UINT32_MAX) context->changed.wait(lock, done);
  else if (!context->changed.wait_for(lock, std::chrono::milliseconds(timeout), done)) return PROSPERITY_TIMEOUT;
  return context->result;
}
prosperity_result PROSPERITY_CALL prosperity_destroy(prosperity_context* context) {
  if (!context) return PROSPERITY_OK;
  if (worker_context == context || state_callback || uwp::InLogCallback() || uwp::InFrameCallback()) return PROSPERITY_INVALID_STATE;
  { std::lock_guard lock(context->mutex); context->closing = true; context->stop = true; }
  context->changed.notify_all();
  if (context->worker.joinable()) context->worker.join();
  if (context->cleanup_failed || guest::LiveThreads()) return PROSPERITY_INTERNAL_ERROR;
  active = nullptr; embedding::SetFailureHandler(nullptr);
  kern::UninstallCrashHandler();
  io::ClearStorage();
  uwp::ClearMemoryPolicy();
  host::CloseAllAudioPorts();
  uwp::SetFrameCallback(nullptr, nullptr); uwp::CloseLogging();
  delete context; created = false; return PROSPERITY_OK;
}
void PROSPERITY_CALL prosperity_get_last_error(prosperity_context* context, char* buffer, uint32_t capacity) {
  if (!buffer || !capacity) return;
  buffer[0] = 0; if (!context) return;
  std::lock_guard lock(context->mutex); std::snprintf(buffer, capacity, "%s", context->error.data());
}

namespace uwp { bool IsActiveContext(prosperity_context* context) { return context && active.load() == context; } }
