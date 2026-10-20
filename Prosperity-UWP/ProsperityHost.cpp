#include "pch.h"
#include "ProsperityHost.h"
#include "StartupLog.h"
#include "BrokeredStorage.h"
#include "Common/DirectXHelper.h"
#include <algorithm>
#include <cstring>

namespace {
std::string Utf8(Platform::String^ value) {
  if (!value || !value->Length()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      value->Data(), value->Length(), nullptr, 0, nullptr, nullptr);
  std::string result(length, 0);
  if (length) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      value->Data(), value->Length(), &result[0], length, nullptr, nullptr);
  return result;
}
}
namespace Prosperity_UWP {
ProsperityHost::ProsperityHost() : m_frame(new Frame) {}
ProsperityHost::~ProsperityHost() {
  m_gamepad.Stop();
  if (m_frontend) { m_frontendDestroy(m_frontend); m_frontend = nullptr; }
  if (m_context) {
    m_stop(m_context);
    const auto context = m_context;
    const auto destroy = m_destroy;
    auto frame = m_frame;
    // Joining emulator workers is explicit background work, never UI teardown.
    try {
      Windows::System::Threading::ThreadPool::RunAsync(ref new Windows::System::Threading::WorkItemHandler(
          [context, destroy, frame](Windows::Foundation::IAsyncAction^) {
            if (destroy(context) != PROSPERITY_OK) {
              static std::shared_ptr<Frame> retained;
              retained = frame;
            }
          }));
    } catch (...) {
      // Retain callback storage if the OS cannot schedule cleanup. Joining on
      // this thread would block UI teardown; dropping storage would be unsafe.
      static std::shared_ptr<Frame> retained;
      retained = frame;
      OutputDebugStringW(L"Prosperity: não foi possível agendar a limpeza do núcleo.\n");
    }
    m_context = nullptr;
  }
  // The core owns process-wide logging and option registries. Its code stays
  // resident until process exit even after the per-title context is destroyed.
  if (m_library && !m_core_started) FreeLibrary(m_library);
}
bool ProsperityHost::Initialize() {
  if (m_context) return true;
  if (!m_library) m_library = LoadPackagedLibrary(L"prosperity.dll", 0);
  if (!m_library) {
    m_status = L"prosperity.dll não pôde ser carregada. Erro " + std::to_wstring(GetLastError());
    return false;
  }
#define RESOLVE(name, member) member = reinterpret_cast<decltype(member)>(GetProcAddress(m_library, name))
  RESOLVE("prosperity_mount_storage", m_mountStorage);
  RESOLVE("prosperity_set_gamepad", m_setGamepad);
  RESOLVE("prosperity_get_vibration", m_getVibration);
  RESOLVE("prosperity_configure_memory", m_configureMemory);
  RESOLVE("prosperity_get_abi_version", m_version);
  RESOLVE("prosperity_create", m_create);
  RESOLVE("prosperity_boot", m_boot);
  RESOLVE("prosperity_pause", m_pause);
  RESOLVE("prosperity_stop", m_stop);
  RESOLVE("prosperity_destroy", m_destroy);
  RESOLVE("prosperity_get_last_error", m_error);
  RESOLVE("prosperity_frontend_get_info", m_frontendInfo);
  RESOLVE("prosperity_frontend_create", m_frontendCreate);
  RESOLVE("prosperity_frontend_destroy", m_frontendDestroy);
  RESOLVE("prosperity_frontend_render", m_frontendRender);
  RESOLVE("prosperity_frontend_input", m_frontendInput);
  RESOLVE("prosperity_frontend_path", m_frontendPath);
#undef RESOLVE
  if (!m_configureMemory || !m_setGamepad || !m_getVibration || !m_mountStorage || !m_version || !m_create || !m_boot || !m_pause || !m_stop || !m_destroy || !m_error ||
      !m_frontendInfo || !m_frontendCreate || !m_frontendDestroy || !m_frontendRender || !m_frontendInput || !m_frontendPath ||
      m_version() != PROSPERITY_ABI_VERSION) {
    m_status = L"A ABI de prosperity.dll é incompatível.";
    return false;
  }
  const auto data = Utf8(Windows::Storage::ApplicationData::Current->LocalFolder->Path);
  prosperity_config config{};
  config.size = sizeof(config);
  config.abi_version = PROSPERITY_ABI_VERSION;
  config.data_directory = data.c_str();
  config.user_data = m_frame.get();
  config.on_frame = &OnFrame;
  config.on_log = &OnLog;
  config.on_state = &OnState;
  m_core_started = true;
  const auto result = m_create(&config, &m_context);
  prosperity_memory_policy memory{}; memory.size = sizeof(memory); memory.query = QueryMemoryBudget;
  memory.cap_bytes = 5120ull << 20; memory.reserve_bytes = 256ull << 20;
  if (result == PROSPERITY_OK && m_configureMemory(m_context, &memory) != PROSPERITY_OK) {
    m_status = L"Falha ao configurar o orçamento de memória UWP."; return false;
  }
  if (result == PROSPERITY_OK && !m_gamepad.Start(m_context, m_setGamepad, m_getVibration)) {
    m_status = L"Falha ao iniciar Windows.Gaming.Input."; return false;
  }
  m_status = result == PROSPERITY_OK ? L"" :
      L"Falha ao inicializar prosperity.dll: " + std::to_wstring(result);
  if (result == PROSPERITY_OK && m_frontendCreate(m_context, &m_frontend) != PROSPERITY_OK) {
    m_status = L"Falha ao criar o frontend ImGui.";
    return false;
  }
  return result == PROSPERITY_OK;
}
bool ProsperityHost::Boot(Platform::String^ path) {
  if (!Initialize()) return false;
  const auto utf8 = Utf8(path);
  if (m_boot(m_context, utf8.c_str()) != PROSPERITY_OK) {
    char error[1024]{};
    m_error(m_context, error, sizeof(error));
    const int count = MultiByteToWideChar(CP_UTF8, 0, error, -1, nullptr, 0);
    std::vector<wchar_t> text(count > 0 ? count : 1, 0);
    if (count > 0) MultiByteToWideChar(CP_UTF8, 0, error, -1, text.data(), count);
    m_status = text.data();
    return false;
  }
  m_status.clear();
  return true;
}
void ProsperityHost::Pause(bool paused) {
  if (m_context) m_pause(m_context, paused ? 1 : 0);
}
Platform::String^ ProsperityHost::Status() const {
  if (!m_status.empty()) return ref new Platform::String(m_status.c_str());
  std::lock_guard<std::mutex> lock(m_frame->mutex);
  return ref new Platform::String(m_frame->status.empty() ? L"Inicializando Prosperity..." : m_frame->status.c_str());
}
void PROSPERITY_CALL ProsperityHost::OnLog(void*, uint32_t, const char* message) {
  OutputDebugStringA(message ? message : ""); OutputDebugStringA("\n");
}
void PROSPERITY_CALL ProsperityHost::OnState(void* user, prosperity_state state,
    prosperity_result, const char* message) {
  try {
    auto& frame = *static_cast<Frame*>(user);
    std::wstring text;
    switch (state) {
      case PROSPERITY_READY: text = L"Prosperity pronto."; break;
      case PROSPERITY_STARTING: text = L"Carregando jogo..."; break;
      case PROSPERITY_RUNNING: text = L"Jogo em execução."; break;
      case PROSPERITY_PAUSED: text = L"Jogo pausado."; break;
      case PROSPERITY_STOPPING: text = L"Encerrando jogo..."; break;
      case PROSPERITY_STOPPED: text = L"Jogo encerrado."; break;
      case PROSPERITY_FAILED: text = L"Falha no núcleo: "; break;
      default: text = L"Inicializando Prosperity..."; break;
    }
    if (message && *message) {
      const int count = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0);
      std::vector<wchar_t> wide(count > 0 ? count : 1, 0);
      if (count > 0) MultiByteToWideChar(CP_UTF8, 0, message, -1, wide.data(), count);
      text += wide.data();
    }
    std::lock_guard<std::mutex> lock(frame.mutex); frame.status = std::move(text);
    if (state == PROSPERITY_STARTING || state == PROSPERITY_STOPPED || state == PROSPERITY_FAILED) {
      frame.pixels.clear(); frame.width = frame.height = 0;
    }
  } catch (...) {}
}
void PROSPERITY_CALL ProsperityHost::OnFrame(void* user, const void* pixels,
    uint32_t width, uint32_t height, uint32_t pitch, uint32_t bgra) {
  if (!user || !pixels || !width || !height || width > 16384 || height > 16384 || pitch < width * 4)
    return;
  try {
    auto& frame = *static_cast<Frame*>(user);
    std::lock_guard<std::mutex> lock(frame.mutex);
    const bool firstFrame = frame.pixels.empty();
    frame.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
      auto dst = frame.pixels.data() + static_cast<size_t>(y) * width * 4;
      const auto src = static_cast<const unsigned char*>(pixels) + static_cast<size_t>(y) * pitch;
      std::memcpy(dst, src, static_cast<size_t>(width) * 4);
      if (!bgra) for (uint32_t x = 0; x < width; ++x) std::swap(dst[x * 4], dst[x * 4 + 2]);
    }
    frame.width = width;
    frame.height = height;
    if (firstFrame) StartupLog::Write(L"First emulated game frame received from DLL");
  } catch (...) { OutputDebugStringW(L"Prosperity: não foi possível copiar o quadro.\n"); }
}
void ProsperityHost::Draw(ID2D1DeviceContext2* context) {
  std::lock_guard<std::mutex> lock(m_frame->mutex);
  if (m_frame->pixels.empty()) return;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
  const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
  DX::ThrowIfFailed(context->CreateBitmap(D2D1::SizeU(m_frame->width, m_frame->height),
      m_frame->pixels.data(), m_frame->width * 4, &properties, &bitmap));
  const auto size = context->GetSize();
  const float scale = (std::min)(size.width / m_frame->width, size.height / m_frame->height);
  const float w = m_frame->width * scale, h = m_frame->height * scale;
  context->BeginDraw();
  context->SetTransform(D2D1::Matrix3x2F::Identity());
  context->DrawBitmap(bitmap.Get(), D2D1::RectF((size.width - w) / 2, (size.height - h) / 2,
      (size.width + w) / 2, (size.height + h) / 2));
  const auto result = context->EndDraw();
  if (result != D2DERR_RECREATE_TARGET) DX::ThrowIfFailed(result);
}
}

