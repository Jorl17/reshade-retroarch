// Tests how the add-on reads shader parameters and what its "Save" button writes
// (read_preset_params and param_overrides in src/chain.h, write_companion in
// src/companion.h). Uses librashader (librashader.dll next to this executable) only to
// parse presets, so no GPU is needed.
//
// Background: a RetroArch shader preset (.slangp) lists shaders (.slang files) and may
// set their parameters (named numbers the user can adjust, each declared in the shader
// with a default). A preset can start with `#reference "other.slangp"` to take
// everything from another preset and then override some parameter values. The add-on
// saves the user's choice as such a file next to the ReShade preset: the "companion".
//
//   params_test [extra.slangp]...   prints pass or FAIL per check; exit code 0 if all
//                                   passed, else 1. Each extra preset is checked to
//                                   parse and to have at least one parameter.
//
// Builds a two-level #reference chain in a temporary folder around a tiny shader:
//   base.slangp   the shader, parameters at their defaults (alpha 0.5, beta 2.0)
//   mid.slangp    #reference base, alpha = 0.25
//   CRT.slangp    #reference mid,  alpha = 0.125      (the companion)

#include "chain.h"
#include "companion.h"
#include "librashader_api.h"
#include "utf8.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

namespace fs = std::filesystem;

namespace
{
// Number of checks that failed and passed so far; wmain() prints them at the end.
int g_failed = 0, g_passed = 0;

// Records one check: prints "pass" or "FAIL" followed by `what`, and counts it.
void check(bool ok, const std::string &what)
{
    printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
    (ok ? g_passed : g_failed)++;
}

// Creates (or replaces) file `p` with exactly `text` as its content.
void write(const fs::path &p, const std::string &text)
{
    std::ofstream(p, std::ios::binary) << text;
}

// Returns the value each parameter gets when `preset` is loaded (the value set by the
// preset or a preset it references, else the shader's default), by parameter name.
// Returns an empty map, after printing why, if the preset cannot be read.
std::map<std::string, float> values(const fs::path &preset)
{
    std::vector<ShaderParam> params;
    std::string err;
    std::map<std::string, float> out;
    if (!read_preset_params(utf8_from_path(preset), params, err))
    {
        printf("      could not read %s: %s\n", utf8_from_path(preset).c_str(), err.c_str());
        return out;
    }
    for (const ShaderParam &p : params)
        out[p.name] = p.initial;
    return out;
}

// True when two parameter values are equal up to float rounding.
bool close_to(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// Turns a list of (name, value) pairs, as param_overrides returns, into a map by name.
std::map<std::string, float> as_map(const std::vector<std::pair<std::string, float>> &v)
{
    return std::map<std::string, float>(v.begin(), v.end());
}

// The test shader: a minimal RetroArch slang shader (GLSL with a vertex and a fragment
// stage) declaring two parameters, alpha (default 0.5) and beta (default 2.0). It is
// only parsed, never run.
const char *kShader = R"(#version 450
#pragma parameter alpha "Alpha" 0.5 0.0 1.0 0.01
#pragma parameter beta "Beta" 2.0 0.0 4.0 0.1
layout(push_constant) uniform Push { vec4 SourceSize; float alpha; float beta; } params;
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main() { gl_Position = global.MVP * Position; vTexCoord = TexCoord; }

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main() { FragColor = texture(Source, vTexCoord) * params.alpha * params.beta; }
)";
}

// Runs all checks in order (wide-character arguments, so paths with any characters work).
int wmain(int argc, wchar_t **argv)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::string err;
    if (!libra::load(fs::path(exe).parent_path() / L"librashader.dll", err))
    {
        printf("FAIL  %s\n", err.c_str());
        return 1;
    }

    // Write the shader and the three presets described at the top of the file into a
    // fresh temporary folder.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / (L"rra-params-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    write(dir / L"test.slang", kShader);
    write(dir / L"base.slangp", "shaders = 1\nshader0 = \"test.slang\"\n");
    write(dir / L"mid.slangp", "#reference \"base.slangp\"\nalpha = \"0.25\"\n");
    const fs::path companion = dir / L"CRT.slangp";
    write(companion, "#reference \"mid.slangp\"\nalpha = \"0.125\"\n");

    // Reading: each preset gives the values of the nearest preset that sets them.
    const auto base = values(dir / L"base.slangp"), mid = values(dir / L"mid.slangp"), comp = values(companion);
    check(base.size() == 2 && close_to(base.at("alpha"), 0.5f) && close_to(base.at("beta"), 2.0f), "shader defaults");
    check(mid.size() == 2 && close_to(mid.at("alpha"), 0.25f) && close_to(mid.at("beta"), 2.0f), "one level of overrides");
    check(comp.size() == 2 && close_to(comp.at("alpha"), 0.125f), "the last override wins (not the deepest)");
    check(comp.size() == 2 && close_to(comp.at("beta"), 2.0f), "parameters nobody sets keep the shader default");

    // Saving. `current` stands for the sliders in the add-on's window: the companion's
    // parameters, whose `value` the test changes. `mid_params` are the parameters of the
    // preset the companion references. param_overrides(current, mid_params) returns what
    // Save would write: every parameter whose value differs from the referenced preset's.

    // Save without touching anything: only the companion's own override is kept.
    std::vector<ShaderParam> current, mid_params;
    read_preset_params(utf8_from_path(companion), current, err);
    read_preset_params(utf8_from_path(dir / L"mid.slangp"), mid_params, err);
    auto o = as_map(param_overrides(current, mid_params));
    check(o.size() == 1 && o.count("alpha") && close_to(o["alpha"], 0.125f), "save unchanged: keeps alpha, writes nothing else");

    // Change beta, save, reload: both the earlier alpha and the new beta are there.
    for (ShaderParam &p : current)
        if (p.name == "beta")
            p.value = 3.0f;
    o = as_map(param_overrides(current, mid_params));
    check(o.size() == 2 && close_to(o["alpha"], 0.125f) && close_to(o["beta"], 3.0f), "save after a change: old and new values");
    check(write_companion(companion, dir / L"mid.slangp", true, param_overrides(current, mid_params), err),
          "write the companion");
    const auto saved = values(companion);
    check(saved.size() == 2 && close_to(saved.at("alpha"), 0.125f) && close_to(saved.at("beta"), 3.0f), "values survive a reload");

    // Save again with nothing changed: nothing is lost.
    read_preset_params(utf8_from_path(companion), current, err);
    o = as_map(param_overrides(current, mid_params));
    check(o.size() == 2 && close_to(o["alpha"], 0.125f) && close_to(o["beta"], 3.0f), "second save keeps earlier values");

    // Setting a value back to the referenced preset's drops it from the file.
    for (ShaderParam &p : current)
        if (p.name == "alpha")
            p.value = 0.25f;
    o = as_map(param_overrides(current, mid_params));
    check(o.size() == 1 && o.count("beta"), "a value equal to the referenced preset's is not written");

    std::vector<ShaderParam> none;
    check(!read_preset_params(utf8_from_path(dir / L"missing.slangp"), none, err) && !err.empty(),
          "a missing preset reports an error");

    // Presets given on the command line: each must parse and have parameters.
    for (int a = 1; a < argc; ++a)
    {
        std::vector<ShaderParam> params;
        const bool ok = read_preset_params(utf8_from_path(argv[a]), params, err);
        check(ok && !params.empty(), utf8_from_path(fs::path(argv[a]).filename()) + ": " +
                                         (ok ? std::to_string(params.size()) + " parameters" : err));
    }

    fs::remove_all(dir);
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
