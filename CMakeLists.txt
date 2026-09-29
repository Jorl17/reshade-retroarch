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

# The capture shader and preset (src/capture.slang, src/capture.slangp), built into the
# program as text in a generated header (see src/capture.h). CMake re-runs when they change.
set(GEN ${CMAKE_BINARY_DIR}/generated)
file(READ ${CMAKE_SOURCE_DIR}/src/capture.slang CAPTURE_SLANG)
file(READ ${CMAKE_SOURCE_DIR}/src/capture.slangp CAPTURE_SLANGP)
file(READ ${CMAKE_SOURCE_DIR}/src/capture_sm3.slang CAPTURE_SM3_SLANG)
configure_file(${CMAKE_SOURCE_DIR}/src/capture_slang.h.in ${GEN}/capture_slang.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_SOURCE_DIR}/src/capture.slang ${CMAKE_SOURCE_DIR}/src/capture.slangp
             ${CMAKE_SOURCE_DIR}/src/capture_sm3.slang)

# Everything that is not ReShade specific; shared by the add-on and the tools.
add_library(rra_core STATIC
    src/grid_detect.cpp
    src/renderer.cpp
    src/chain.cpp
    src/librashader_api.cpp
    src/discovery.cpp
    src/companion.cpp
    src/capture.cpp)
target_include_directories(rra_core PUBLIC src ${GEN})
# Third-party headers are "external": their warnings are not ours to fix.
target_include_directories(rra_core SYSTEM PUBLIC third_party/librashader)
target_link_libraries(rra_core PUBLIC d3d11 advapi32)

# The ReShade add-on. Its renderer and grid detector work through ReShade's API.
add_library(RetroArchShaders SHARED src/addon.cpp src/frame_renderer.cpp src/detector.cpp)
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

# Test host: a stand-in game that draws identical frames with every graphics API.
add_executable(test_host testhost/main.cpp testhost/d3d9.cpp testhost/d3d10.cpp testhost/d3d11.cpp
               testhost/d3d12.cpp testhost/opengl.cpp testhost/vulkan.cpp)
target_include_directories(test_host SYSTEM PRIVATE third_party/vulkan)
target_link_libraries(test_host PRIVATE d3d9 d3d10 d3d11 d3d12 dxgi opengl32 gdi32 windowscodecs ole32 user32 delayimp)
# Load only the API in use, like a real game (and so ReShade sees only that API).
target_link_options(test_host PRIVATE /DELAYLOAD:d3d9.dll /DELAYLOAD:d3d10.dll /DELAYLOAD:d3d11.dll
                    /DELAYLOAD:d3d12.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:opengl32.dll)

add_library(TestCapture SHARED tools/test_capture_addon.cpp)
set_target_properties(TestCapture PROPERTIES PREFIX "" SUFFIX ".addon64")
target_include_directories(TestCapture SYSTEM PRIVATE third_party/reshade third_party/imgui)
target_link_libraries(TestCapture PRIVATE windowscodecs ole32)
