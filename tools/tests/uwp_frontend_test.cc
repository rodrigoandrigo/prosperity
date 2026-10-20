// Standalone PC D3D11 readback and concurrent input test. Does not install or
// launch the UWP application and does not boot a guest.
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include "../../Prosperity-UWP/Frontend/frontend_api.h"
#include "../../delta/uwp/gamepad_api.h"
using Microsoft::WRL::ComPtr;
void Check(bool ok, const char* text) {
  if (!ok)
    throw std::runtime_error(text);
}
void PROSPERITY_CALL
Frame(void*, const void*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void Save(ID3D11Device* device,
          ID3D11DeviceContext* context,
          ID3D11Texture2D* image,
          const std::filesystem::path& path) {
  D3D11_TEXTURE2D_DESC d;
  image->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &staging)), "staging");
  context->CopyResource(staging.Get(), image);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
        "readback");
  const auto sample = static_cast<const unsigned char*>(mapped.pData) +
                      (d.Height / 2) * mapped.RowPitch + (d.Width - 10) * 4;
  const bool backgroundDrawn = sample[0] || sample[1] || sample[2];
  BITMAPFILEHEADER header{};
  header.bfType = 0x4d42;
  header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
  header.bfSize = header.bfOffBits + d.Width * d.Height * 4;
  BITMAPINFOHEADER info{};
  info.biSize = sizeof(info);
  info.biWidth = d.Width;
  info.biHeight = -LONG(d.Height);
  info.biPlanes = 1;
  info.biBitCount = 32;
  info.biCompression = BI_RGB;
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<char*>(&header), sizeof(header));
  file.write(reinterpret_cast<char*>(&info), sizeof(info));
  for (UINT y = 0; y < d.Height; ++y)
    file.write(static_cast<char*>(mapped.pData) + y * mapped.RowPitch,
               d.Width * 4);
  context->Unmap(staging.Get(), 0);
  Check(bool(file), "save BMP");
  Check(backgroundDrawn, "procedural/artwork background must render");
}
int wmain(int argc, wchar_t** argv) {
  try {
    Check(argc == 5,
          "usage: test runtime-directory vclibs-directory data-directory "
          "output-directory");
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(argv[1]);
    AddDllDirectory(argv[2]);
    auto dll = LoadLibraryExW(
        (std::filesystem::path(argv[1]) / L"prosperity.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Check(dll != nullptr, "load prosperity.dll");
#define API(name)                                                              \
  auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(dll, #name)); \
  Check(name != nullptr, #name)
    API(prosperity_set_gamepad);
    API(prosperity_get_state);
    API(prosperity_create);
    API(prosperity_destroy);
    API(prosperity_wait);
    API(prosperity_frontend_create);
    API(prosperity_frontend_destroy);
    API(prosperity_frontend_input);
    API(prosperity_frontend_path);
    API(prosperity_frontend_render);
    API(prosperity_frontend_get_info);
#undef API
    std::filesystem::create_directories(argv[4]);
    auto data = std::filesystem::path(argv[3]).u8string();
    prosperity_config config{};
    config.size = sizeof(config);
    config.abi_version = PROSPERITY_ABI_VERSION;
    config.data_directory = data.c_str();
    config.on_frame = Frame;
    prosperity_context* core;
    Check(prosperity_create(&config, &core) == PROSPERITY_OK, "create core");
    Check(prosperity_wait(core, 10000) == PROSPERITY_OK, "initialize core");
    prosperity_frontend* frontend;
    Check(prosperity_frontend_create(core, &frontend) == PROSPERITY_OK,
          "create frontend");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
    auto makeDevice = [&] {
      HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                     flags, nullptr, 0, D3D11_SDK_VERSION,
                                     &device, nullptr, &context);
      if (FAILED(hr)) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                               &context);
      }
      Check(SUCCEEDED(hr), "D3D11 WARP device");
    };
    makeDevice();
    ComPtr<ID3D11Texture2D> image;
    ComPtr<ID3D11RenderTargetView> target;
    auto makeTarget = [&](UINT w, UINT h) {
      target.Reset();
      image.Reset();
      D3D11_TEXTURE2D_DESC d{};
      d.Width = w;
      d.Height = h;
      d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
      d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      d.BindFlags = D3D11_BIND_RENDER_TARGET;
      Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &image)),
            "target texture");
      Check(SUCCEEDED(
                device->CreateRenderTargetView(image.Get(), nullptr, &target)),
            "target view");
    };
    const bool emptyLibrary = !std::filesystem::exists(
        std::filesystem::path(argv[3]) / L"recent-games");
    UINT w = emptyLibrary ? 384 : 1280, h = emptyLibrary ? 216 : 720;
    float logicalWidth = 1280, logicalHeight = 720;
    bool dpiTest = false;
    makeTarget(w, h);
    auto render = [&] {
      const float clear[4] = {0, 0, 0, 1};
      context->ClearRenderTargetView(target.Get(), clear);
      auto actions = prosperity_frontend_render(
          frontend, device.Get(), context.Get(), target.Get(), w, h,
          (dpiTest || emptyLibrary) ? logicalWidth : float(w),
          (dpiTest || emptyLibrary) ? logicalHeight : float(h), 1.f / 60, 0);
      prosperity_frontend_info info{};
      info.size = sizeof(info);
      prosperity_frontend_get_info(frontend, &info);
      if (actions & 16)
        throw std::runtime_error(info.error);
      Check(info.queued_events <= 512 && info.games <= 12 &&
                info.atlas_bytes <= 32u * 1024 * 1024,
            "frontend budget");
      return actions;
    };
    auto key = [&](UINT code, bool down) {
      prosperity_input e{4, code, down ? 1.f : 0.f, 0};
      prosperity_frontend_input(frontend, &e);
    };
    auto settle = [&] {
      if (emptyLibrary) {
        render();
        Sleep(220);
        render();
      } else
        for (int i = 0; i < 24; ++i) {
          render();
          Sleep(15);
        }
    };
    settle();
    Check((render()&128)==0,"system pointer requested in library");
    prosperity_gamepad_state pad{}; pad.size=sizeof(pad); pad.connected=1;
    for(auto buttons : {uint32_t(4), uint32_t(8)}) {
      pad.buttons=buttons; Check(prosperity_set_gamepad(core,&pad)==PROSPERITY_OK,"publish gamepad");
      for(int i=0;i<3;++i) Check((render()&4)==0,"B cannot exit home");
      Check(prosperity_get_state(core)==PROSPERITY_READY,"A cannot start title from home");
      pad.buttons=0; prosperity_set_gamepad(core,&pad); render();
    }
    pad.left_x=1; prosperity_set_gamepad(core,&pad);
    for(int i=0;i<8;++i) render();
    pad.left_x=0; prosperity_set_gamepad(core,&pad); render();
    Save(device.Get(), context.Get(), image.Get(),
         std::filesystem::path(argv[4]) / L"stick-pointer-only.bmp");
    Save(device.Get(), context.Get(), image.Get(),
         std::filesystem::path(argv[4]) / L"home.bmp");
    key('S', true);
    render();
    key('S', false);
    settle();
    Save(device.Get(), context.Get(), image.Get(),
         std::filesystem::path(argv[4]) / L"settings.bmp");
    auto click = [&](float x, float y) {
      uint32_t actions = 0;
      prosperity_input pos{1, 0, x, y};
      prosperity_frontend_input(frontend, &pos);
      actions |= render();
      prosperity_input button{2, 0, 1, 0};
      prosperity_frontend_input(frontend, &button);
      actions |= render();
      button.x = 0;
      prosperity_frontend_input(frontend, &button);
      actions |= render();
      return actions;
    };
    // Toggle a real setting, save, then save a second change over the same
    // file.
    std::string previousSettings;
    for (int save = 0; save < 2; ++save) {
      click(810, 468);
      click(940, 655);
      render();
      const auto settings = std::filesystem::path(argv[3]) / L"settings.txt";
      Check(std::filesystem::exists(settings) &&
                std::filesystem::file_size(settings) > 0,
            "save settings/replacement");
      std::ifstream saved(settings, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(saved)), {});
      Check(text != previousSettings,
            "setting toggle must change saved values");
      previousSettings = text;
    }
    click(900, 524);
    key(VK_DOWN, true);
    render();
    key(VK_DOWN, false);
    render();
    key(VK_DOWN, true);
    render();
    key(VK_DOWN, false);
    render();
    key(VK_RETURN, true);
    render();
    key(VK_RETURN, false);
    render();
    click(940, 655);
    Check((render() & 8) != 0, "saved vertical sync off must reach the host");
    click(540, 197);
    settle();
    Save(device.Get(), context.Get(), image.Get(),
         std::filesystem::path(argv[4]) / L"appearance.bmp");
    click(730, 197);
    settle();
    Save(device.Get(), context.Get(), image.Get(),
         std::filesystem::path(argv[4]) / L"controls.bmp");
    click(920, 197);
    settle();
    Save(device.Get(), context.Get(), image.Get(),
         std::filesystem::path(argv[4]) / L"advanced.bmp");
    key(VK_ESCAPE, true);
    render();
    key(VK_ESCAPE, false);
    render();
    key('O', true);
    Check((render() & 1) != 0, "file picker request");
    key('O', false);
    render();
    prosperity_frontend_path(frontend, nullptr);
    render();
    Check((click(1100, 42) & 2) != 0, "folder picker request");
    prosperity_frontend_path(frontend, nullptr);
    render();
    if(!emptyLibrary) {
      const auto dir=std::filesystem::path(argv[3]);
      Check(std::filesystem::is_directory(dir/"modules/ps4") && std::filesystem::is_directory(dir/"modules/ps5"), "module directories created at startup");
      { std::ofstream keep(dir/"modules/ps4/keep.sprx",std::ios::binary); keep<<"preserve existing module"; }
      auto import=[&](bool ps5,const char* archive,const char* expected) {
        Save(device.Get(), context.Get(), image.Get(), std::filesystem::path(argv[4])/L"before-import.bmp");
        auto requested=click(1100,ps5?501:457); std::cout << "module action=" << requested << " archive=" << archive << std::endl;
        Check((requested&(ps5?64:32))!=0,"module picker action");
        auto name=(dir/archive).u8string(); prosperity_frontend_path(frontend,name.c_str());
        for(int i=0;i<200;++i) { render(); Sleep(10); if(std::filesystem::exists(dir/(ps5?"modules/ps5/test_module.sprx":"modules/ps4/test_module.sprx")) && !std::filesystem::exists(dir/(ps5?"modules/ps5.importing":"modules/ps4.importing"))) break; }
        settle();
        if(expected) {
          std::ifstream file(dir/(ps5?"modules/ps5/test_module.sprx":"modules/ps4/test_module.sprx"),std::ios::binary);
          std::string content((std::istreambuf_iterator<char>(file)),{});
          Save(device.Get(), context.Get(), image.Get(), std::filesystem::path(argv[4])/L"after-import.bmp");
          Check(content==expected,"exact module extraction bytes");
        }
      };
      import(false,"modules.zip","ZIP module fixture");
      import(true,"modules.rar","RAR module fixture");
      { std::ifstream keep(dir/"modules/ps4/keep.sprx",std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(keep)),{}); Check(bytes=="preserve existing module","existing modules preserved during import"); }
      import(false,"unsafe.zip","ZIP module fixture");
      Check(!std::filesystem::exists(dir/"modules/escape.sprx") && !std::filesystem::exists(dir/"escape.sprx"),"archive traversal blocked");
      import(false,"duplicate.zip","ZIP module fixture");
      Check(!std::filesystem::exists(dir/"modules/ps4/same.sprx"),"duplicate basenames rejected without replacing modules");
      Check((click(1100,457)&32)!=0,"module picker cancellation action");
      prosperity_frontend_path(frontend,nullptr); render();
      click(1100,545); render();
      prosperity_frontend_info removed{}; removed.size=sizeof(removed); prosperity_frontend_get_info(frontend,&removed);
      Check(removed.games==11,"remove selected library entry");
      { auto expected=(dir/"fixture-02.elf").generic_u8string(); std::string hex; const char* digits="0123456789abcdef"; for(unsigned char c:expected) {hex+=digits[c>>4];hex+=digits[c&15];}
        std::ifstream index(dir/"recent-games",std::ios::binary); std::string line; std::getline(index,line);
        Check(line.rfind(hex+"\t",0)==0,"left stick must not change selected game"); }
      Check(std::filesystem::exists(dir/"fixture-01.elf"),"remove preserves game file");
    }
    // Multiple producers emulate keyboard/pointer/focus events arriving while
    // rendering. No producer accesses an ImGuiContext or D3D object.
    std::atomic<int> done{0};
    std::thread producers[3];
    for (int t = 0; t < 3; ++t)
      producers[t] = std::thread([&, t] {
        for (UINT i = 0; i < 10000; ++i) {
          prosperity_input e{UINT(i % 2 ? 1 : 4), UINT('A' + t), float(i % 500),
                             float(i % 400)};
          prosperity_frontend_input(frontend, &e);
        }
        ++done;
      });
    while (done != 3)
      render();
    for (auto& producer : producers)
      producer.join();
    prosperity_input focus{0, 0, 0, 0};
    prosperity_frontend_input(frontend, &focus);
    render();
    prosperity_frontend_info focusedInfo{};
    focusedInfo.size = sizeof(focusedInfo);
    prosperity_frontend_get_info(frontend, &focusedInfo);
    Check(!focusedInfo.held_keys && !focusedInfo.focused &&
              focusedInfo.input_blocked,
          "focus loss must neutralize input");
    focus.code = 1;
    prosperity_frontend_input(frontend, &focus);
    render();
    // Resize and device replacement repeatedly retire the previous atlas SRV.
    for (int i = 0; i < 8; ++i) {
      w = emptyLibrary ? (i % 2 ? 384 : 256) : (i % 2 ? 1920 : 960);
      h = emptyLibrary ? (i % 2 ? 216 : 144) : (i % 2 ? 1080 : 540);
      makeTarget(w, h);
      render();
      target.Reset();
      image.Reset();
      context->ClearState();
      context->Flush();
      context.Reset();
      device.Reset();
      makeDevice();
      makeTarget(w, h);
      render();
    }
    w = emptyLibrary ? 384 : 1280;
    h = emptyLibrary ? 216 : 720;
    makeTarget(w, h);
    settle();
    Save(device.Get(), context.Get(), image.Get(),
         std::filesystem::path(argv[4]) / L"restored.bmp");
    prosperity_frontend_info info{};
    info.size = sizeof(info);
    prosperity_frontend_get_info(frontend, &info);
    Check(info.games == (std::filesystem::exists(
                             std::filesystem::path(argv[3]) / L"recent-games")
                             ? 11u
                             : 0u),
          "fixture library count");
    Check(info.vertices > 0 && !info.error[0], "nonempty UI draw data");
    std::cout << "PASS: 30000 concurrent events, bounded queue/artwork, 8 "
                 "device replacements, resize, home/settings readbacks; atlas="
              << info.atlas_bytes << " games=" << info.games
              << " vertices=" << info.vertices << "\n";
    ComPtr<ID3D11InfoQueue> debug;
    if (SUCCEEDED(device.As(&debug))) {
      for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        debug->GetMessage(i, nullptr, &size);
        std::string bytes(size, '\0');
        auto message = reinterpret_cast<D3D11_MESSAGE*>(&bytes[0]);
        debug->GetMessage(i, message, &size);
        Check(message->Severity != D3D11_MESSAGE_SEVERITY_CORRUPTION &&
                  message->Severity != D3D11_MESSAGE_SEVERITY_ERROR,
              message->pDescription);
      }
      std::cout << "PASS: D3D11 debug layer has no errors\n";
    }
    w = emptyLibrary ? 576 : 1920;
    h = emptyLibrary ? 324 : 1080;
    dpiTest = true;
    makeTarget(w, h);
    render();
    Save(device.Get(), context.Get(), image.Get(),
         std::filesystem::path(argv[4]) / L"dpi150.bmp");
    if(!emptyLibrary) {
      dpiTest=false; w=1280; h=720; makeTarget(w,h); render();
      for(int i=0;i<11;++i) { click(1100,545); render(); }
      prosperity_frontend_info empty{}; empty.size=sizeof(empty); prosperity_frontend_get_info(frontend,&empty);
      Check(empty.games==0,"remove final game without stale selection");
      Check(std::filesystem::file_size(std::filesystem::path(argv[3])/"recent-games")==0,"empty library persists");
      Check(std::filesystem::exists(std::filesystem::path(argv[3])/"fixture-12.elf"),"game files survive clearing library");
      Save(device.Get(),context.Get(),image.Get(),std::filesystem::path(argv[4])/L"removed-all.bmp");
    }
    prosperity_frontend_destroy(frontend);
    Check(prosperity_destroy(core) == PROSPERITY_OK, "destroy core");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << "\n";
    return 1;
  }
}
