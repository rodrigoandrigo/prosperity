#pragma once
#include "../delta/uwp/prosperity_api.h"
#include <mutex>
#include <atomic>
#include "Frontend/frontend_api.h"
#include "GamepadInput.h"
#include "MemoryBudget.h"
#include <string>
#include <vector>

namespace Prosperity_UWP {
class ProsperityHost {
 public:
  ProsperityHost();
  ~ProsperityHost();
  bool Initialize();
  bool Boot(Platform::String^ path);
  void Pause(bool paused);
  Platform::String^ Status() const;
  void Draw(ID2D1DeviceContext2* context);
  void RenderFrontend(ID3D11Device*, ID3D11DeviceContext*, ID3D11RenderTargetView*,
      uint32_t width, uint32_t height, float logicalWidth, float logicalHeight, float dt);
  void Input(const prosperity_input& input);
  void PickerCompleted(Platform::String^ path);
  Platform::String^ RegisterStorage(Windows::Storage::IStorageItem^ item, Platform::String^ token);
  uint32_t TakeActions() { return m_actions.exchange(0); }
  bool FrontendAvailable() const { return m_frontend != nullptr && !m_frontendFailed.load(); }
  bool HideSystemPointer() const { return m_hidePointer.load(); }
  bool ImmediatePresentation() const { return m_immediate.load(); }
 private:
  struct Frame {
    std::mutex mutex;
    std::vector<unsigned char> pixels;
    uint32_t width = 0, height = 0;
    std::wstring status;
  };
  static void PROSPERITY_CALL OnFrame(void* user, const void* pixels,
      uint32_t width, uint32_t height, uint32_t pitch, uint32_t bgra);
  static void PROSPERITY_CALL OnLog(void*, uint32_t, const char* message);
  static void PROSPERITY_CALL OnState(void* user, prosperity_state state,
      prosperity_result result, const char* message);
  prosperity_frontend* m_frontend = nullptr;
  decltype(&prosperity_frontend_get_info) m_frontendInfo = nullptr;
  std::atomic<bool> m_frontendFailed{false};
  decltype(&prosperity_frontend_create) m_frontendCreate = nullptr;
  decltype(&prosperity_frontend_destroy) m_frontendDestroy = nullptr;
  decltype(&prosperity_frontend_render) m_frontendRender = nullptr;
  decltype(&prosperity_frontend_input) m_frontendInput = nullptr;
  decltype(&prosperity_frontend_path) m_frontendPath = nullptr;
  std::atomic<uint32_t> m_actions{0};
  std::atomic<bool> m_immediate{false};
  std::atomic<bool> m_hidePointer{false};
  decltype(&prosperity_mount_storage) m_mountStorage = nullptr;
  decltype(&prosperity_set_gamepad) m_setGamepad = nullptr;
  decltype(&prosperity_get_vibration) m_getVibration = nullptr;
  GamepadInput m_gamepad;
  decltype(&prosperity_configure_memory) m_configureMemory = nullptr;
  HMODULE m_library = nullptr;
  prosperity_context* m_context = nullptr;
  decltype(&prosperity_get_abi_version) m_version = nullptr;
  decltype(&prosperity_create) m_create = nullptr;
  decltype(&prosperity_boot) m_boot = nullptr;
  decltype(&prosperity_pause) m_pause = nullptr;
  decltype(&prosperity_stop) m_stop = nullptr;
  decltype(&prosperity_destroy) m_destroy = nullptr;
  decltype(&prosperity_get_last_error) m_error = nullptr;
  std::shared_ptr<Frame> m_frame;
  std::wstring m_status;
  bool m_core_started = false;
};
}
