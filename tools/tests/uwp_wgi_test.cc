#include <windows.h>
#include <roapi.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <stdexcept>
#include "../../Prosperity-UWP/GamepadInput.h"
#include "../../Prosperity-UWP/MemoryBudget.h"
std::atomic<unsigned> snapshots{0}, connected{0};
decltype(&prosperity_set_gamepad) publish;
prosperity_result PROSPERITY_CALL Observe(prosperity_context* context, const prosperity_gamepad_state* state) {
  connected = state->connected; ++snapshots; return publish(context, state);
}
void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class T> T Symbol(HMODULE module, const char* name) {
  auto result = reinterpret_cast<T>(GetProcAddress(module, name)); Check(result != nullptr, name); return result;
}
[Platform::MTAThread]
int wmain(int argc, wchar_t** argv) {
  try {
    Check(argc == 4, "runtime vclibs data");
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(argv[1]); AddDllDirectory(argv[2]);
    auto module = LoadLibraryExW((std::wstring(argv[1]) + L"/prosperity.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Check(module != nullptr, "load DLL");
    auto create = Symbol<decltype(&prosperity_create)>(module, "prosperity_create");
    auto wait = Symbol<decltype(&prosperity_wait)>(module, "prosperity_wait");
    auto destroy = Symbol<decltype(&prosperity_destroy)>(module, "prosperity_destroy");
    publish = Symbol<decltype(publish)>(module, "prosperity_set_gamepad");
    auto vibration = Symbol<decltype(&prosperity_get_vibration)>(module, "prosperity_get_vibration");
    const int size = WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, nullptr, 0, nullptr, nullptr);
    std::string data(size, 0); WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, &data[0], size, nullptr, nullptr);
    prosperity_config config{}; config.size = sizeof(config); config.abi_version = PROSPERITY_ABI_VERSION;
    config.data_directory = data.c_str(); config.on_frame = [](void*, const void*, uint32_t, uint32_t, uint32_t, uint32_t) {};
    prosperity_context* context = nullptr;
    Check(create(&config, &context) == 0 && wait(context, 5000) == 0, "create/wait");
    uint64_t usage = 0, limit = 0;
    int32_t queried = -1;
    std::thread budgetThread([&] { queried = Prosperity_UWP::QueryMemoryBudget(nullptr, &usage, &limit); });
    budgetThread.join();
    Check(queried == 0 && limit, "actual WinRT memory budget on a native worker");
    auto configure = Symbol<decltype(&prosperity_configure_memory)>(module, "prosperity_configure_memory");
    auto getMemory = Symbol<decltype(&prosperity_get_memory_info)>(module, "prosperity_get_memory_info");
    prosperity_memory_policy policy{sizeof(policy), Prosperity_UWP::QueryMemoryBudget, nullptr, 5120ull << 20, 256ull << 20};
    Check(configure(context, &policy) == 0, "configure actual DLL budget callback");
    prosperity_memory_info info{}; info.size = sizeof(info);
    Check(getMemory(context, &info) == 0 && info.chunk_bytes == (16ull << 20) && info.commit_bytes == (64ull << 10), "actual DLL memory policy/info");
    printf("PASS WinRT memory query from native worker: usage=%llu limit=%llu; DLL 16 MiB/64 KiB policy\n", usage, limit);
    const auto physical = Windows::Gaming::Input::Gamepad::Gamepads->Size;
    Prosperity_UWP::GamepadInput input;
    Check(input.Start(context, Observe, vibration), "start actual WGI timer");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check(snapshots.load() >= 4, "independent WGI polling publishes into real DLL");
    input.Focus(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Check(!connected.load(), "focus loss publishes neutral");
    input.Focus(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    input.Stop(); const auto stopped = snapshots.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Check(stopped == snapshots.load(), "no callbacks after stop");
    Check(destroy(context) == 0, "context cleanup after poller stop");
    printf("PASS actual Windows.Gaming.Input -> DLL, %u snapshots, focus/stop lifetime; physical controllers=%u\n", stopped, physical);
    return 0;
  } catch (Platform::Exception^ error) { fprintf(stderr, "WinRT failure %08x\n", error->HResult); return 1; }
  catch (const std::exception& error) { fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
