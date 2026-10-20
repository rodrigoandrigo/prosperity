# Separate AppContainer composition root. Desktop/Linux configuration is unchanged.
if(NOT WIN32 OR NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "DELTA_UWP requires the 64-bit MSYS2 UCRT64 toolchain")
endif()
set(DELTA_UWP_DEPS "${CMAKE_SOURCE_DIR}/build/uwp-deps" CACHE PATH "Prepared dependency source snapshots")
set(DELTA_UWP_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/Prosperity-UWP/Runtime/${CMAKE_BUILD_TYPE}" CACHE PATH "DLL deployment directory")
set(DELTA_SDL_SOURCE "C:/Users/rodri/Dev1/Projetos/SDL3-uwp" CACHE PATH "SDL3 UWP source tree")
set(DELTA_SDL_LIBRARY "" CACHE FILEPATH "SDL3 UWP import library (not desktop SDL)")
set(DELTA_SDL_RUNTIME "" CACHE FILEPATH "SDL3 UWP runtime DLL")
set(DELTA_DXC_DIRECTORY "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" CACHE PATH "DXC and DXIL runtime directory")
foreach(dep equilibrium mbedtls zlib xbyak)
  if(NOT EXISTS "${DELTA_UWP_DEPS}/${dep}/.prosperity-revision")
    message(FATAL_ERROR "Missing verified ${dep} snapshot; run tools/build-uwp.ps1")
  endif()
endforeach()
if(NOT EXISTS "${DELTA_SDL_LIBRARY}" OR NOT EXISTS "${DELTA_SDL_RUNTIME}")
  message(FATAL_ERROR "SDL3 UWP library/runtime required; run tools/build-uwp.ps1")
endif()
foreach(runtime dxcompiler.dll dxil.dll)
  if(NOT EXISTS "${DELTA_DXC_DIRECTORY}/${runtime}")
    message(FATAL_ERROR "Missing shader compiler runtime: ${DELTA_DXC_DIRECTORY}/${runtime}")
  endif()
endforeach()
add_compile_definitions(DELTA_UWP=1 DELTA_BACKEND_NATIVE=1
  DELTA_HAVE_D3D12=1 DELTA_HAVE_SPIRV_BACKEND=1 DELTA_GPU_VULKAN=0
  NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINAPI_FAMILY=WINAPI_FAMILY_APP
  rsc_copyright="Prosperity")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wno-deprecated-declarations -Wno-narrowing -Wno-multichar -Wno-invalid-offsetof)
endif()

set(eq "${DELTA_UWP_DEPS}/equilibrium")
set(eq_sources
  base/logging.cc base/option_file.cc
  base/allocator/memory_error.cc base/containers/init_chain_win.cc
  base/dynamic_library.cc base/win/scoped_handle.cc
  base/strings/float_format.cc base/strings/format.cc base/strings/number_parse.cc
  base/text/code_convert.cc base/random/random.cc
  base/threading/spinning_mutex.cc
  base/threading/mutex_win.cc base/threading/thread_win.cc
  base/threading/condition_variable_win.cc base/time/time.cc base/time/time_win.cc
  base/filesystem/file.cc base/filesystem/path.cc base/filesystem/scoped_file.cc
  base/filesystem/win/file_win.cc base/filesystem/win/path_win.cc)
list(TRANSFORM eq_sources PREPEND "${eq}/")
# Keep dependency worktrees untouched while adding a worker exception boundary.
file(READ "${eq}/base/threading/thread.cc" eq_thread_source)
string(FIND "${eq_thread_source}" "  run_functor_();" eq_thread_entry)
if(eq_thread_entry EQUAL -1)
  message(FATAL_ERROR "equilibrium Thread::Run changed; its embedding exception boundary must be reviewed")
endif()
string(REPLACE "  run_functor_();" "  try { run_functor_(); } catch (const std::exception& error) { embedding::ReportFailure(error.what()); return 1; } catch (...) { embedding::ReportFailure(\"Unhandled host worker error\"); return 1; }" eq_thread_source "${eq_thread_source}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/eq_thread.cc" CONTENT "#include <exception>\n#include \"embedding/host.h\"\n${eq_thread_source}" @ONLY)
list(APPEND eq_sources "${CMAKE_CURRENT_BINARY_DIR}/eq_thread.cc")
add_library(eq_base STATIC ${eq_sources} "${CMAKE_SOURCE_DIR}/delta/uwp/random_win.cc"
  "${CMAKE_SOURCE_DIR}/delta/uwp/environment_win.cc"
  "${CMAKE_SOURCE_DIR}/delta/uwp/dynamic_library_win.cc")
