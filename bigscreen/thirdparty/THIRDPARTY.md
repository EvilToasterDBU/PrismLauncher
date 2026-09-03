# Third-party code vendored into BigScreen

## Dear ImGui (`thirdparty/imgui/`)
Git submodule, `docking` branch, upstream https://github.com/ocornut/imgui.
License: MIT (see `thirdparty/imgui/LICENSE.txt`).

## stb_image (`thirdparty/stb/stb_image.h`)
Single header from https://github.com/nothings/stb, public domain / MIT
(dual-licensed, see the header's own license block).

## ImGuiFullscreen toolkit (`thirdparty/pcsx2_common/ImGui/ImGuiFullscreen.{h,cpp}`, `ImGuiAnimated.h`)
Vendored, with local adaptations, from PCSX2 (https://github.com/PCSX2/pcsx2),
`pcsx2/ImGui/`. License: GPL-3.0-or-later (SPDX header preserved in each
file), compatible with PrismLauncher's own GPL-3.0-only.

This toolkit originates with DuckStation (https://github.com/stenzek/duckstation),
whose current source is under CC-BY-NC-ND 4.0 (no derivatives) as of a
September 2024 relicense — **not usable here**. PCSX2 carries a copy from
before that relicense, still actively maintained under GPL-3.0, which is
what BigScreen vendors instead.

Local adaptations from PCSX2's original (all marked inline with `BigScreen
note:` comments):
- `GetLineHeight()` uses `ImFont::LegacySize` instead of PCSX2's local
  `ImFont::LineHeight` patch (PCSX2 patches stock Dear ImGui to add that
  field — see their `3rdparty/imgui/CHANGELOG.txt` — which we don't carry).
- Two `reinterpret_cast<ImTextureID>` → `static_cast` (ImTextureID widened
  to 64-bit upstream; PCSX2's GLuint handles no longer reinterpret-cast
  cleanly).
- `LoadSvgTextureImage()` is stubbed to fail gracefully (falls back to the
  placeholder texture) rather than pulling in plutovg/plutosvg — not needed
  for the M1 nav demo, revisit when real SVG assets are wired up.
- Added `#include "common/EmuFolders.h"` (see below).

## PCSX2 "common" utilities (`thirdparty/pcsx2_common/common/{LRUCache,HeterogeneousContainers,Easing}.h`)
Vendored unmodified from PCSX2's `common/`. Generic, no PS2/emulator-specific
content. License: GPL-3.0-or-later.

## Everything else under `thirdparty/pcsx2_common/`
Compatibility shims **written for BigScreen** (not vendored from PCSX2),
providing just enough of PCSX2's `common/`, `Input/`, `GS/Renderers/Common/`,
and `ImGui/ImGuiManager.h` surface for the toolkit above to compile and run
standalone — e.g. `Console`/`DevCon` logging, `Timer`, `Threading::Thread`,
a real `FileSystem`/`Path` implementation on `std::filesystem`, and a
`GSTexture`/`g_gs_device` adapter backed directly by OpenGL (PCSX2's real
`GSDevice` abstracts D3D11/D3D12/Vulkan/OpenGL/Metal; BigScreen only ever
targets OpenGL, v1 is Linux/SteamOS-only). License: GPL-3.0-only.
