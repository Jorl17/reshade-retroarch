#pragma once

#include <d3d11.h>
#include <string>

#define LIBRA_RUNTIME_D3D11
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
}
