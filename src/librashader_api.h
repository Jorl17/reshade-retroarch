#pragma once

#include <d3d9.h>
#include <d3d11.h>
#include <d3d12.h>
#include <string>

// The librashader runtimes (one per graphics API) whose functions this project calls.
#define LIBRA_RUNTIME_D3D9
#define LIBRA_RUNTIME_D3D11
#define LIBRA_RUNTIME_D3D12
#define LIBRA_RUNTIME_OPENGL
#include "librashader_ld.h"

// librashader, loaded at runtime from a known location (next to the add-on), so a
// missing or mismatched DLL is reported instead of preventing ReShade from loading.
namespace libra
{
// Loads librashader.dll from `dll_path`. Safe to call repeatedly.
bool load(const std::wstring &dll_path, std::string &error);
bool loaded();
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
