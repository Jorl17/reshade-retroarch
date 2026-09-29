// Command-line tests for the grid detector (detect_grid and part_of in src/grid_detect.h),
// the code that finds a game's low-resolution picture inside the high-resolution frame
// it presents. Runs on the CPU only: no GPU, no ReShade, no librashader.
//
// Words used below:
//   pixel grid   where the game's native (low-resolution) picture sits in the frame and
//                at what resolution, e.g. "424x240 stretched over 3840x2160 at (0,0)".
//                Each native pixel becomes a block of frame pixels, called a cell.
//   bars         uniform borders around a picture that does not fill the frame.
//   bounded      a detected grid whose edges are the picture's edges: the frame's
//                edges, or bars.
//   piece        a detection covering only part of the grid in use, on the same cells
//                (what stays visible around a menu or title card drawn over the game).
//
// Usage:
//   detect_test <manifest.txt>      runs every case listed in the manifest, prints pass or
//                                   FAIL for each, exits 1 if any failed (0 otherwise)
//   detect_test <image.png>         prints the grid it detects in that image, the
//                                   detector's diagnosis and the time taken
//   detect_test --rules             checks part_of, the rule deciding which detections are
//                                   only a piece of the grid in use (built-in cases)
//
// Manifest lines (tests/make_synthetic.py writes them; lines starting with '#' are skipped):
//   <valid> <native_w> <native_h> <rect_x> <rect_y> <rect_w> <rect_h> <file>
// <file> is relative to the manifest's folder unless it starts with a drive letter, and
// may contain spaces (it is the rest of the line). The numbers describe the expected
// grid. <valid> says what counts as a pass:
//   0 = the detector must find no grid (e.g. a smoothly upscaled or blank frame).
//   1 = it must find exactly that grid.
//   2 = exactly that grid, or no grid; any other grid fails (e.g. HD menus over gameplay).
//   3 = no grid, or an unbounded piece of that grid. The add-on never lets such a piece
//       replace the grid in use (title cards and menus over dark screens).
//   4 = exactly that grid, and bounded (so the add-on may let it replace a larger grid).

#include "../src/grid_detect.h"
#include "png_io.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

// Converts a UTF-8 string (a file name read from the manifest) to UTF-16, the form
// Windows file functions take.
static std::wstring widen(const std::string &s)
{
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    w.pop_back();
    return w;
}

// Loads the image at `path` and runs the grid detector on it. On success returns true
// and sets `g` to the detected grid (invalid if none was found), `log` to the detector's
// short diagnosis and `ms` to the detection time in milliseconds (loading not included).
// Returns false if the image cannot be loaded.
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

// Entry point (wide-character arguments, so file names with any characters work). See
// the top of the file for the three modes. Exit code: 0 all passed, 1 a failure (or an
// image that cannot be loaded), 2 no argument given.
int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        fwprintf(stderr, L"usage: detect_test <manifest.txt | image.png>\n");
        return 2;
    }
    // COM must be initialised before png_io.h can load images.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring arg = argv[1];

    if (arg == L"--rules")
    {
        // Checks part_of(piece, whole): true when `piece` lies inside `whole` on the same
        // cells (same cell size, cell edges lined up), i.e. it is part of that picture
        // rather than a different picture. Each case is a detection, the grid in use,
        // and whether the detection should count as a piece of it.
        // grid() builds a valid PixelGrid: native size nw x nh stretched over the frame
        // rectangle at (x, y) of size w x h.
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
        // One --rules case: a description, the detection (`piece`), the grid in use
        // (`whole`), and the expected result of part_of(piece, whole).
        struct Case
        {
            const char *what;
            PixelGrid piece, whole;
            bool expected;
        };
        // 424x240 is Sonic Origins' widescreen picture, 320x240 its 4:3 mode. H40 and H32
        // are the Mega Drive's 320- and 256-pixel-wide display modes.
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

    // A single image (any argument ending in .png, any case): print what is detected.
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

    // Otherwise the argument is a manifest: run and judge every case in it.
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
        // "C:..." is an absolute path; anything else is relative to the manifest.
        const bool absolute = file.size() > 2 && file[1] == ':';
        if (!run(absolute ? widen(file) : dir + widen(file), g, log, ms))
        {
            printf("FAIL  %-44s could not load\n", file.c_str());
            ++fail;
            continue;
        }
        // `exact`: the detector found the expected grid. `whole`: the expected grid, to
        // test whether an unexpected detection is only a piece of it (valid 3).
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
        // Pass rule for each <valid> code (see the top of the file).
        const bool ok = valid == 1   ? exact
                        : valid == 0 ? !g.valid
                        : valid == 2 ? (exact || !g.valid)
                        : valid == 3 ? (!g.valid || (!g.bounded && part_of(g, whole)))
                                     : (exact && g.bounded);
        // One line per case: result, the last 44 characters of the file name, time, and
        // the detected grid; on failure also the expected grid and the diagnosis.
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
