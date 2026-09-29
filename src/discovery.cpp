#include "discovery.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
bool is_dir(const fs::path &p)
{
    std::error_code ec;
    return fs::is_directory(p, ec);
}

std::wstring reg_string(HKEY root, const wchar_t *key, const wchar_t *value)
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

// Steam library folders, from steamapps/libraryfolders.vdf.
std::vector<fs::path> steam_libraries()
{
    std::vector<fs::path> libs;
    const std::wstring steam = reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty())
        return libs;
    libs.push_back(fs::path(steam));
    std::ifstream vdf(fs::path(steam) / L"steamapps" / L"libraryfolders.vdf");
    std::stringstream text;
    text << vdf.rdbuf();
    const std::string s = text.str();
    static const std::regex path_re("\"path\"\\s+\"([^\"]+)\"");
    for (auto it = std::sregex_iterator(s.begin(), s.end(), path_re); it != std::sregex_iterator(); ++it)
    {
        std::string p = (*it)[1].str();
        // VDF escapes backslashes.
        for (size_t i = p.find("\\\\"); i != std::string::npos; i = p.find("\\\\", i + 1))
            p.erase(i, 1);
        libs.push_back(fs::u8path(p));
    }
    return libs;
}
}

std::vector<ShaderRoot> find_shader_roots(const fs::path &addon_dir, const fs::path &exe_dir,
                                          const std::vector<fs::path> &extra)
{
    std::vector<ShaderRoot> roots;
    auto add = [&](const std::string &name, const fs::path &dir) {
        if (!is_dir(dir))
            return;
        std::error_code ec;
        const fs::path canon = fs::weakly_canonical(dir, ec);
        for (const ShaderRoot &r : roots)
            if (fs::equivalent(r.dir, canon, ec))
                return;
        roots.push_back({name, ec ? dir : canon});
    };

    add("retroarch-shaders", addon_dir / L"retroarch-shaders");
    add("retroarch-shaders", exe_dir / L"retroarch-shaders");

    const fs::path slang = fs::path(L"shaders") / L"shaders_slang";
    add("RetroArch", fs::path(L"C:\\RetroArch-Win64") / slang);
    add("RetroArch", fs::path(L"C:\\RetroArch") / slang);
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0)
        add("RetroArch", fs::path(appdata) / L"RetroArch" / slang);
    for (const fs::path &lib : steam_libraries())
        add("RetroArch (Steam)", lib / L"steamapps" / L"common" / L"RetroArch" / slang);

    for (const fs::path &p : extra)
        add(p.filename().u8string(), p);
    return roots;
}

std::vector<PresetEntry> scan_presets(const std::vector<ShaderRoot> &roots)
{
    std::vector<PresetEntry> out;
    for (const ShaderRoot &root : roots)
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root.dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec))
        {
            if (ec)
                break;
            if (!it->is_regular_file(ec))
                continue;
            fs::path ext = it->path().extension();
            std::wstring e = ext.wstring();
            std::transform(e.begin(), e.end(), e.begin(), ::towlower);
            if (e != L".slangp")
                continue;
            std::string rel = fs::relative(it->path(), root.dir, ec).generic_u8string();
            out.push_back({root.name + "/" + rel, it->path()});
        }
    }
    std::sort(out.begin(), out.end(), [](const PresetEntry &a, const PresetEntry &b) { return a.label < b.label; });
    return out;
}