namespace Prosperity_UWP {
void ProsperityHost::Input(const prosperity_input& input) {
  if (input.type == 0) m_gamepad.Focus(input.code != 0);
  if (m_frontend) m_frontendInput(m_frontend, &input);
}
void ProsperityHost::PickerCompleted(Platform::String^ path) {
  if (m_frontend) { const auto text = Utf8(path); m_frontendPath(m_frontend, text.empty() ? nullptr : text.c_str()); }
}
void ProsperityHost::RenderFrontend(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11RenderTargetView* target, uint32_t width, uint32_t height, float lw, float lh, float dt) {
  if (!m_frontend) return;
  uint32_t hasFrame;
  { std::lock_guard<std::mutex> lock(m_frame->mutex); hasFrame = !m_frame->pixels.empty(); }
  const auto actions = m_frontendRender(m_frontend, device, context, target, width, height, lw, lh, dt, hasFrame);
  if (actions & 16) {
    prosperity_frontend_info info{}; info.size = sizeof(info); m_frontendInfo(m_frontend, &info);
    const int count = MultiByteToWideChar(CP_UTF8, 0, info.error, -1, nullptr, 0);
    std::vector<wchar_t> text(count > 0 ? count : 1, 0);
    if (count > 0) MultiByteToWideChar(CP_UTF8, 0, info.error, -1, text.data(), count);
    { std::lock_guard<std::mutex> lock(m_frame->mutex); m_frame->status = text.data(); }
    m_frontendFailed = true;
  }
  m_immediate = (actions & 8) != 0;
  m_actions.fetch_or(actions & (7 | 32 | 64));
}
}

namespace Prosperity_UWP {
Platform::String^ ProsperityHost::RegisterStorage(Windows::Storage::IStorageItem^ item, Platform::String^ token) {
  std::string path;
  const auto result = MountBrokeredStorage(m_context, m_mountStorage, item, token, path);
  if (result != PROSPERITY_IO_OK) throw ref new Platform::FailureException(L"Não foi possível registrar o acesso brokered ao arquivo/pasta.");
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
  if (!length) throw ref new Platform::InvalidArgumentException();
  std::wstring wide(length, 0); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, &wide[0], length);
  return ref new Platform::String(wide.c_str());
}
}
