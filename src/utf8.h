#pragma once

// UTF-8 <-> path conversions that never throw. std::filesystem's u8path and
// u8string throw on invalid input (a hand-edited file saved as ANSI, a stray
// surrogate in a file name), and an exception escaping into ReShade's present
// call takes the game down with it.

#include <windows.h>

#include <filesystem>
#include <string>

// Returns the path whose name is the UTF-8 text `s`. Invalid bytes become the replacement
// character U+FFFD.
inline std::filesystem::path path_from_utf8(const std::string &s)
{
    // Without MB_ERR_INVALID_CHARS, invalid bytes become U+FFFD instead of failing.
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return std::filesystem::path(std::move(w));
}

// Returns path `p` as UTF-8 text, with backslashes, or with forward slashes when
// `generic` is set (as .slangp files write paths). Unpaired surrogates become U+FFFD.
inline std::string utf8_from_path(const std::filesystem::path &p, bool generic = false)
{
    std::wstring w = generic ? p.generic_wstring() : p.wstring();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