target_include_directories(eq_base PUBLIC "${eq}" PRIVATE "${eq}/base" "${CMAKE_SOURCE_DIR}/shared")
target_compile_definitions(eq_base PUBLIC OS_WIN BASE_STRIP_BUGCHECK CONFIG_SHIPPING PRIVATE PROJECT_NAME="Prosperity")
target_link_libraries(eq_base PUBLIC bcrypt synchronization)
target_compile_options(eq_base PRIVATE -include "${CMAKE_SOURCE_DIR}/delta/uwp/winapi_compat.h")

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${DELTA_UWP_DEPS}/zlib" "deps/zlib" EXCLUDE_FROM_ALL)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory("${DELTA_UWP_DEPS}/mbedtls" "deps/mbedtls" EXCLUDE_FROM_ALL)
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
set(CAPSTONE_X86_SUPPORT ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/capstone EXCLUDE_FROM_ALL)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/SPIRV-Cross EXCLUDE_FROM_ALL)
set(SPIRV-Headers_SOURCE_DIR "${CMAKE_SOURCE_DIR}/vendor/SPIRV-Headers")
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/SPIRV-Tools EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)

file(GLOB_RECURSE core_sources CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/shared/*.cc"
  "${CMAKE_SOURCE_DIR}/delta/cpu/*.cc"
  "${CMAKE_SOURCE_DIR}/delta/formats/*.cc"
  "${CMAKE_SOURCE_DIR}/delta/gpu/*.cc"
  "${CMAKE_SOURCE_DIR}/delta/kern/*.cc"
  "${CMAKE_SOURCE_DIR}/delta/patch/*.cc"
  "${CMAKE_SOURCE_DIR}/delta/runtime/*.cc")
list(FILTER core_sources EXCLUDE REGEX "/tests/|/gpu/vulkan/|/gpu/opengl/|/cpu/fex_backend\\.cc$|_posix\\.cc$")
list(FILTER core_sources EXCLUDE REGEX "/kern/crash\\.cc$|/kern/probe/probe_trap\\.cc$")
list(FILTER core_sources EXCLUDE REGEX "/shared/logger/logger\\.cc$")
file(GLOB atrac_sources "${CMAKE_SOURCE_DIR}/vendor/libatrac9/src/*.c")
set(unrar_sources
  rar.cpp strlist.cpp strfn.cpp pathfn.cpp smallfn.cpp global.cpp file.cpp filcreat.cpp
  filefn.cpp archive.cpp arcread.cpp unicode.cpp system.cpp crypt.cpp crc.cpp rawread.cpp
  encname.cpp resource.cpp match.cpp timefn.cpp rdwrfn.cpp consio.cpp options.cpp errhnd.cpp
  rarvm.cpp secpassword.cpp rijndael.cpp getbits.cpp sha1.cpp sha256.cpp blake2s.cpp hash.cpp
  extinfo.cpp extract.cpp volume.cpp list.cpp find.cpp unpack.cpp headers.cpp threadpool.cpp
  rs16.cpp cmddata.cpp ui.cpp largepage.cpp filestr.cpp scantree.cpp dll.cpp qopen.cpp motw.cpp)
list(TRANSFORM unrar_sources PREPEND "${CMAKE_SOURCE_DIR}/vendor/unrar/")
add_library(uwp_unrar STATIC ${unrar_sources})
target_sources(uwp_unrar PRIVATE delta/uwp/unrar_platform.cc)
target_compile_definitions(uwp_unrar PUBLIC RARDLL RAR_SMP UNICODE _UNICODE)
target_include_directories(uwp_unrar PUBLIC "${CMAKE_SOURCE_DIR}/vendor")
target_compile_options(uwp_unrar PRIVATE -include "${CMAKE_SOURCE_DIR}/delta/uwp/winapi_compat.h")
add_library(SDL3::SDL3 SHARED IMPORTED)
set_target_properties(SDL3::SDL3 PROPERTIES IMPORTED_IMPLIB "${DELTA_SDL_LIBRARY}"
  IMPORTED_LOCATION "${DELTA_SDL_RUNTIME}"
  INTERFACE_INCLUDE_DIRECTORIES "${DELTA_SDL_SOURCE}/include")

add_library(prosperity SHARED ${core_sources} ${atrac_sources}
  delta/uwp/prosperity_api.cc delta/uwp/host_bridge.cc delta/uwp/storage_win.cc
  delta/uwp/host_memory_win.cc delta/uwp/log_sinks_win.cc
  delta/uwp/memory_win.cc delta/uwp/thread_stack_win.cc delta/uwp/signals_win.cc delta/uwp/memory_api.cc delta/uwp/posix_runtime.cc
  delta/uwp/crash_win.cc delta/uwp/probe_win.cc
  delta/uwp/crt_compat.cc
  delta/uwp/embedding_support.cc delta/uwp/check_win.cc
  delta/host/gameplay_state.cc delta/host/controller_input.cc delta/host/audio_output_xaudio2.cc
  delta/ui/shader_activity.cc delta/ui/pause_menu.cc delta/ui/settings.cc
  delta/ui/overlay_perf.cc
  delta/main/launcher.cc delta/main/game_directory.cc delta/main/recent_games.cc
  delta/ui/home_screen.cc delta/ui/overlay_theme.cc delta/ui/image.cc
  delta/ui/overlay.cc delta/ui/overlay_busy.cc delta/ui/overlay_log.cc delta/ui/overlay_memory.cc
  Prosperity-UWP/Frontend/frontend.cc
  Prosperity-UWP/third_party/imgui/imgui.cpp
  Prosperity-UWP/third_party/imgui/imgui_draw.cpp
  Prosperity-UWP/third_party/imgui/imgui_tables.cpp
  Prosperity-UWP/third_party/imgui/imgui_widgets.cpp
  Prosperity-UWP/third_party/imgui/backends/imgui_impl_dx11.cpp)
target_compile_definitions(prosperity PRIVATE PROSPERITY_EXPORTS=1 PROSPERITY_CORE=1 _FILE_OFFSET_BITS=64)
target_include_directories(prosperity BEFORE PRIVATE delta/uwp/include)
target_compile_options(prosperity PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-include>"
  "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/delta/uwp/winapi_compat.h>" -fms-extensions)
target_include_directories(prosperity PRIVATE delta shared
  Prosperity-UWP/Frontend Prosperity-UWP/third_party Prosperity-UWP/third_party/imgui
  vendor/SPIRV-Cross
  "${DELTA_UWP_DEPS}/xbyak/xbyak" "${DELTA_UWP_DEPS}/zlib"
  "${CMAKE_CURRENT_BINARY_DIR}/deps/zlib"
  vendor/SPIRV-Headers/include vendor/bcdec vendor/libatrac9/src)
target_link_libraries(prosperity PRIVATE eq_base zlibstatic mbedcrypto capstone_static
  SPIRV-Tools-opt SPIRV-Tools-static spirv-cross-hlsl
  uwp_unrar SDL3::SDL3 Threads::Threads d3d11 d3dcompiler d3d12 dxgi dxguid bcrypt synchronization xaudio2_9 ole32
  "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64/OneCore.lib"
  "-Wl,--start-group" "-l:libstdc++.a" "-l:libgcc_eh.a" "-l:libwinpthread.a" mingwex mingw32 windowsapp "-Wl,--end-group")
set_target_properties(prosperity PROPERTIES PREFIX "" OUTPUT_NAME "prosperity")
target_link_options(prosperity PRIVATE -static-libgcc -static-libstdc++)
add_custom_command(TARGET prosperity POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${DELTA_UWP_RUNTIME_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:prosperity>" "${DELTA_UWP_RUNTIME_DIR}/prosperity.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DELTA_SDL_RUNTIME}" "${DELTA_UWP_RUNTIME_DIR}/SDL3.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DELTA_DXC_DIRECTORY}/dxcompiler.dll" "${DELTA_UWP_RUNTIME_DIR}/dxcompiler.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DELTA_DXC_DIRECTORY}/dxil.dll" "${DELTA_UWP_RUNTIME_DIR}/dxil.dll"
  VERBATIM)
