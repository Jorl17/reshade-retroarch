# RetroArch Shaders for ReShade

A ReShade add-on that runs **unmodified RetroArch slang shader presets** (`.slangp`) on
any Direct3D 11 game, with the same output you would get in RetroArch.

Most PC re-releases of old games draw a low-resolution picture and scale it up to your
screen. A CRT shader has to see the picture at its original resolution, or it draws
scanlines and phosphors at the wrong scale. This add-on finds that original picture
inside the scaled-up frame, rebuilds it at its native resolution, and passes it to the
RetroArch preset through [librashader](https://github.com/SnowflakePowered/librashader).
The shader's output then replaces that part of the screen.

- Uses the RetroArch presets as-is. There is nothing to port, and any `.slangp` works,
  including `#reference` presets with parameter overrides.
- Finds the native resolution automatically and handles black bars, pillarboxing,
  window resizes and the game changing resolution.
- Pixels outside the game picture are never changed, such as HD side art or borders.
- Each ReShade preset can have its own RetroArch shader, so ReShade's preset keys also
  switch shaders.
- Includes an in-game panel to pick shaders, adjust their parameters and save them.

## Install

1. Install [ReShade](https://reshade.me) **with full add-on support** for your game
   (Direct3D 10/11/12, the "ReShade with full add-on support" download).
2. Download the latest release zip. Extract its contents into the game's folder, next to
   the game's `.exe` and ReShade's `dxgi.dll` / `d3d11.dll`:
   ```
   RetroArchShaders.addon64
   librashader.dll
   reshade-shaders\Shaders\RetroArchShaders.fx
   retroarch-shaders\            (put shaders here, see below)
   ```
3. Get shaders. Either:
   - download the [libretro slang-shaders](https://github.com/libretro/slang-shaders)
     (Code → Download ZIP) and extract them into `retroarch-shaders\`, or
   - do nothing if RetroArch is already installed. The add-on also looks in
     `C:\RetroArch-Win64\shaders\shaders_slang`, `%APPDATA%\RetroArch\shaders\shaders_slang`
     and RetroArch on Steam, in every Steam library.

Start the game and press **Home** to open ReShade. A **RetroArch Shaders** window opens
with the overlay and shows how many presets were found. On a fresh ReShade layout it is a
tab next to Home and Add-ons. If you have used ReShade in this game before, it may open
as a separate window; drag it onto the tabs to dock it.

## Use

In the ReShade overlay (**Home**), open the **RetroArch Shaders** window:

1. **Shader.** Type in the filter (for example `crt-royale`), select a preset, and press
   **Use for this ReShade preset**. The shader appears immediately.
2. **Shader parameters.** Move the sliders to see changes live. Press **Save to this
   ReShade preset** to keep them. The file stores only the values that differ from the
   preset you picked, including ones saved earlier. **Revert** goes back to the saved
   values.
3. **Game resolution.** Usually leave this on **Detect automatically**. See
   [Resolution](#resolution).

To have several looks, create several ReShade presets with the **+** button next to the
preset name on ReShade's Home tab, and give each one a different shader. Switching ReShade
presets switches shaders. To switch during play, set **Previous preset key** and **Next
preset key** in ReShade's Settings tab. **Effect toggle key** turns everything off.
ReShade leaves all three unset by default.

### How the choice is stored

The shader for a ReShade preset is stored in a small `.slangp` file next to the preset's
`.ini`, with the same name:

```
reshade-presets\
  CRT.ini       ReShade preset (effects, keys, ...)
  CRT.slangp    RetroArch shader for it, written by the panel
```

That file is an ordinary RetroArch preset that references the real one and lists your
changed parameters:

```ini
#reference "../retroarch-shaders/slang-shaders/crt/crt-royale.slangp"
diffusion_weight = "0.010000"
```

You can write or edit these by hand, or drop one in while the game runs. Changes are
picked up within a second. A ReShade preset without a `.slangp` next to it leaves the
picture untouched. **Remove from this ReShade preset** renames the file to
`.slangp.removed` (or `.removed.2`, and so on), so nothing is overwritten.

`#reference` paths are relative to the `.slangp` file; absolute paths also work. The
panel writes a relative path for shaders inside the game's folder, so the folder can be
moved, and an absolute path for shaders elsewhere (a RetroArch install, `ShaderPaths`),
so the file still works when you copy it to another game.

## Use any shader

Any slang preset that librashader can load works. See [Compatibility](#compatibility)
for how much of the libretro collection that covers. You can use:

- **The libretro collection:** extract it into `retroarch-shaders\`.
- **Preset packs** (for example CyberLab, Mega Bezel packs, Sonkun): extract them into
  `retroarch-shaders\` so their relative paths still point at the shaders they use. Most
  packs expect to sit next to the libretro `slang-shaders` folder, the same way they do
  in RetroArch.
- **Your own presets** anywhere under `retroarch-shaders\`, or in extra folders listed in
  `ReShade.ini`:
  ```ini
  [RetroArchShaders]
  ShaderPaths=D:\my-shaders;E:\more-shaders
  ```
  Separate folders with `;` or `,`. Folder names containing a comma cannot be used,
  because ReShade splits values on commas.
- **A preset saved from RetroArch** (Quick Menu → Shaders → Save): copy the `.slangp`
  next to your ReShade preset `.ini` and give it the same name. Make sure its paths
  resolve from its new location.

The panel lists every `.slangp` it finds, recursively, following folder links
(symlinks and junctions). Folders that cannot be read are skipped, and the panel says how
many. Press **Rescan** after adding files. The list comes from `retroarch-shaders\` next to the add-on and next to the game
`.exe`, the RetroArch locations above, and `ShaderPaths`.

## Resolution

The shader needs the game's native picture, for example 320×224 for a Mega Drive game
shown at 4K. There are three modes:

| Mode | What it does | Use it when |
|---|---|---|
| **Detect automatically** (default) | Finds the pixel grid in the frame: its resolution and where it sits (black bars, pillarbox, centred window). It checks again every second, and every quarter second while nothing is found. A result has to agree twice before it replaces a working one. | Games that scale with nearest-neighbour or sharp filtering. That covers nearly all emulated re-releases. |
| **Fixed** | Treats the whole frame as W×H native pixels. | Detection fails, or the game uses a smooth filter, or the game always fills the screen at one resolution. For example, Sonic Origins' widescreen mode is always 424×240, and Fixed also keeps pause menus and title cards from confusing detection. |
| **Whole frame** | Passes the full frame to the shader, with no native recovery. | The game renders at full resolution already, or you want the shader to see the frame as-is. |

What happens when things change:

- **Window or output resolution changes** (resize, fullscreen toggle, new display mode):
  the known grid is rescaled at once, then re-detected. The shader chain adapts to the
  new size without recompiling.
- **The game changes native resolution or aspect** (for example a 4:3/16:9 toggle, a
  different system, or a menu): the new grid is picked up within one to two seconds.
- **Nothing retro on screen** (HD menus, loading screens with smooth art): the last grid
  stays in use, so the shader keeps drawing and does not flicker on and off. HD screens
  are shown at the last native resolution until you return to the game.
- **Something covers most of the picture** (a pause menu, a title card, a text box): a
  patch of game pixels left visible is not mistaken for the picture. A patch whose pixel
  grid carries on past its edges is rejected, and a patch that lines up with the grid in
  use never replaces it unless black bars surround it (a genuine resolution change). See
  the [FAQ](#faq) if your game still gets it wrong.

The RetroArch Shaders window always shows the grid in use, for example
`Running on 424x240 in 3840x2160 at (0,0), match 100%`.

## Troubleshooting

The RetroArch Shaders window and `ReShade.log` (next to the game `.exe`) explain what is
happening.

| Message | Meaning |
|---|---|
| `librashader.dll not found next to the add-on` | Copy `librashader.dll` next to `RetroArchShaders.addon64`. |
| `Paused: ReShade is not rendering effects` | Effects are toggled off, or ReShade found no effect files. Make sure `reshade-shaders\Shaders\RetroArchShaders.fx` is installed. ReShade only renders when at least one effect exists. |
| `Waiting: looking for the game's pixels` or `no pixel grid found yet` | Detection has not found a grid yet. It needs a scene with detail, so it will not work on a black screen. Switch to **Fixed** if it never finds one. |
| An error in red under the shader name | The preset failed to load or compile. The message comes from librashader, and `ReShade.log` has it as `Could not load <file>: ...`. Try the same preset in RetroArch to tell a broken preset from a problem here. |
| `HDR output is not supported yet` | Turn HDR off in the game. |
| `Off: no RetroArch shader for this ReShade preset` | This ReShade preset has no `.slangp` next to it. Pick a shader in the RetroArch Shaders window. |

The add-on only supports **Direct3D 11** games with 8-bit or 10-bit SDR back buffers. On
other APIs, or with HDR output, it leaves the picture alone and says why.

## FAQ

### The CRT effect only covers a box in the middle, or there are black lines in part of the screen

The add-on took part of the picture for the whole game. This can happen when a pause
menu, title card or text box covers most of the screen, so that only a patch of game
pixels is left to look at. That patch then gets the CRT effect at a tiny resolution,
which looks like a box of dark horizontal lines. The add-on guards against this: it
rejects any patch whose pixel grid carries on past its edges, and once it knows the real
picture it never lets a piece of it take over. If it still happens with your game, set
the resolution by hand:

1. Press **Home** and open the **RetroArch Shaders** window.
2. Open **Game resolution** and choose **Fixed**.
3. Type the game's native **Width** and **Height** (see below). The setting is saved and
   used from then on.

With **Fixed**, detection is off and menus or title cards can no longer confuse it. The
whole frame is treated as one picture of that size, so only use it when the game fills
the screen. For a game shown with bars at the sides or top, stay on **Detect
automatically** and press **Detect again** if it gets it wrong.

**Finding the native resolution.** During normal gameplay (not in a menu), the window
shows the grid in use, for example `Running on 424x240 in 3840x2160 at (0,0)`. The first
two numbers are the width and height to type. Common values: Sonic Origins in widescreen
424×240, Mega Drive/Genesis 320×224 (some games 256×224), SNES 256×224, NES 256×240.

### The effect looks blurry, or the scanlines are the wrong size

Same cause and same fix: the resolution in use is wrong. Check the grid shown in the
window during gameplay, and set **Fixed** with the right width and height if needed.

### I have an HDR screen. Does it work?

Yes. When a game draws a normal (SDR) picture, Windows shows it on an HDR screen by
itself, and the add-on works as usual. Only games whose own HDR option is turned on are
refused; turn that option off in the game.

### Pause menus and other HD screens get the CRT effect too

That is expected. The shader covers the game's picture, and anything the game draws on
top of it goes through the shader at the game's native resolution.

### How do I report a detection problem?

In ReShade's **Settings** tab, turn on **Save before and after images** and set a
**Screenshot key**. Take a screenshot while the problem is on screen and include the
`... Before.png` file: it is exactly what the detector sees.

## Limitations

- Direct3D 11 only. Vulkan, D3D12 and OpenGL are not supported yet. librashader supports
  them, so this is future work.
- No HDR back buffers.
- The add-on keeps one compiled shader per window. Switching to a ReShade preset with a
  different shader recompiles it, which usually takes a fraction of a second but can take
  a few seconds for very large presets.
- Detection needs sharp pixels. Games that upscale with a smooth (bilinear) filter need
  **Fixed** mode.
- The shader sees the native picture only. The rest of the frame, such as HD side art,
  is left exactly as the game drew it, so shaders that draw their own bezels only cover
  the game area.

## Build

Requirements: Visual Studio 2022 or its Build Tools with the C++ workload (includes CMake
and Ninja), Windows SDK, and Python 3 with `numpy` and `Pillow` for the tests.

```
build.bat
```

The output goes to `out\build\`. `RetroArchShaders.addon64` is the add-on. The build does
not need librashader at link time. It loads `librashader.dll` at run time through
librashader's own loader header. Get the DLL from
[librashader releases](https://github.com/SnowflakePowered/librashader/releases), version
0.12.0 or newer, the `librashader-x86_64-windows-v<version>-optimized.zip` asset (not the
`win7` one). Newer versions are fine as long as the API stays compatible, and the add-on
checks.

`python tools/package.py` builds the release zip into `out\package\`. It downloads the
pinned librashader release and checks its SHA-256, unless you pass
`--librashader path\to\librashader.dll`.

### Tests

| Test | What it checks |
|---|---|
| `python tests/make_synthetic.py --procedural out/synthetic` then `out/build/detect_test out/synthetic/manifest.txt` | Grid detection on generated frames: nearest and sharp scaling, black bars, pillarbox with HD art, non-integer scales, and frames that must be rejected. Pass a real 1:1 game capture instead of `--procedural` to test with your own game. |
| `out/build/files_test` | Companion `.slangp` files and discovery: `#reference` parsing as librashader does it (quotes, comments), bad text encodings, relative and absolute references, backups that never overwrite, folder links and loops, cancelling a scan. |
| `out/build/render_png <preset.slangp> <frame.png> <out.png>` | Runs a preset on a screenshot through the add-on's own code, with no game and no ReShade. Useful for comparing presets and reporting problems. |
| `python tests/e2e.py --reshade <d3d11.dll> --librashader <librashader.dll> --shaders <slang-shaders> --preset <preset> --native <native.png>` | The add-on inside real ReShade in a test window. It checks the output against offline renders, ReShade preset switches, hot reload, pillarbox with side art, window resize, and a missing DLL. |
| `python tests/compat_sweep.py <shaders> <frame.png> <out.json>` | Loads and renders every preset in a folder. `--pause-while game.exe` makes it wait while a game runs. |

## Licence

MIT, see [LICENSE](LICENSE).

Third-party code: ReShade add-on headers (BSD-3-Clause), Dear ImGui headers (MIT),
librashader C headers (MIT), and `librashader.dll`, shipped unmodified in the release
(MPL-2.0, source at https://github.com/SnowflakePowered/librashader). See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Shaders are not included. Each shader
keeps its own licence.
