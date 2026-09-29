// Grid detection test runner.
//
//   detect_test <manifest.txt>      checks every case, exits non-zero on any failure
//   detect_test <image.png>         prints what it detects
//   detect_test --rules             checks which detections count as a piece of the grid in use
//
// Manifest lines: <valid 0|1|2> <native_w> <native_h> <rect_x> <rect_y> <rect_w> <rect_h> <file>
// (file relative to the manifest; the rest of the line may contain spaces)
// valid 2 = either that grid or rejected (e.g. HD menus over gameplay); a wrong grid fails.
// valid 3 = rejected, or a piece of that grid without bars around it, which the detector
//           never lets replace that grid (title cards and menus over dark screens).
// valid 4 = exactly that grid, bounded by bars or the frame (so it may replace a larger one).

#include "../src/grid_detect.h"
#include "png_io.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static std::wstring widen(const std::string &s)
{
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    w.pop_back();
    return w;
}

static bool run(const std::wstring &path, PixelGrid &g, std::string &log, double &ms)
{
    std::vector<uint8_t> rgba;
    UINT w = 0, h = 0;
    if (!load_png(path.c_str(), rgba, w, h))
        return false;
    FrameView f;
    f.data = rgba.data();
    f.width = int(w);
    f.height = int(h);
    f.pitch = size_t(w) * 4;
    f.bytes_per_pixel = 4;
    const auto t0 = std::chrono::steady_clock::now();
    g = detect_grid(f, &log);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        fwprintf(stderr, L"usage: detect_test <manifest.txt | image.png>\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring arg = argv[1];

    if (arg == L"--rules")
    {
        // part_of: which detections count as a piece of the grid in use.
        auto grid = [](int nw, int nh, int x, int y, int w, int h) {
            PixelGrid g;
            g.valid = true;
            g.native_w = nw;
            g.native_h = nh;
            g.rect_x = x;
            g.rect_y = y;
            g.rect_w = w;
            g.rect_h = h;
            return g;
        };
        struct Case
        {
            const char *what;
            PixelGrid piece, whole;
            bool expected;
        };
        const Case cases[] = {
            {"title card piece of 424x240 at 4K", grid(122, 240, 1368, 0, 1104, 2160), grid(424, 240, 0, 0, 3840, 2160), true},
            {"pause-menu piece of 424x240 at 4K", grid(172, 64, 1141, 792, 1558, 577), grid(424, 240, 0, 0, 3840, 2160), true},
            {"title card piece of 424x240 at 1080p", grid(121, 41, 688, 450, 549, 185), grid(424, 240, 0, 0, 1920, 1080), true},
            {"4:3 mode (320x240) is not a piece of 424x240, 4K", grid(320, 240, 480, 0, 2880, 2160), grid(424, 240, 0, 0, 3840, 2160), false},
            {"4:3 mode (320x240) is not a piece of 424x240, 1080p", grid(320, 240, 240, 0, 1440, 1080), grid(424, 240, 0, 0, 1920, 1080), false},
            {"widescreen is not a piece of 4:3", grid(424, 240, 0, 0, 3840, 2160), grid(320, 240, 480, 0, 2880, 2160), false},
            {"H32 inside H40 at 4x is on the same grid", grid(256, 224, 448, 92, 1024, 896), grid(320, 224, 320, 92, 1280, 896), true},
            {"half-cell offset is a different grid", grid(122, 240, 1372, 0, 1104, 2160), grid(424, 240, 0, 0, 3840, 2160), false},
            {"bigger than the grid in use", grid(424, 240, 0, 0, 3840, 2160), grid(122, 240, 1368, 0, 1104, 2160), false},
        };
        int pass = 0, fail = 0;
        for (const Case &c : cases)
        {
            const bool ok = part_of(c.piece, c.whole) == c.expected;
            printf("%s  %s\n", ok ? "pass" : "FAIL", c.what);
            (ok ? pass : fail)++;
        }
        printf("\n%d passed, %d failed\n", pass, fail);
        return fail == 0 ? 0 : 1;
    }

    if (arg.size() > 4 && _wcsicmp(arg.c_str() + arg.size() - 4, L".png") == 0)
    {
        PixelGrid g;
        std::string log;
        double ms = 0;
        if (!run(arg, g, log, ms))
            return 1;
        printf("%s  [%s]  %.0f ms\n", g.describe().c_str(), log.c_str(), ms);
        return 0;
    }

    std::ifstream in(arg);
    const std::wstring dir = arg.substr(0, arg.find_last_of(L"\\/") + 1);
    int pass = 0, fail = 0;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream s(line);
        int valid, nw, nh, rx, ry, rw, rh;
        s >> valid >> nw >> nh >> rx >> ry >> rw >> rh;
        std::string file;
        std::getline(s >> std::ws, file);

        PixelGrid g;
        std::string log;
        double ms = 0;
        const bool absolute = file.size() > 2 && file[1] == ':';
        if (!run(absolute ? widen(file) : dir + widen(file), g, log, ms))
        {
            printf("FAIL  %-44s could not load\n", file.c_str());
            ++fail;
            continue;
        }
        const bool exact = g.valid && g.native_w == nw && g.native_h == nh && g.rect_x == rx &&
                           g.rect_y == ry && g.rect_w == rw && g.rect_h == rh;
        PixelGrid whole;
        whole.valid = true;
        whole.native_w = nw;
        whole.native_h = nh;
        whole.rect_x = rx;
        whole.rect_y = ry;
        whole.rect_w = rw;
        whole.rect_h = rh;
        const bool ok = valid == 1   ? exact
                        : valid == 0 ? !g.valid
                        : valid == 2 ? (exact || !g.valid)
                        : valid == 3 ? (!g.valid || (!g.bounded && part_of(g, whole)))
                                     : (exact && g.bounded);
        printf("%s  %-44.44s %5.0f ms  got: %s", ok ? "pass" : "FAIL", file.size() > 44 ? file.c_str() + file.size() - 44 : file.c_str(), ms, g.describe().c_str());
        if (!ok)
            printf("\n      expected: %s%s   [%s]", valid == 3 ? "rejected or a piece of " : "", valid ? (std::to_string(nw) + "x" + std::to_string(nh) + " in " +
                                                           std::to_string(rw) + "x" + std::to_string(rh) + " at (" +
                                                           std::to_string(rx) + "," + std::to_string(ry) + ")").c_str()
                                                        : "rejected", log.c_str());
        printf("\n");
        (ok ? pass : fail)++;
    }
    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
