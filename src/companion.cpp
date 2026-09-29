#include "companion.h"
#include "utf8.h"

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
    bool in_block_comment = false;
    while (std::getline(in, line))
    {
        // Same rules as librashader's lexer: the line (after leading whitespace) starts
        // with "#reference" and whitespace; the value runs to the end of the line and
        // may be quoted. Anything else starting with '#' or "//" is a comment.
        size_t pos = line.find_first_not_of(" \t\r");
        if (pos == std::string::npos)
            continue;
        if (in_block_comment)
        {
            in_block_comment = line.find("*/", pos) == std::string::npos;
            continue;
        }
        if (line.compare(pos, 2, "/*") == 0)
        {
            in_block_comment = line.find("*/", pos + 2) == std::string::npos;
            continue;
        }
        if (line.compare(pos, 10, "#reference") != 0 || pos + 10 >= line.size() ||
            (line[pos + 10] != ' ' && line[pos + 10] != '\t'))
            continue;
        std::string value = line.substr(pos + 10);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r") + 1);
        if (value.size() >= 2 && value.front() == '"')
        {
            const size_t close = value.find('"', 1);
            if (close != std::string::npos)
                value = value.substr(1, close - 1);
        }
        if (value.empty())
            return false;

        target = path_from_utf8(value);
        std::error_code ec;
        if (target.is_relative())
        {
            // librashader resolves against the canonical folder (junctions resolved).
            fs::path dir = fs::canonical(companion.parent_path(), ec);
            if (ec)
                dir = companion.parent_path();
            target = dir / target;
        }
        const fs::path canon = fs::weakly_canonical(target, ec);
        if (!ec)
            target = canon;
        return true;
    }
    return false;
}

bool write_companion(const fs::path &companion, const fs::path &target, bool relative,
                     const std::vector<std::pair<std::string, float>> &params, std::string &error)
{
    std::error_code ec;
    fs::path ref = target;
    if (relative)
    {
        // Relative to the canonical folder, which is what librashader resolves against.
        fs::path dir = fs::canonical(companion.parent_path(), ec);
        if (ec)
            dir = companion.parent_path();
        const fs::path rel = fs::relative(target, dir, ec);
        if (!ec && !rel.empty())
            ref = rel;
    }

    std::ostringstream text;
    text << "#reference \"" << utf8_from_path(ref, true) << "\"\n";
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
        out.close(); // the data is only written here; check after it
        if (!out)
        {
            error = "could not write " + utf8_from_path(tmp);
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, companion, ec);
    if (ec)
    {
        error = "could not write " + utf8_from_path(companion) + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool set_aside(const fs::path &file, std::string &error)
{
    for (int i = 1; i <= 100; ++i)
    {
        const std::wstring name = file.wstring() + L".removed" + (i == 1 ? std::wstring() : L"." + std::to_wstring(i));
        // Without MOVEFILE_REPLACE_EXISTING this fails if the name is taken.
        if (MoveFileExW(file.c_str(), name.c_str(), 0))
            return true;
        const DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS)
        {
            error = "could not rename " + utf8_from_path(file) + " (error " + std::to_string(e) + ")";
            return false;
        }
    }
    error = "could not rename " + utf8_from_path(file) + ": too many .removed files next to it";
    return false;
}
