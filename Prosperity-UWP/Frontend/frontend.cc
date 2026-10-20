#include <SDL3/SDL.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <future>
#include "ModuleImport.h"
#include <stdexcept>
#include <string>
#include <utility>
#include "backends/imgui_impl_dx11.h"
#include "base/option.h"
#include "base/option_file.h"
#include "frontend_api.h"
#include "guest/pause.h"
#include "host/window.h"
#include "io/file.h"
#include "io/path.h"
#include "main/home_screen.h"
#include "main/recent_games.h"
#include "options/options.h"
#include "options/settings.h"
#include "platform.h"
#include "spirv_hlsl.hpp"
#include "ui/home_background.h"
#include "ui/home_screen.h"
#include "ui/input_sdl.h"
#include "ui/mouse_look.h"
#include "ui/overlay.h"
#include "ui/overlay_log.h"
#include "ui/overlay_theme.h"
#include "ui/pause_menu.h"
#include "ui/settings.h"
#include "ui/shaders/home_background_frag.h"
#include "uwp/host_bridge.h"

using Microsoft::WRL::ComPtr;
namespace ui {
void PauseMenuApplyTitle(const base::String&);
}
struct prosperity_frontend {
  prosperity_context* core;
  std::mutex mutex;
  std::deque<prosperity_input> input;
  std::string path, title;
  bool path_ready = false, picker = false;
  std::atomic<bool> reset_input{false}, requested_focus{true};
  uint32_t actions = 0;
  int module_picker = 0;
  std::future<std::string> importing;
  std::string import_status;
  base::String remove_game;
  ImGuiContext* imgui = nullptr;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11VertexShader> background_vs;
  ComPtr<ID3D11PixelShader> background_ps;
  ComPtr<ID3D11Buffer> background_constants;
  ComPtr<ID3D11BlendState> blend;
  ComPtr<ID3D11RasterizerState> raster;
  ComPtr<ID3D11DepthStencilState> depth;
  base::Vector<ui::HomeGame> games;
  bool initialized = false, home = false, booting = false, stopping = false,
       settings_loaded = false;
  bool pad = false, combo = false, immediate = false, focused = true;
  std::array<bool, 256> keys{}, suppressed{};
  bool was_blocked = true;
  float font_scale = 1;
  uint32_t atlas_bytes = 0, vertices = 0, indices = 0;
  uint32_t width = 0, height = 0;
  std::array<char, 2048> failure{};
};
namespace {
std::atomic<bool> frontend_created{false};
// This pointer is accessed only by the serialized render owner. Platform event
// producers address their explicit frontend handle and only touch its mailbox.
thread_local prosperity_frontend* owner = nullptr;
DELTA_OPTION(const char*, kVsyncUwp, "DELTA_GPU_VSYNC", nullptr);
void Check(HRESULT result) {
  if (FAILED(result))
    throw std::runtime_error("UWP frontend D3D11 resource failure");
}
bool Idle(prosperity_state state) {
  return state == PROSPERITY_READY || state == PROSPERITY_STOPPED ||
         state == PROSPERITY_FAILED;
}
void ReleaseDevice(prosperity_frontend& f) {
  if (f.imgui) {
    ImGui::SetCurrentContext(f.imgui);
    if (ImGui::GetIO().BackendRendererUserData)
      ImGui_ImplDX11_Shutdown();
  }
  f.background_vs.Reset();
  f.background_ps.Reset();
  f.background_constants.Reset();
  f.blend.Reset();
  f.raster.Reset();
  f.depth.Reset();
  f.device.Reset();
}
void ReleaseGraphics(prosperity_frontend& f) {
  ReleaseDevice(f);
  if (f.imgui) {
    ImGui::SetCurrentContext(f.imgui);
    ui::SettingsReset();
    ImGui::DestroyContext(f.imgui);
    f.imgui = nullptr;
  }
  f.initialized = false;
}
void InitGraphics(prosperity_frontend& f,
                  ID3D11Device* device,
                  ID3D11DeviceContext* context,
                  float scale) {
  f.device = device;
  IMGUI_CHECKVERSION();
  if (!f.imgui) {
    f.font_scale = scale;
    f.imgui = ImGui::CreateContext();
    ui::overlay_theme::Apply(scale);
  }
  ImGui::SetCurrentContext(f.imgui);
  auto& io = ImGui::GetIO();
  io.IniFilename = io.LogFilename = nullptr;
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  if (!ImGui_ImplDX11_Init(device, context))
    throw std::runtime_error("ImGui D3D11 initialization failed");
  spirv_cross::CompilerHLSL compiler(
      kHomeBackgroundFrag, sizeof(kHomeBackgroundFrag) / sizeof(uint32_t));
  auto options = compiler.get_hlsl_options();
  options.shader_model = 40;
  compiler.set_hlsl_options(options);
  const auto pixel = compiler.compile();
  const char* vertex =
      "float4 main(uint id:SV_VertexID):SV_Position { return "
      "float4(id==2?3:-1,id==1?-3:1,0,1); }";
  ComPtr<ID3DBlob> vs, ps, error;
  Check(D3DCompile(vertex, strlen(vertex), nullptr, nullptr, nullptr, "main",
                   "vs_4_0", 0, 0, &vs, &error));
  Check(D3DCompile(pixel.data(), pixel.size(), nullptr, nullptr, nullptr,
                   "main", "ps_4_0", 0, 0, &ps, &error));
  Check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                                   nullptr, &f.background_vs));
  Check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
                                  nullptr, &f.background_ps));
  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = 32;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  Check(device->CreateBuffer(&desc, nullptr, &f.background_constants));
  D3D11_BLEND_DESC blend{};
  auto& rt = blend.RenderTarget[0];
  rt.BlendEnable = TRUE;
  rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  rt.BlendOp = D3D11_BLEND_OP_ADD;
  rt.SrcBlendAlpha = D3D11_BLEND_ONE;
  rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  Check(device->CreateBlendState(&blend, &f.blend));
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  Check(device->CreateRasterizerState(&raster, &f.raster));
  D3D11_DEPTH_STENCIL_DESC depth{};
  depth.DepthEnable = FALSE;
  depth.StencilEnable = FALSE;
  Check(device->CreateDepthStencilState(&depth, &f.depth));
}
bool Firmware(bool ps5) {
  char value[32768]{};
  if (auto* option =
          base::FindOption(ps5 ? "DELTA_PS5_MODULES" : "DELTA_PS4_MODULES"))
    option->FormatValue(value, sizeof(value));
  const std::string paths(value);
  bool kernel = false, libc = false;
  for (size_t start = 0; start < paths.size();) {
    const auto end = paths.find(';', start);
    const base::String directory(
        paths.substr(start, end == std::string::npos ? end : end - start)
            .c_str());
    auto has = [&](const char* name) {
      for (const char* ext : {".sprx", ".native.sprx"}) {
        if (!ps5 && ext[1] == 'n')
          continue;
        io::File file(directory + "/" + name + ext, io::FileMode::kRead);
        unsigned char header[20]{};
        if (file.IsOpen() &&
            file.Read(header, sizeof(header)) == sizeof(header) &&
            header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' &&
            header[3] == 'F' && header[4] == 2 && header[5] == 1 &&
            header[18] == 62 && header[19] == 0)
          return true;
      }
      return false;
    };
    kernel |= has("libkernel");
    libc |= has("libSceLibcInternal");
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return kernel && libc;
}

void Home(prosperity_frontend& f) {
  ui::PauseMenuReset();
  // Exactly one bounded font/artwork atlas, rather than a descriptor per cover.
  // Recreate it on library changes, never append artwork to a previous atlas.
  if (ImGui::GetIO().BackendRendererUserData)
    ImGui_ImplDX11_InvalidateDeviceObjects();
  ImGui::GetIO().Fonts->Clear();
  ui::overlay_theme::Apply(f.font_scale);
  f.games = cli::ReadRecentGames();
  if (f.games.size() > 12)
    f.games.resize(12);
  ui::BeginHomeScreen(f.games, Firmware(false), Firmware(true));
  ui::HomeScreenSetBackground(options::ReadSettings().background);
  base::Vector<base::String> devices;
  ComPtr<IDXGIFactory4> factory;
  if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
          !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
          SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                      __uuidof(ID3D12Device), nullptr))) {
        base::String name;
        for (const WCHAR* c = desc.Description; *c; ++c)
          name += *c < 128 ? char(*c) : '?';
        devices.push_back(base::move(name));
      }
      adapter.Reset();
    }
  }
  ui::ConfigureSettings({"d3d12"}, devices);
  if (!f.settings_loaded) {
    f.immediate = options::ReadSettings().vsync == "0";
    f.settings_loaded = true;
  }
  unsigned char* pixels;
  int w, h;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
  f.atlas_bytes = uint32_t(w) * uint32_t(h) * 4;
  if (uint64_t(w) * h * 4 > (32u << 20))
    throw std::runtime_error("Artwork atlas exceeded 32 MiB budget");
  if (!ImGui_ImplDX11_CreateDeviceObjects())
    throw std::runtime_error("Could not upload artwork atlas");
  // CPU compressed covers are no longer needed after atlas construction.
  for (auto& game : f.games) {
    game.icon = {};
    game.artwork = {};
  }
  f.home = true;
  f.booting = f.stopping = false;
  uwp::SetFrontendInput(true, {});
}
ImGuiKey Key(uint32_t key) {
  if (key >= 'A' && key <= 'Z')
    return ImGuiKey(ImGuiKey_A + key - 'A');
  if (key >= '0' && key <= '9')
    return ImGuiKey(ImGuiKey_0 + key - '0');
  if (key >= 112 && key <= 123)
    return ImGuiKey(ImGuiKey_F1 + key - 112);
  switch (key) {
    case 9:
      return ImGuiKey_Tab;
    case 13:
      return ImGuiKey_Enter;
    case 27:
      return ImGuiKey_Escape;
    case 32:
      return ImGuiKey_Space;
    case 37:
      return ImGuiKey_LeftArrow;
    case 38:
      return ImGuiKey_UpArrow;
    case 39:
      return ImGuiKey_RightArrow;
    case 40:
      return ImGuiKey_DownArrow;
    case 8:
      return ImGuiKey_Backspace;
    case 46:
      return ImGuiKey_Delete;
    case 17:
    case 162:
      return ImGuiKey_LeftCtrl;
    case 163:
      return ImGuiKey_RightCtrl;
    case 16:
    case 160:
      return ImGuiKey_LeftShift;
    case 161:
      return ImGuiKey_RightShift;
    case 18:
    case 164:
      return ImGuiKey_LeftAlt;
    case 165:
      return ImGuiKey_RightAlt;
    default:
      return ImGuiKey_None;
  }
}
void Events(prosperity_frontend& f) {
  std::deque<prosperity_input> events;
  bool reset;
  std::string title;
  {
    std::lock_guard lock(f.mutex);
    events.swap(f.input);
    reset = f.reset_input.exchange(false);
    title.swap(f.title);
  }
  auto& io = ImGui::GetIO();
  if (reset) {
    events.clear();
    f.keys.fill(false);
    f.suppressed.fill(false);
    io.ClearInputKeys();
    for (int button = 0; button < 5; ++button)
      io.AddMouseButtonEvent(button, false);
  }
  if (!title.empty())
    ui::PauseMenuApplyTitle(title.c_str());
  for (auto e : events) {
    if (e.type != 0)
      f.pad = false;
    switch (e.type) {
      case 0:
        f.focused = e.code != 0;
        io.AddFocusEvent(f.focused);
        if (!e.code) {
          f.keys.fill(false);
          // Foreground is an independent pause reason in the core worker.
          // Do not latch the user pause flag when focus is temporarily lost.
          io.ClearInputKeys();
          for (int button = 0; button < 5; ++button)
            io.AddMouseButtonEvent(button, false);
        }
        break;
      case 1:
        io.AddMousePosEvent(e.x, e.y);
        break;
      case 2:
        if (e.code < 5)
          io.AddMouseButtonEvent(e.code, e.x != 0);
        break;
      case 3:
        io.AddMouseWheelEvent(e.x, e.y);
        break;
      case 4:
        if (e.code < f.keys.size()) {
          f.keys[e.code] = e.x != 0;
          if (!f.keys[e.code])
            f.suppressed[e.code] = false;
        }
        if (auto key = Key(e.code); key != ImGuiKey_None)
          io.AddKeyEvent(key, e.x != 0);
        break;
      case 5:
        if (e.code <= 0x10ffff)
          io.AddInputCharacter(e.code);
        break;
    }
  }
  // Focus is also a sticky mailbox value, so an overflow cannot discard its
  // final transition and leave the UI believing the window is unfocused.
  const bool focused = f.requested_focus.load();
  if (focused != f.focused) {
    f.focused = focused;
    io.AddFocusEvent(focused);
  }
  if (!focused)
    f.keys.fill(false);
}
void Pad(prosperity_frontend& f) {
  host::PadKeys p{};
  if (f.focused)
    uwp::PollPhysicalPad(p);
  if (p.cross || p.circle || p.square || p.triangle || p.options ||
      p.touchpad || p.l1 || p.r1 || p.l2 || p.r2 || p.l3 || p.r3 || p.up ||
      p.down || p.left || p.right || p.lx != 128 || p.ly != 128 ||
      p.rx != 128 || p.ry != 128)
    f.pad = true;
  auto& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  auto add = [&](ImGuiKey key, bool down) {
    io.AddKeyEvent(key, down);
    if (down)
      f.pad = true;
  };
  // The home and pause screens intentionally use the desktop shortcut keys.
  add(ImGuiKey_GamepadFaceDown, p.cross);
  add(ImGuiKey_GamepadFaceRight, p.circle);
  add(ImGuiKey_GamepadDpadLeft, p.left);
  add(ImGuiKey_GamepadDpadRight, p.right);
  add(ImGuiKey_GamepadDpadUp, p.up);
  add(ImGuiKey_GamepadDpadDown, p.down);
  if (f.pad) {
    add(ImGuiKey_LeftArrow, p.left || p.lx < 70);
    add(ImGuiKey_RightArrow, p.right || p.lx > 186);
    add(ImGuiKey_UpArrow, p.up || p.ly < 70);
    add(ImGuiKey_DownArrow, p.down || p.ly > 186);
    add(ImGuiKey_Enter, p.cross);
    add(ImGuiKey_Escape, p.circle);
    add(ImGuiKey_O, p.triangle);
    add(ImGuiKey_S, p.options && !p.touchpad);
  }
  const bool combo = p.options && p.touchpad;
  if (combo && !f.combo)
    ui::PauseMenuToggle();
  f.combo = combo;
}
void Background(prosperity_frontend& f,
                ID3D11DeviceContext* context,
                uint32_t w,
                uint32_t h) {
  const auto b = ui::HomeScreenBackground();
  if (!b.visible)
    return;
  const float constants[8] = {float(w), float(h),  b.time, float(b.style),
                              b.pulse,  b.opacity, 0,      0};
  context->UpdateSubresource(f.background_constants.Get(), 0, nullptr,
                             constants, 0, 0);
  auto buffer = f.background_constants.Get();
  context->PSSetConstantBuffers(0, 1, &buffer);
  context->VSSetShader(f.background_vs.Get(), nullptr, 0);
  context->PSSetShader(f.background_ps.Get(), nullptr, 0);
  context->GSSetShader(nullptr, nullptr, 0);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->RSSetState(f.raster.Get());
  context->OMSetDepthStencilState(f.depth.Get(), 0);
  context->OMSetBlendState(f.blend.Get(), nullptr, UINT32_MAX);
  context->Draw(3, 0);
}
}  // namespace
namespace uwp::frontend {
void SetPaused(bool paused) {
  if (owner)
    prosperity_pause(owner->core, paused ? 1 : 0);
}
void SetPresentationMode(bool immediate) {
  if (owner)
    owner->immediate = immediate;
}
void OpenPicker(bool folder) {
  if (owner && !owner->picker) {
    owner->picker = true;
    owner->actions |= folder ? 2 : 1;
  }
}
bool PickerPending() {
  return owner && owner->picker;
}
base::String TakePickerPath() {
  if (!owner)
    return {};
  std::lock_guard lock(owner->mutex);
  if (!owner->path_ready)
    return {};
  owner->path_ready = false;
  owner->picker = false;
  base::String result(owner->path.c_str());
  owner->path.clear();
  return result;
}
// Launcher invokes this from its worker. No ImGui or screen state is touched.
std::mutex title_mutex;
std::string pending_title;
void QueueTitle(const base::String& title) {
  std::lock_guard lock(title_mutex);
  pending_title = title.c_str();
}
}  // namespace uwp::frontend
namespace ui {
bool UsingGamepad() {
  return owner && owner->pad;
}
const char* GamepadButtonName(int button) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
      return "A";
    case SDL_GAMEPAD_BUTTON_EAST:
      return "B";
    case SDL_GAMEPAD_BUTTON_NORTH:
      return "Y";
    case SDL_GAMEPAD_BUTTON_START:
      return "Menu";
    default:
      return "X";
  }
}
const char* MouseLookNotice() {
  return nullptr;
}
MouseLookSettings GetMouseLookSettings() {
  return {};
}
void ConfigureMouseLook(MouseLookSettings) {
}  // Desktop cursor capture is excluded from UWP settings.
}  // namespace ui
prosperity_result PROSPERITY_CALL
prosperity_frontend_create(prosperity_context* core,
                           prosperity_frontend** out) {
  if (!out || !core)
    return PROSPERITY_INVALID_ARGUMENT;
  *out = nullptr;
  bool expected = false;
  if (!frontend_created.compare_exchange_strong(expected, true))
    return PROSPERITY_INVALID_STATE;
  try {
    *out = new prosperity_frontend;
    (*out)->core = core;
    uwp::SetForeground(true);
    return PROSPERITY_OK;
  } catch (...) {
    frontend_created = false;
    return PROSPERITY_INTERNAL_ERROR;
  }
}
void PROSPERITY_CALL prosperity_frontend_input(prosperity_frontend* f,
                                               const prosperity_input* e) {
  if (!f || !e || e->type > 5 || !std::isfinite(e->x) || !std::isfinite(e->y))
    return;
  if (e->type == 0) {
    f->requested_focus = e->code != 0;
    uwp::SetForeground(e->code != 0);
  }
  try {
    std::lock_guard lock(f->mutex);
    if (e->type == 1 && !f->input.empty() && f->input.back().type == 1)
      f->input.back() = *e;
    else {
      if (f->input.size() >= 512) {
        f->input.clear();
        f->reset_input = true;
      }
      f->input.push_back(*e);
    }
  } catch (...) {
    f->reset_input = true;
  }
}
void PROSPERITY_CALL prosperity_frontend_path(prosperity_frontend* f,
                                              const char* path) {
  if (!f)
    return;
  try {
    std::lock_guard lock(f->mutex);
    f->path = path && strlen(path) <= 32768 ? path : "";
    f->path_ready = true;
  } catch (...) {
  }
}
uint32_t PROSPERITY_CALL prosperity_frontend_render(prosperity_frontend* f,
                                                    void* d,
                                                    void* c,
                                                    void* r,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    float lw,
                                                    float lh,
                                                    float dt,
                                                    uint32_t has_frame) {
  if (!f || !d || !c || !r || !width || !height || lw <= 0 || lh <= 0)
    return 0;
  owner = f;
  if (f->failure[0]) {
    owner = nullptr;
    return 16;
  }
  try {
    auto device = static_cast<ID3D11Device*>(d);
    auto context = static_cast<ID3D11DeviceContext*>(c);
    auto state = prosperity_get_state(f->core);
    if (state == PROSPERITY_INITIALIZING) {
      owner = nullptr;
      return 0;
    }
    if (f->device.Get() != device) {
      ReleaseDevice(*f);
      InitGraphics(*f, device, context, width / lw);
    }
    ImGui::SetCurrentContext(f->imgui);
    if (f->module_picker) {
      std::string path; bool ready=false;
      { std::lock_guard lock(f->mutex); if(f->path_ready) { path.swap(f->path); f->path_ready=false; ready=true; } }
      if(ready) {
        const bool ps5=f->module_picker==5; f->module_picker=0; f->picker=false;
        if(!path.empty()) { f->import_status="Extracting modules..."; f->importing=std::async(std::launch::async,[path,ps5]{return uwp::frontend::ImportModules(path,ps5);}); }
      }
    }
    if(f->importing.valid() && f->importing.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
      f->import_status=f->importing.get();
      if(f->home) Home(*f);
    }
    if (!f->initialized) {
      ui::OverlayLogAttach();
      if (Idle(state))
        Home(*f);
      else {
        ImGui_ImplDX11_CreateDeviceObjects();
      }
      f->initialized = true;
    }
    // The embedding host may start a title directly (launch activation).
    // Adopt that session just as a title launched from the home screen.
    if (f->home && !Idle(state)) {
      ui::EndHomeScreen();
      f->home = false;
      f->booting = true;
    }
    if (!f->home && Idle(state) && (f->booting || f->stopping)) {
      char error[2048]{};
      prosperity_get_last_error(f->core, error, sizeof(error));
      Home(*f);
      if (state == PROSPERITY_FAILED)
        ui::HomeScreenSetError(error);
    }
    {
      std::lock_guard lock(uwp::frontend::title_mutex);
      if (!uwp::frontend::pending_title.empty()) {
        ui::PauseMenuApplyTitle(uwp::frontend::pending_title.c_str());
        uwp::frontend::pending_title.clear();
      }
    }
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(lw, lh);
    io.DisplayFramebufferScale = ImVec2(width / lw, height / lh);
    io.DeltaTime = std::clamp(dt, 0.001f, 0.1f);
    Events(*f);
    Pad(*f);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    if (!f->home && has_frame && state != PROSPERITY_STARTING) {
      ui::LaunchTransitionGameReady();
      ui::PauseMenuGameReady();
      if (ImGui::IsKeyPressed(ImGuiKey_LeftCtrl, false) ||
          ImGui::IsKeyPressed(ImGuiKey_RightCtrl, false))
        ui::PauseMenuToggle();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
      ui::OverlayToggle();
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
      ui::OverlayLogToggle();
    if(!f->importing.valid()) ui::OverlayBuildContents(uint32_t(lw), uint32_t(lh));
    if(f->home && !ui::SettingsVisible() && !ui::HomeScreenDone()) {
      ImGui::SetNextWindowPos(ImVec2(lw-20,std::max(110.f,lh-330.f)),ImGuiCond_Always,ImVec2(1,0));
      ImGui::SetNextWindowSize(ImVec2(290,0));
      ImGui::Begin("Library and modules",nullptr,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::BeginDisabled(f->picker || f->importing.valid());
      if(ImGui::Button("Import PS4 modules (RAR/ZIP)",ImVec2(-1,32))) { f->picker=true; f->module_picker=4; f->actions|=32; }
      if(ImGui::Button("Import PS5 modules (RAR/ZIP)",ImVec2(-1,32))) { f->picker=true; f->module_picker=5; f->actions|=64; }
      auto selected=ui::HomeScreenSelectedPath();
      ImGui::BeginDisabled(selected.empty());
      if(ImGui::Button("Remove selected game",ImVec2(-1,32))) f->remove_game=selected;
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      ImGui::TextWrapped("Removing a game only removes its library entry.");
      if(!f->import_status.empty()) ImGui::TextWrapped("%s",f->import_status.c_str());
      ImGui::End();
    }
    ImGui::Render();
    f->vertices = ImGui::GetDrawData()->TotalVtxCount;
    f->indices = ImGui::GetDrawData()->TotalIdxCount;
    auto target = static_cast<ID3D11RenderTargetView*>(r);
    context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    Background(*f, context, width, height);
    f->width = width;
    f->height = height;
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if(!f->remove_game.empty()) {
      if(cli::ForgetGame(f->remove_game)) { f->remove_game.clear(); Home(*f); }
      else { f->remove_game.clear(); ui::HomeScreenSetError("Could not save the library."); }
    }
    if (f->home) {
      const auto path = ui::TakeHomeScreenAddPath();
      if (!path.empty() && Idle(state)) {
        const auto error = cli::AddHomeGame(path);
        if (error.empty())
          Home(*f);
        else
          ui::HomeScreenSetError(error);
      }
      if (ui::HomeScreenDone()) {
        const auto path = ui::EndHomeScreen();
        if (path.empty())
          f->actions |= 4;
        else if (prosperity_boot(f->core, path.c_str()) == PROSPERITY_OK) {
          f->home = false;
          f->booting = true;
        } else {
          Home(*f);
          ui::HomeScreenSetError("The core is not ready to start a game.");
        }
      }
    } else {
      if (ui::PauseMenuReturnRequested() && !f->stopping) {
        prosperity_request_stop(f->core);
        f->stopping = true;
      }
      if (ui::PauseMenuExitRequested()) {
        prosperity_request_stop(f->core);
        f->actions |= 4;
      }
    }
    const bool blocked = !f->focused || f->home || ui::PauseMenuVisible() ||
                         ui::LaunchTransitionActive();
    if (f->was_blocked && !blocked)
      f->suppressed = f->keys;
    f->was_blocked = blocked;
    host::PadKeys keys{};
    if (f->focused && !f->home && !ui::PauseMenuVisible() &&
        !ui::LaunchTransitionActive()) {
      auto k = f->keys;
      for (size_t i = 0; i < k.size(); ++i)
        k[i] = k[i] && !f->suppressed[i];
      keys.lx = k['A'] ? 0 : k['D'] ? 255 : 128;
      keys.ly = k['W'] ? 0 : k['S'] ? 255 : 128;
      keys.rx = k[37] ? 0 : k[39] ? 255 : 128;
      keys.ry = k[38] ? 0 : k[40] ? 255 : 128;
      keys.cross = k[32];
      keys.circle = k[27] || k[8];
      keys.square = k['F'];
      keys.triangle = k['R'];
      keys.l1 = k['Q'];
      keys.r1 = k['E'];
      keys.l2 = k[160];
      keys.r2 = k[161];
      keys.options = k[13] || k['P'];
      keys.touchpad = k[9];
    }
    uwp::SetFrontendInput(blocked, keys);
    const auto result =
        std::exchange(f->actions, 0u) | (f->immediate ? 8u : 0u);
    owner = nullptr;
    return result;
  } catch (const std::exception& error) {
    std::snprintf(f->failure.data(), f->failure.size(), "%s", error.what());
    OutputDebugStringA(error.what());
  } catch (...) {
    std::snprintf(f->failure.data(), f->failure.size(),
                  "Prosperity frontend failed");
  }
  owner = nullptr;
  return 16;
}
void PROSPERITY_CALL prosperity_frontend_destroy(prosperity_frontend* f) {
  if (!f)
    return;
  uwp::SetForeground(false);
  uwp::SetFrontendInput(false, {});
  ReleaseGraphics(*f);
  delete f;
  frontend_created = false;
}

prosperity_result PROSPERITY_CALL
prosperity_frontend_get_info(prosperity_frontend* f,
                             prosperity_frontend_info* info) {
  if (!f || !info || info->size != sizeof(*info))
    return PROSPERITY_INVALID_ARGUMENT;
  std::lock_guard lock(f->mutex);
  info->queued_events = uint32_t(f->input.size());
  info->atlas_bytes = f->atlas_bytes;
  info->held_keys = uint32_t(std::count(f->keys.begin(), f->keys.end(), true));
  info->focused = f->focused ? 1 : 0;
  info->input_blocked = f->was_blocked ? 1 : 0;
  info->games = uint32_t(f->games.size());
  info->vertices = f->vertices;
  info->indices = f->indices;
  std::snprintf(info->error, sizeof(info->error), "%s", f->failure.data());
  return PROSPERITY_OK;
}
