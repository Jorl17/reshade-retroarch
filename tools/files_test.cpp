// Tests for the companion .slangp files and shader discovery. CPU only, no GPU,
// no ReShade. Works in a fresh temporary folder, removed at the end.
//
//   files_test

#include "companion.h"
#include "discovery.h"
#include "utf8.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
int g_failed = 0, g_passed = 0;

void check(bool ok, const std::string &what)
{
    printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
    (ok ? g_passed : g_failed)++;
}

void write(const fs::path &p, const std::string &bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

std::string read(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

bool same(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}
}

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

    // read_reference follows librashader's rules.
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

    // write_companion: relative inside the game, absolute when asked, round trip.
    std::string err;
    check(write_companion(c, royale, true, {{"diffusion_weight", 0.004688f}}, err), "write relative companion");
    const std::string text = read(c);
    check(text.find("#reference \"../retroarch-shaders/crt/crt-royale.slangp\"") != std::string::npos &&
              text.find("diffusion_weight = \"0.004688\"") != std::string::npos,
          "relative companion text");
    check(read_reference(c, target) && same(target, royale), "relative companion reads back");
    check(write_companion(c, geom, false, {}, err), "write absolute companion");
    check(path_from_utf8(read(c).substr(12, read(c).find('"', 12) - 12)).is_absolute(), "absolute reference written");
    check(read_reference(c, target) && same(target, geom), "absolute companion reads back");
    check(!fs::exists(c.wstring() + L".tmp"), "no temporary file left behind");

    // set_aside never overwrites an earlier backup.
    write(c, "first");
    check(set_aside(c, err) && read(c.wstring() + L".removed") == "first", "set aside to .removed");
    write(c, "second");
    check(set_aside(c, err) && read(c.wstring() + L".removed") == "first" &&
              read(c.wstring() + L".removed.2") == "second",
          "second set aside keeps the first backup");
    check(!set_aside(c, err) && !err.empty(), "set aside of a missing file reports an error");

    // Discovery: recursive, follows a junction once, survives a link loop.
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

    // Junctions first, so removing the tree does not follow them.
    fs::remove(junction);
    fs::remove(loop);
    fs::remove_all(root);
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
