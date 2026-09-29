# RetroArch Shaders for ReShade

A ReShade add-on that runs **unmodified RetroArch shader presets** (`.slangp`) on
Direct3D 11, Direct3D 12, OpenGL and Vulkan games, through
[librashader](https://github.com/SnowflakePowered/librashader).

Re-releases of old games usually scale a low-resolution picture up to your screen. A CRT
shader needs the original picture, or scanlines and masks come out at the wrong scale.
The add-on finds the native picture inside the frame (for example 424×240 inside 4K),
runs the preset on it, and puts the result back. Pixels outside the game picture, such
as HD side art, are left alone.

## Install

1. Install [ReShade](https://reshade.me) **with full add-on support** for the game.
2. Extract the release zip into the game's folder, next to its `.exe`.
3. Put shaders in `retroarch-shaders\`, for example the
   [libretro slang-shaders](https://github.com/libretro/slang-shaders). An installed
   RetroArch (default folder, `%APPDATA%\RetroArch` or Steam) is found automatically.
4. **Direct3D 12 games only:** also add Microsoft's DirectX Shader Compiler, which is not
   included. Download `dxc_<date>.zip` from
   [Microsoft's DirectXShaderCompiler releases](https://github.com/microsoft/DirectXShaderCompiler/releases)
   and copy `bin\x64\dxcompiler.dll` and `bin\x64\dxil.dll` next to
   `RetroArchShaders.addon64`. Without them the add-on leaves Direct3D 12 games untouched
   and says so. (Not sure which your game uses? The RetroArch Shaders window tells you
   if it needs them.)

## Use

Press **Home** and open the **RetroArch Shaders** window (a tab next to Home, or a
separate window if you have used ReShade in this game before).

- **Shader:** filter, select a preset, press **Use for this ReShade preset**.
- **Shader parameters:** sliders apply live. **Save to this ReShade preset** keeps them,
  **Revert** goes back.
- **Game resolution:** leave on **Detect automatically** unless it gets it wrong (see
  [FAQ](#faq)).

Each ReShade preset gets its own shader, so switching ReShade presets switches shaders.
Set ReShade's **Next/Previous preset key** to do that in game, and **Effect toggle key**
to turn it off.

The choice is stored as `<preset>.slangp` next to the ReShade preset's `.ini`: an
ordinary RetroArch preset referencing the one you picked, plus your changed parameters.

```ini
#reference "../retroarch-shaders/slang-shaders/crt/crt-royale.slangp"
diffusion_weight = "0.010000"
```

You can write these by hand or drop one in while the game runs.

To search more folders, add them to `ReShade.ini`:

```ini
[RetroArchShaders]
ShaderPaths=D:\my-shaders;E:\more-shaders
```

## Resolution modes

| Mode | What it does |
|---|---|
| **Detect automatically** | Finds the native resolution and where the picture sits, and follows resizes, resolution changes and 4:3/16:9 toggles. Menus, fades and title cards keep the last good grid. |
| **Fixed** | Treats the whole frame as the width × height you type. |
| **Whole frame** | Gives the shader the frame as-is. |

The window shows the grid in use, for example
`Running on 424x240 in 3840x2160 at (0,0), match 100%`.

## FAQ

### The effect covers only a box, or part of the screen has black lines

Detection took part of the picture for the whole game, usually because a menu or title
card covered the rest. It guards against this, but if your game still triggers it, set
the resolution by hand: **Game resolution → Fixed**, then type the native **Width** and
**Height**.

To find them, look at the `Running on ...` line during normal gameplay: `424x240` means
width 424, height 240. Common values: Mega Drive 320×224, SNES 256×224, NES 256×240,
Sonic Origins widescreen 424×240.

Only use Fixed when the game fills the screen at a single resolution. For games with
bars at the sides, stay on automatic and press **Detect again**.

### Blurry picture, or scanlines at the wrong size

The resolution in use is wrong. Same fix as above.

### Does it work with HDR?

- **HDR screen, SDR game:** yes. This is the usual case: Windows shows the game's normal
  picture on the HDR screen, and the add-on works on that picture.
- **HDR content** (the game's own HDR option turned on): no. RetroArch shaders are
  written for SDR, so the add-on leaves the picture untouched and says so. Turn the
  game's HDR option off.

### My game uses Direct3D 12 and nothing happens

The window (and `ReShade.log`) says `Direct3D 12 needs dxcompiler.dll and dxil.dll`.
librashader needs Microsoft's DirectX Shader Compiler to run shaders on Direct3D 12, and
it is not included with the add-on: Microsoft's licence for `dxil.dll` puts conditions on
redistributing it, so you download it from Microsoft yourself. See step 4 of
[Install](#install).

### How do I report a detection problem?

In ReShade's Settings, turn on **Save before and after images**, take a screenshot while
it happens, and include the `Before.png`.

## Troubleshooting

The RetroArch Shaders window and `ReShade.log` say what is happening.

| Message | Fix |
|---|---|
| `librashader.dll not found` | Put `librashader.dll` next to `RetroArchShaders.addon64`. |
| `Paused: ReShade is not rendering effects` | Effects are toggled off, or `reshade-shaders\Shaders\RetroArchShaders.fx` is missing. |
| `Waiting: looking for the game's pixels` | Needs a scene with detail. Use **Fixed** if it never finds one. |
| Red error under the shader name | The preset failed to load. Check it in RetroArch. |
| `HDR output is not supported yet` | Turn the game's HDR option off. |
| `Direct3D 12 needs dxcompiler.dll and dxil.dll` | Add Microsoft's DirectX Shader Compiler: step 4 of [Install](#install). |
| `... is not supported yet` | The game uses a graphics API the add-on does not support yet (see Limitations). |
| `Vulkan: could not find ...` or `Vulkan: cannot tell ...` | The add-on could not find what it needs on the game's GPU (see Limitations). Please report it with `ReShade.log`. |

## Limitations

- Direct3D 11, Direct3D 12, OpenGL and Vulkan. Direct3D 10 cannot be supported
  (librashader has no Direct3D 10 support).
- Vulkan relies on a workaround: ReShade does not give add-ons the game's Vulkan instance
  and GPU object, which librashader needs, so the add-on makes its own and matches the
  GPU. If it cannot match the GPU or the game's queue, it leaves the game untouched and
  says why. Games that draw their final picture on a different GPU queue from the one
  they show it with (a few recent games do) may not work correctly.
- Direct3D 9 does not work yet: every preset tried so far fails inside librashader
  (`D3DERR_INVALIDCALL`), so the picture is left untouched and the window shows the error.
- Direct3D 12 needs Microsoft's DirectX Shader Compiler, downloaded separately (see Install).
- On Direct3D 12, librashader renders some presets slightly differently from the other
  APIs, so they can look a little different there (crt-royale's glow, for example).
- No HDR content (see FAQ).
- Automatic detection needs sharp pixels. For smoothly upscaled games, use **Fixed**.
- Bezel shaders only cover the game area.

## Build and test

Needs Visual Studio 2022 (or Build Tools) with C++, and Python 3 with `numpy` and
`Pillow` for tests.

```
build.bat
python tools/package.py
```

`build.bat` builds into `out\build\`. `tools/package.py` makes the release zip and
fetches the pinned `librashader.dll`.

| Test | Checks |
|---|---|
| `python tests/make_synthetic.py --procedural out/s` then `out/build/detect_test out/s/manifest.txt` | Detection on generated frames. |
| `out/build/detect_test --rules` | Which detections may replace the grid in use. |
| `python tests/host_selftest.py` | The test host (a stand-in game for D3D9/10/11/12, OpenGL and Vulkan) shows its pictures byte for byte on every API and format. |
| `out/build/files_test`, `out/build/params_test` | Preset files, discovery, saving parameters. |
| `python tests/e2e.py ...` | The add-on inside real ReShade (needs a GPU). |
| `python tests/compat_sweep.py ...` | Loads every preset in a shader folder. |

[docs/assumptions.md](docs/assumptions.md) lists what the add-on relies on in ReShade,
librashader and the graphics APIs beyond what they document, and how each is checked.
Re-run the end-to-end tests on every API after updating ReShade or librashader.

## Licence

MIT. `librashader.dll` is MPL-2.0 and shipped unmodified; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Shaders are not included.
