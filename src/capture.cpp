// Implementation of capture.h: writes the built-in capture shader and preset to disk.

#include "capture.h"
#include "capture_slang.h"
#include "utf8.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>

namespace fs = std::filesystem;

namespace
{
// Writes `text` to `path` unless the file already holds exactly that text. Writes to a
// temporary name first and renames, so another process reading the file never sees
// half of it. Returns false if the file cannot be written.
bool write_if_different(const fs::path &path, const std::string &text)
{
    {
        std::ifstream in(path, std::ios::binary);
        if (in && std::string(std::istreambuf_iterator<char>(in), {}) == text)
            return true;
    }
    const fs::path tmp = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        // Another process may have written the same file in the meantime.
        std::ifstream in(path, std::ios::binary);
        return in && std::string(std::istreambuf_iterator<char>(in), {}) == text;
    }
    return true;
}
}

std::string capture_preset_path(std::string &error)
{
    static std::mutex mutex;
    static std::string cached;
    std::lock_guard<std::mutex> lock(mutex);
    if (!cached.empty())
        return cached;

    const std::string slang = kCaptureSlang, slangp = kCaptureSlangp;
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) == 0)
    {
        error = "no temporary folder for the capture shader";
        return {};
    }
    // Folder named after the content: an older version's files are never reused.
    const size_t hash = std::hash<std::string>()(slang + slangp);
    const fs::path dir = fs::path(tmp) / (L"reshade-retroarch-capture-" + std::to_wstring(hash));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!write_if_different(dir / L"capture.slang", slang) || !write_if_different(dir / L"capture.slangp", slangp))
    {
        error = "could not write the capture shader to " + utf8_from_path(dir);
        return {};
    }
    cached = utf8_from_path(dir / L"capture.slangp");
    return cached;
}
