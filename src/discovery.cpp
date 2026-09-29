// Implementation of discovery.h: the folders searched for presets, and the search.

#include "discovery.h"
#include "utf8.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
// True if `p` is an existing folder (false on any error, e.g. no access).
bool is_dir(const fs::path &p)
{
    std::error_code ec;
    return fs::is_directory(p, ec);
}

// Returns the text value `value` of registry key `key` under `root`, or an empty string
// if it does not exist.
std::wstring reg_string(HKEY root, const wchar_t *key, const wchar_t *value)
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

// Returns the folders Steam installs games into ("libraries"): Steam's own folder (from
// the registry) and those listed in its steamapps/libraryfolders.vdf. Empty if Steam is not
// installed.
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
        libs.push_back(path_from_utf8(p));
    }
    return libs;
}
}

std::vector<ShaderRoot> find_shader_roots(const fs::path &addon_dir, const fs::path &exe_dir,
                                          const std::vector<fs::path> &extra)
{
    std::vector<ShaderRoot> roots;
    // Adds `dir` under `name` if it exists and is not the same folder as one already added.
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

    // RetroArch keeps its slang shaders in shaders\shaders_slang, wherever it is installed.
    const fs::path slang = fs::path(L"shaders") / L"shaders_slang";
    add("RetroArch", fs::path(L"C:\\RetroArch-Win64") / slang);
    add("RetroArch", fs::path(L"C:\\RetroArch") / slang);
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0)
        add("RetroArch", fs::path(appdata) / L"RetroArch" / slang);
    for (const fs::path &lib : steam_libraries())
        add("RetroArch (Steam)", lib / L"steamapps" / L"common" / L"RetroArch" / slang);

    for (const fs::path &p : extra)
        add(utf8_from_path(p.filename()), p);
    return roots;
}

std::vector<PresetEntry> scan_presets(const std::vector<ShaderRoot> &roots, const std::atomic<bool> &cancel,
                                      int &skipped)
{
    std::vector<PresetEntry> out;
    std::unordered_set<std::wstring> visited; // canonical folders, so links cannot loop
    skipped = 0;
    // True the first time a folder is seen (by its real path, ignoring case), false after.
    auto first_visit = [&](const fs::path &dir) {
        std::error_code ec;
        const fs::path canon = fs::canonical(dir, ec);
        std::wstring key = (ec ? dir : canon).wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        return visited.insert(key).second;
    };
    // Walk each root's folder tree with a list of folders still to open (no recursion, so
    // deep trees cannot overflow the stack).
    for (const ShaderRoot &root : roots)
    {
        std::vector<fs::path> pending;
        if (first_visit(root.dir))
            pending.push_back(root.dir);
        while (!pending.empty() && !cancel.load(std::memory_order_relaxed))
        {
            const fs::path dir = std::move(pending.back());
            pending.pop_back();
            std::error_code ec;
            fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
            if (ec)
            {
                ++skipped; // e.g. a path too long for the game process: skip it, keep going
                continue;
            }
            for (; it != end && !cancel.load(std::memory_order_relaxed); it.increment(ec))
            {
                if (ec)
                {
                    ++skipped;
                    break;
                }
                const fs::path &path = it->path();
                if (it->is_directory(ec)) // follows links
                {
                    if (!ec && first_visit(path))
                        pending.push_back(path);
                    continue;
                }
                if (!it->is_regular_file(ec))
                    continue;
                std::wstring e = path.extension().wstring();
                std::transform(e.begin(), e.end(), e.begin(), ::towlower);
                if (e != L".slangp")
                    continue;
                // Lexical: through a link, fs::relative would use the link's target and
                // give "../..".
                const fs::path rel = path.lexically_relative(root.dir);
                out.push_back({root.name + "/" + utf8_from_path(rel.empty() ? path.filename() : rel, true), path});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const PresetEntry &a, const PresetEntry &b) { return a.label < b.label; });
    return out;
}
