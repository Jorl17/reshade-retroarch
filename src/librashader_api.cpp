// Implementation of librashader_api.h: loading librashader.dll (and, for Direct3D 12,
// the DirectX shader compiler it needs) at run time.

#include "librashader_api.h"

#include <mutex>

namespace
{
std::mutex g_mutex;
libra_instance_t g_api = {};
bool g_loaded = false;
}

bool libra::load(const std::wstring &dll_path, std::string &error)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_loaded)
        return true;

    // librashader_load_instance() calls LoadLibraryW(L"librashader.dll"). Loading our
    // copy by full path first makes that call return this module rather than
    // whatever the DLL search order would find.
    if (LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr)
    {
        error = "librashader.dll not found next to the add-on (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    g_api = librashader_load_instance();
    if (!g_api.instance_loaded)
    {
        error = "librashader.dll could not be loaded (ABI " + std::to_string(g_api.instance_abi_version()) +
                ", expected " + std::to_string(LIBRASHADER_CURRENT_ABI) + ")";
        return false;
    }
    if (g_api.instance_api_version() < LIBRASHADER_CURRENT_VERSION)
    {
        error = "librashader.dll is too old (API " + std::to_string(g_api.instance_api_version()) + ", need " +
                std::to_string(LIBRASHADER_CURRENT_VERSION) + ")";
        return false;
    }
    g_loaded = true;
    return true;
}

bool libra::loaded()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_loaded;
}

const libra_instance_t &libra::api()
{
    return g_api;
}

std::string libra::take_error(libra_error_t err)
{
    if (err == nullptr)
        return {};
    char *msg = nullptr;
    std::string text = "librashader error";
    if (g_api.error_write(err, &msg) == 0 && msg != nullptr)
    {
        text = msg;
        g_api.error_free_string(&msg);
    }
    g_api.error_free(&err);
    return text;
}

bool libra::load_d3d12_compiler(const std::wstring &addon_dir, std::string &error)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    static bool loaded = false;
    if (loaded)
        return true;
    // dxil.dll first: dxcompiler.dll loads it by name, which then finds this copy.
    for (const wchar_t *name : {L"dxil.dll", L"dxcompiler.dll"})
    {
        const std::wstring next_to_addon = addon_dir + L"\\" + name;
        if (GetModuleHandleW(name) == nullptr &&
            LoadLibraryExW(next_to_addon.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr &&
            LoadLibraryW(name) == nullptr)
        {
            error = "Direct3D 12 needs dxcompiler.dll and dxil.dll (Microsoft's DirectX Shader Compiler) next to "
                    "RetroArchShaders.addon64; they were not found.";
            return false;
        }
    }
    loaded = true;
    return true;
}
