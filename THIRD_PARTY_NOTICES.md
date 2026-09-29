# Third-party notices

The add-on's own code is MIT licensed (see `LICENSE`). It uses the following.

Full licence texts: in the release zip they are in `licenses/` next to this file; in the
repository they sit next to each component in `third_party/`.

## librashader

- `librashader.dll` (shipped in the release zip, unmodified):
  **Mozilla Public License 2.0**. The full text is in `licenses/librashader-MPL-2.0.md`.
  The source code for this exact build is available at
  https://github.com/SnowflakePowered/librashader/tree/librashader-v0.12.0.
  The binary is the official `librashader-x86_64-windows-v0.12.0-optimized.zip` release from
  https://github.com/SnowflakePowered/librashader/releases/tag/librashader-v0.12.0.
- `third_party/librashader/librashader.h` and `librashader_ld.h` (C API headers, used at
  build time): **MIT**, Copyright 2022 chyyran. The licence text is at the top of each header.
  `librashader_ld.h` is patched: it assigns a Direct3D 9 function that does not exist,
  which does not compile when Direct3D 9 support is enabled; the two lines are removed
  (marked "reshade-retroarch patch" in the file).

The add-on loads `librashader.dll` at run time through `librashader_ld.h`, as the librashader
README recommends for projects that are not MPL-2.0 themselves. You may replace
`librashader.dll` with any compatible build, including one you compile yourself.

## ReShade add-on headers

`third_party/reshade/` (ReShade 6.8.0 `include/` headers): **BSD 3-Clause**,
Copyright 2014 Patrick Mours. The full text is in `licenses/ReShade-BSD-3-Clause.md`.

## Dear ImGui headers

`third_party/imgui/` (`imgui.h`, `imconfig.h`, used for the ReShade overlay): **MIT**,
Copyright (c) 2014-2025 Omar Cornut. The full text is in `licenses/imgui-MIT.txt`.

## Vulkan headers

`third_party/vulkan/` (Khronos Vulkan-Headers, version in `third_party/vulkan/VERSION`,
used only to build the test host): **Apache-2.0 OR MIT**, Copyright The Khronos Group
Inc. The full text is in `third_party/vulkan/LICENSE.md`.

## Shaders

No shaders are included. RetroArch shader presets keep their own licences. For example,
crt-royale is GPL-2.0-or-later, and many others are public domain or MIT. Check the header
of each shader you redistribute.
