#pragma once
// Access to librashader, the library that runs RetroArch shaders: loads librashader.dll
// at run time and gives the table of its functions (libra::api()). Also includes the
// graphics API headers librashader's own header needs.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES // Vulkan functions are looked up at run time, never linked
#endif
#include <vulkan/vulkan.h>

#include <d3d9.h>
#include <d3d11.h>
#include <d3d12.h>
#include <string>

// The librashader runtimes (one per graphics API) whose functions this project calls.
#define LIBRA_RUNTIME_D3D9
#define LIBRA_RUNTIME_D3D11
#define LIBRA_RUNTIME_D3D12
#define LIBRA_RUNTIME_OPENGL
#define LIBRA_RUNTIME_VULKAN
#include "librashader_ld.h"

// librashader, loaded at runtime from a known location (next to the add-on), so a
// missing or mismatched DLL is reported instead of preventing ReShade from loading.
namespace libra
{
// Loads librashader.dll from `dll_path` and checks it is a version this code can use.
// Returns false with `error` set (a message for the user) if it cannot. Does nothing once
// it has succeeded, so it is safe to call repeatedly.
bool load(const std::wstring &dll_path, std::string &error);
// True once load() has succeeded.
bool loaded();
// librashader's functions (librashader_ld.h). Only valid after load() has succeeded.
const libra_instance_t &api();

// Turns a librashader error into text and frees it. Empty string for no error.
std::string take_error(libra_error_t err);

// Makes sure DirectX's shader compiler (dxcompiler.dll and dxil.dll, from Microsoft's
// DirectX Shader Compiler) is loaded, which librashader's Direct3D 12 support needs: it
// compiles every shader with it, and if the DLLs cannot be found the whole game process
// is terminated. Looks next to the add-on (`addon_dir`) first, then where Windows
// normally looks (the game's folder, the system). Returns false with `error` set (a
// message for the user) when they are not available; Direct3D 12 must then not be used.
// Safe to call repeatedly.
bool load_d3d12_compiler(const std::wstring &addon_dir, std::string &error);
}
