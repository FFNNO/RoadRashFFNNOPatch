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
  src/ddraw.def
)
set_target_properties(ddraw PROPERTIES OUTPUT_NAME "ddraw" PREFIX "")
target_compile_options(ddraw PRIVATE ${RR95_COMMON_OPTIONS})
target_compile_definitions(ddraw PRIVATE ${RR95_COMMON_DEFINITIONS})
target_link_libraries(ddraw PRIVATE bcrypt)

# Forwarding test -------------------------------------------------------------
add_executable(forward_test tests/forward_test.cpp)
target_compile_options(forward_test PRIVATE ${RR95_COMMON_OPTIONS})
target_compile_definitions(forward_test PRIVATE ${RR95_COMMON_DEFINITIONS}
  RR95_PROXY_PATH=L"$<TARGET_FILE:ddraw>")
add_dependencies(forward_test ddraw)

# Hook engine test (in-process, links the engine sources directly) -----------
add_executable(hook_test tests/hook_test.cpp src/hook.cpp src/log.cpp)
target_compile_options(hook_test PRIVATE ${RR95_COMMON_OPTIONS})
target_compile_definitions(hook_test PRIVATE ${RR95_COMMON_DEFINITIONS})

enable_testing()
add_test(NAME forward_test COMMAND forward_test)
add_test(NAME hook_test COMMAND hook_test)
