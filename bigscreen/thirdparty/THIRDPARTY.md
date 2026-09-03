# Third-party code vendored into BigScreen

## Dear ImGui (`thirdparty/imgui/`)
Git submodule, `docking` branch, upstream https://github.com/ocornut/imgui.
License: MIT (see `thirdparty/imgui/LICENSE.txt`).

## Roboto-Medium.ttf (`resources/fonts/Roboto-Medium.ttf`)
Copied from `thirdparty/imgui/misc/fonts/Roboto-Medium.ttf` (Dear ImGui
bundles it as a sample font) into `resources/` so it loads through the same
`EmuFolders::Resources`-relative path as every other BigScreen asset,
independent of the submodule's own internal layout. Google's Roboto,
licensed Apache License 2.0. Used instead of Dear ImGui's default
(`ProggyClean`, a tiny fixed-size bitmap font covering only Basic Latin) —
Roboto is a real scalable TTF with Cyrillic coverage (confirmed via
`fontTools`: 275/304 codepoints in the Cyrillic Unicode block, everything
needed for Russian and most other Cyrillic-script languages), needed for
BigScreen to render translated UI text once a non-Latin language is
selected — see `BigScreenGui::LoadFonts()`.

## fa-solid-900.ttf (`resources/fonts/fa-solid-900.ttf`)
Font Awesome 7 Free Solid (v7.2.0). Copyright (c) 2026 Fonticons, Inc.
(https://fontawesome.com), Reserved Font Name "Font Awesome". Licensed SIL
Open Font License, Version 1.1 (the font-file license Font Awesome Free
ships — separate from the CC-BY-4.0 icons/CC-BY-4.0 SVGs and MIT code that
also ship in a full Font Awesome Free download, neither of which BigScreen
uses). File supplied by the user, sourced from their local PCSX2 AppImage
install (`resources/fonts/fa-solid-900.ttf` there too — PCSX2's own
reference "Big Picture" UI, the visual target this project is modeled on,
ships the same file for the same purpose). Provides the menu/file icon
glyphs in `thirdparty/pcsx2_common/IconsFontAwesome.h` — every codepoint in
that header was read directly out of this exact file's own cmap (via
`fontTools`), not guessed or carried over from an unrelated FA version.

## promptfont.otf (`resources/fonts/promptfont.otf`)
PromptFont v1.0, by Yukari Hafner (Shinmera) —
https://codeberg.org/shinmera/promptfont (formerly hosted on GitHub).
Dual-licensed "(OFL-1.1 OR zlib)"; used here under the SIL Open Font
License, Version 1.1 (matching the license text the shipped file itself
carries). File supplied by the user, sourced from their local PCSX2
AppImage install (same file PCSX2's own reference UI ships, for the same
purpose: real gamepad-button glyphs instead of plain "[A]"/"[B]" text).
Provides the controller-button/d-pad glyphs in
`thirdparty/pcsx2_common/IconsPromptFont.h` — every codepoint in that
header was cross-checked against PromptFont's own official `glyphs.json`
manifest (fetched from its Codeberg repo), not guessed.

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
