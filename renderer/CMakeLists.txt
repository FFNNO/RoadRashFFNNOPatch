cmake_minimum_required(VERSION 3.21)

# Static CRT (/MT) for every target, so the proxy has no VC runtime dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(rr95gpu LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "rr95gpu must be built for 32-bit x86 (use -A Win32 or a vcvars32 environment).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RR95_COMMON_OPTIONS /W4 /WX /permissive- /utf-8)
set(RR95_COMMON_DEFINITIONS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# Proxy ddraw.dll -----------------------------------------------------------
add_library(ddraw SHARED
  src/dllmain.cpp
  src/log.cpp
  src/config.cpp
  src/exe_identity.cpp
  src/hook.cpp
  src/hook_sites.cpp
  src/presenter.cpp
  src/capture.cpp
  src/texture_pack.cpp
  src/save_redirect.cpp
  src/overlay_text.cpp
  src/widescreen.cpp
  src/options_menu.cpp
  src/hud_icons.cpp
  src/ddraw.def
)
set_target_properties(ddraw PROPERTIES OUTPUT_NAME "ddraw" PREFIX "")
target_compile_options(ddraw PRIVATE ${RR95_COMMON_OPTIONS})
target_compile_definitions(ddraw PRIVATE ${RR95_COMMON_DEFINITIONS})
target_link_libraries(ddraw PRIVATE bcrypt d3d11 dxgi d3dcompiler windowscodecs)

# Forwarding test -------------------------------------------------------------
add_executable(forward_test tests/forward_test.cpp)
target_compile_options(forward_test PRIVATE ${RR95_COMMON_OPTIONS})
target_compile_definitions(forward_test PRIVATE ${RR95_COMMON_DEFINITIONS}
  RR95_PROXY_PATH=L"$<TARGET_FILE:ddraw>")
add_dependencies(forward_test ddraw)

# Hook engine, presenter math/latch, config, and capture writers test ---------
add_executable(hook_test
  tests/hook_test.cpp
  src/hook.cpp
  src/log.cpp
  src/config.cpp
  src/presenter.cpp
  src/capture.cpp
  src/texture_pack.cpp
  src/save_redirect.cpp
  src/overlay_text.cpp
  src/widescreen.cpp
  src/options_menu.cpp
  src/hud_icons.cpp
)
target_compile_options(hook_test PRIVATE ${RR95_COMMON_OPTIONS})
target_compile_definitions(hook_test PRIVATE ${RR95_COMMON_DEFINITIONS})
target_link_libraries(hook_test PRIVATE d3d11 dxgi d3dcompiler windowscodecs)
target_link_options(hook_test PRIVATE /INCREMENTAL:NO)

enable_testing()
add_test(NAME forward_test COMMAND forward_test)
add_test(NAME hook_test COMMAND hook_test)
