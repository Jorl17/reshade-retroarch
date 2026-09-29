// Tests for the add-on's file handling: companion .slangp files (src/companion.h) and
// the search for shader presets (src/discovery.h). CPU only: no GPU, no ReShade, no
// librashader. Works in a fresh temporary folder, removed at the end.
//
// A "companion .slangp" is the RetroArch preset file the add-on keeps next to a ReShade
// preset: "<ReShade preset>.slangp" beside "<ReShade preset>.ini". While that ReShade
// preset is selected, the add-on runs it. The add-on writes it as a "#reference" line
// with the path of the preset the user picked, plus the parameter values the user changed:
//
//   #reference "../retroarch-shaders/crt/crt-royale.slangp"
//   diffusion_weight = "0.004688"
//
//   files_test          prints pass or FAIL per check; exit code 0 if all passed, else 1

#include "companion.h"
#include "discovery.h"
#include "utf8.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
// Number of checks that failed and passed so far; main() prints them at the end.
int g_failed = 0, g_passed = 0;

// Records one check: prints "pass" or "FAIL" followed by `what`, and counts it.
void check(bool ok, const std::string &what)
{
    printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
    (ok ? g_passed : g_failed)++;
}

// Creates (or replaces) file `p` with exactly `bytes` as its content, creating its
// folders if needed.
void write(const fs::path &p, const std::string &bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

// Returns the whole content of file `p`, or an empty string if it cannot be read.
std::string read(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// True when `a` and `b` name the same file on disk (after following links); false if
// they differ or either does not exist.
bool same(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}
}

// Runs all checks in order. The folder layout imitates a game folder:
//   <temp>\rra-files-test-<process id>\game\reshade-presets\     ReShade presets, companions
//   <temp>\rra-files-test-<process id>\game\retroarch-shaders\   RetroArch shader presets
int main()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path root = fs::path(tmp) / (L"rra-files-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    const fs::path presets = root / L"game" / L"reshade-presets";
    const fs::path shaders = root / L"game" / L"retroarch-shaders";
    const fs::path royale = shaders / L"crt" / L"crt-royale.slangp";
    const fs::path geom = shaders / L"crt" / L"crt-geom.slangp";
    write(royale, "shaders = 0\n");
    write(geom, "shaders = 0\n");
    fs::path target;

    // read_reference(companion, target) finds the preset a companion refers to. It must
    // read the file the way librashader does: the path may be quoted or not, lines may
    // end in CRLF, commented-out lines do not count, and a malformed file must not throw.
    const fs::path c = presets / L"CRT.slangp";
    write(c, "#reference \"../retroarch-shaders/crt/crt-royale.slangp\"\r\nfoo = \"1.0\"\r\n");
    check(read_reference(c, target) && same(target, royale), "quoted relative reference, CRLF");
    write(c, "  #reference ../retroarch-shaders/crt/crt-royale.slangp  \n");
    check(read_reference(c, target) && same(target, royale), "unquoted reference with surrounding spaces");
    write(c, "// #reference \"../retroarch-shaders/crt/crt-geom.slangp\"\n"
             "# #reference \"../retroarch-shaders/crt/crt-geom.slangp\"\n"
             "/* #reference \"../retroarch-shaders/crt/crt-geom.slangp\"\n"
             "   #reference \"../retroarch-shaders/crt/crt-geom.slangp\" */\n"
             "#reference \"../retroarch-shaders/crt/crt-royale.slangp\"\n");
    check(read_reference(c, target) && same(target, royale), "commented-out references are ignored");
    write(c, "#referencex \"../retroarch-shaders/crt/crt-geom.slangp\"\nshaders = 1\n");
    check(!read_reference(c, target), "'#referencex' is not a reference");
    // A path saved in the Windows ANSI code page instead of UTF-8 ("\xE9" is an e with
    // an accent): converting it must not throw, because an exception inside ReShade
    // would crash the game.
    write(c, "#reference \"C:/Jeux/Caf\xE9/crt.slangp\"\n"); // ANSI, not UTF-8
    bool no_throw = true;
    try
    {
        read_reference(c, target);
    }
    catch (...)
    {
        no_throw = false;
    }
    check(no_throw, "invalid UTF-8 in a reference does not throw");

    // write_companion(companion, target, relative, params, error) writes a companion.
    // Checks: with `relative` set, the path is written relative to the companion's
    // folder; without it, as an absolute path; parameter values are written; the file
    // reads back to the same target; and the temporary file used while writing is gone.
    std::string err;
    check(write_companion(c, royale, true, {{"diffusion_weight", 0.004688f}}, err), "write relative companion");
    const std::string text = read(c);
    check(text.find("#reference \"../retroarch-shaders/crt/crt-royale.slangp\"") != std::string::npos &&
              text.find("diffusion_weight = \"0.004688\"") != std::string::npos,
          "relative companion text");
    check(read_reference(c, target) && same(target, royale), "relative companion reads back");
    check(write_companion(c, geom, false, {}, err), "write absolute companion");
    // The path is the text between `#reference "` (12 characters) and the next quote.
    check(path_from_utf8(read(c).substr(12, read(c).find('"', 12) - 12)).is_absolute(), "absolute reference written");
    check(read_reference(c, target) && same(target, geom), "absolute companion reads back");
    check(!fs::exists(c.wstring() + L".tmp"), "no temporary file left behind");

    // set_aside(file, error) renames a file to "<file>.removed", or "<file>.removed.2",
    // ... if that name is taken, so an earlier backup is never overwritten. (The add-on
    // uses it when a companion is removed, instead of deleting a file the user may have
    // written by hand.) It must report an error when the file does not exist.
    write(c, "first");
    check(set_aside(c, err) && read(c.wstring() + L".removed") == "first", "set aside to .removed");
    write(c, "second");
    check(set_aside(c, err) && read(c.wstring() + L".removed") == "first" &&
              read(c.wstring() + L".removed.2") == "second",
          "second set aside keeps the first backup");
    check(!set_aside(c, err) && !err.empty(), "set aside of a missing file reports an error");

    // scan_presets(roots, cancel, skipped) lists every .slangp under the given folders.
    // Checks: it searches subfolders at any depth and matches the extension in any case;
    // it follows a junction (a Windows folder link, made here with "mklink /J") to a
    // folder elsewhere; a junction pointing back up to the shader folder (a loop) does
    // not make it list presets twice or run forever; and it stops when `cancel` is set.
    // Expected presets: crt-royale, crt-geom, look.SLANGP and, through the junction,
    // linked.slangp. The junction checks are skipped if mklink fails.
    write(shaders / L"pack" / L"deep" / L"a" / L"b" / L"look.SLANGP", "shaders = 0\n");
    const fs::path linked = root / L"elsewhere";
    write(linked / L"linked.slangp", "shaders = 0\n");
    const std::wstring junction = (shaders / L"link").wstring(), loop = (shaders / L"pack" / L"loop").wstring();
    const std::wstring mk1 = L"cmd /c mklink /J \"" + junction + L"\" \"" + linked.wstring() + L"\" >nul";
    const std::wstring mk2 = L"cmd /c mklink /J \"" + loop + L"\" \"" + shaders.wstring() + L"\" >nul";
    const bool links = _wsystem(mk1.c_str()) == 0 && _wsystem(mk2.c_str()) == 0;
    std::atomic<bool> cancel{false};
    int skipped = 0;
    const std::vector<ShaderRoot> roots = {{"retroarch-shaders", shaders}};
    const std::vector<PresetEntry> found = scan_presets(roots, cancel, skipped);
    // True if the scan found a preset with this label ("<root name>/<path inside root>").
    auto has = [&](const std::string &label) {
        for (const PresetEntry &e : found)
            if (e.label == label)
                return true;
        return false;
    };
    check(has("retroarch-shaders/crt/crt-royale.slangp") && has("retroarch-shaders/pack/deep/a/b/look.SLANGP"),
          "finds presets recursively, any extension case");
    if (links)
    {
        check(has("retroarch-shaders/link/linked.slangp"), "follows a junction");
        check(found.size() == 4, "a junction loop is visited once (" + std::to_string(found.size()) + " presets)");
    }
    else
        printf("skip  junction tests (mklink failed)\n");
    cancel = true;
    check(scan_presets(roots, cancel, skipped).empty(), "a cancelled scan stops");

    // Clean up. The junctions are removed first, so that deleting the folder tree does
    // not follow them into the folders they point to.
    fs::remove(junction);
    fs::remove(loop);
    fs::remove_all(root);
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
