#include "uwp/logging.h"
#include "logger/logger.h"
#include "base/logging.h"
#include <windows.h>
#include <mutex>
#include <string>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <SDL3/SDL_log.h>
namespace {
struct Logging {
  std::recursive_mutex mutex;
  HANDLE file = INVALID_HANDLE_VALUE;
  prosperity_log_callback callback = nullptr;
  void* user = nullptr;
  std::vector<base::UniquePointer<logger::LogSink>> sinks;
};
Logging& Logs() { static Logging logs; return logs; }
thread_local bool in_callback = false;
SDL_LogOutputFunction previous_sdl_log = nullptr;
void* previous_sdl_user = nullptr;
}
namespace uwp {
bool InLogCallback() { return in_callback; }
void SDLCALL SdlLog(void*, int, SDL_LogPriority priority, const char* message) {
  WriteHostLog(priority > 0 ? static_cast<uint32_t>(priority) - 1 : 0, message);
}
void ConfigureLogging(const char* directory, prosperity_log_callback callback, void* user) {
  auto& logs = Logs();
  {
  std::lock_guard lock(logs.mutex);
  const auto path = std::filesystem::u8path(directory) / L"prosperity.log";
  CREATEFILE2_EXTENDED_PARAMETERS options{}; options.dwSize = sizeof(options);
  options.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  HANDLE file = CreateFile2FromAppW(path.c_str(), FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, &options);
  if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open LocalFolder/prosperity.log");
  if (logs.file != INVALID_HANDLE_VALUE) CloseHandle(logs.file);
  logs.file = file; logs.callback = callback; logs.user = user;
  }
  SDL_GetLogOutputFunction(&previous_sdl_log, &previous_sdl_user);
  SDL_SetLogOutputFunction(SdlLog, nullptr);
}
void CloseLogging() {
  SDL_LogOutputFunction current = nullptr; void* user = nullptr;
  SDL_GetLogOutputFunction(&current, &user);
  if (current == SdlLog) SDL_SetLogOutputFunction(previous_sdl_log, previous_sdl_user);
  auto& logs = Logs(); std::lock_guard lock(logs.mutex);
  logs.callback = nullptr; logs.user = nullptr;
  if (logs.file != INVALID_HANDLE_VALUE) { FlushFileBuffers(logs.file); CloseHandle(logs.file); logs.file = INVALID_HANDLE_VALUE; }
}
void WriteHostLog(uint32_t level, const char* message) noexcept {
  try {
    auto& logs = Logs(); std::lock_guard lock(logs.mutex);
    std::string text = message ? message : ""; text += '\n';
    if (logs.file != INVALID_HANDLE_VALUE) {
      size_t offset = 0;
      while (offset < text.size()) {
        DWORD written = 0;
        if (!WriteFile(logs.file, text.data() + offset,
            static_cast<DWORD>(text.size() - offset), &written, nullptr) || !written) break;
        offset += written;
      }
    }
    if (logs.callback && !in_callback) {
      in_callback = true;
      try { logs.callback(logs.user, level, message ? message : ""); } catch (...) {}
      in_callback = false;
    }
  } catch (...) {}
}
}
namespace logger {
base::String FormatLogEntry(const LogEntry& entry) {
  return base::Format("[{}] {}:{} {}", static_cast<unsigned>(entry.log_level),
      entry.function, entry.line_num, entry.message);
}
LogSink* AddLogSink(base::UniquePointer<LogSink> sink) {
  auto& logs = Logs(); std::lock_guard lock(logs.mutex);
  auto* pointer = sink.Get_UseOnlyIfYouKnowWhatYouareDoing();
  logs.sinks.push_back(base::move(sink)); return pointer;
}
LogSink* GetLogSink(base::StringRef name) {
  auto& logs = Logs(); std::lock_guard lock(logs.mutex);
  for (auto& sink : logs.sinks) if (name == base::StringRef(sink->GetName())) return sink.Get_UseOnlyIfYouKnowWhatYouareDoing();
  return nullptr;
}
void AddLogMsg(LogLevel level, u32 line, const char* function, base::String message) {
  LogEntry entry; entry.log_level = level; entry.line_num = line;
  entry.function = function ? function : ""; entry.message = base::move(message);
  const auto text = FormatLogEntry(entry);
  uwp::WriteHostLog(static_cast<uint32_t>(level), text.c_str());
  auto& logs = Logs(); std::lock_guard lock(logs.mutex);
  for (auto& sink : logs.sinks) sink->Write(entry);
}
void CreateLogger(bool) {} // Configured explicitly by the embedding context.
void SilenceLogging() {} // UWP diagnostics retain their file/callback sinks.
void RouteBaseLogging() {
  base::SetLogHandler([](void*, const char* channel, base::LogLevel level, const char* message) {
    FmtLogMsg(static_cast<LogLevel>(level), 0, "base", "[{}] {}", channel ? channel : "?", message ? message : "");
  }, nullptr);
}
}
