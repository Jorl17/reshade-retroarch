#include "companion.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

fs::path companion_path(const fs::path &reshade_preset)
{
    fs::path p = reshade_preset;
    p.replace_extension(L".slangp");
    return p;
}

bool read_reference(const fs::path &companion, fs::path &target)
{
    std::ifstream in(companion);
    std::string line;
    while (std::getline(in, line))
    {
        const size_t pos = line.find("#reference");
        if (pos == std::string::npos)
            continue;
        const size_t a = line.find('"', pos), b = line.rfind('"');
        if (a == std::string::npos || b <= a)
            return false;
        target = fs::u8path(line.substr(a + 1, b - a - 1));
        if (target.is_relative())
            target = companion.parent_path() / target;
        std::error_code ec;
        target = fs::weakly_canonical(target, ec);
        return true;
    }
    return false;
}

bool write_companion(const fs::path &companion, const fs::path &target,
                     const std::vector<std::pair<std::string, float>> &params, std::string &error)
{
    // Relative when possible, so the folder can move (and RetroArch reads it too).
    std::error_code ec;
    fs::path ref = fs::relative(target, companion.parent_path(), ec);
    if (ec || ref.empty())
        ref = target;

    std::ostringstream text;
    text << "#reference \"" << ref.generic_u8string() << "\"\n";
    if (!params.empty())
        text << "\n";
    for (const auto &[name, value] : params)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", value);
        text << name << " = \"" << buf << "\"\n";
    }

    // Write then rename, so a half-written file is never loaded.
    const fs::path tmp = companion.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text.str();
        if (!out)
        {
            error = "could not write " + tmp.u8string();
            return false;
        }
    }
    fs::rename(tmp, companion, ec);
    if (ec)
    {
        error = "could not write " + companion.u8string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}
