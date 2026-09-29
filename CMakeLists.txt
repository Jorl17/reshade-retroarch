cmake_minimum_required(VERSION 3.21)
project(reshade_retroarch LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build for x64 (ReShade add-ons for 64-bit games).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the add-on then needs no Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /permissive- /utf-8 /EHsc /external:W0)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

# Capture shader, compiled to bytecode headers.
find_program(FXC fxc REQUIRED HINTS "$ENV{WindowsSdkVerBinPath}x64" "$ENV{WindowsSdkBinPath}$ENV{WindowsSDKVersion}x64")
set(GEN ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${GEN}/capture_vs.h ${GEN}/capture_ps.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
    COMMAND ${FXC} /nologo /T vs_5_0 /E VSMain /O3 /Fh ${GEN}/capture_vs.h /Vn g_capture_vs ${CMAKE_SOURCE_DIR}/src/capture.hlsl
    COMMAND ${FXC} /nologo /T ps_5_0 /E PSMain /O3 /Fh ${GEN}/capture_ps.h /Vn g_capture_ps ${CMAKE_SOURCE_DIR}/src/capture.hlsl
    DEPENDS ${CMAKE_SOURCE_DIR}/src/capture.hlsl
    VERBATIM)

# Everything that is not ReShade specific; shared by the add-on and the tools.
add_library(rra_core STATIC
    src/grid_detect.cpp
    src/detector.cpp
    src/renderer.cpp
    src/chain.cpp
    src/librashader_api.cpp
    src/discovery.cpp
    src/companion.cpp
    ${GEN}/capture_vs.h
    ${GEN}/capture_ps.h)
target_include_directories(rra_core PUBLIC src ${GEN})
# Third-party headers are "external": their warnings are not ours to fix.
target_include_directories(rra_core SYSTEM PUBLIC third_party/librashader)
target_link_libraries(rra_core PUBLIC d3d11 advapi32)

# The ReShade add-on.
add_library(RetroArchShaders SHARED src/addon.cpp)
set_target_properties(RetroArchShaders PROPERTIES PREFIX "" SUFFIX ".addon64")
target_include_directories(RetroArchShaders SYSTEM PRIVATE third_party/reshade third_party/imgui)
target_link_libraries(RetroArchShaders PRIVATE rra_core)

# Tools and tests.
add_executable(detect_test tools/detect_test.cpp)
target_link_libraries(detect_test PRIVATE rra_core windowscodecs ole32)

add_executable(files_test tools/files_test.cpp)
target_link_libraries(files_test PRIVATE rra_core)

add_executable(params_test tools/params_test.cpp)
target_link_libraries(params_test PRIVATE rra_core)

add_executable(render_png tools/render_png.cpp)
target_link_libraries(render_png PRIVATE rra_core windowscodecs ole32)

add_executable(test_host tools/test_host.cpp)
target_link_libraries(test_host PRIVATE d3d11 windowscodecs ole32 user32)

add_library(TestCapture SHARED tools/test_capture_addon.cpp)
set_target_properties(TestCapture PROPERTIES PREFIX "" SUFFIX ".addon64")
target_include_directories(TestCapture SYSTEM PRIVATE third_party/reshade third_party/imgui)
target_link_libraries(TestCapture PRIVATE windowscodecs ole32)
