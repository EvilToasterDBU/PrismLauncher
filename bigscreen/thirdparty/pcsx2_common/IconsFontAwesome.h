// SPDX-License-Identifier: GPL-3.0-only
// Real codepoints for the Font Awesome 7 Free Solid glyphs the vendored
// ImGuiFullscreen toolkit (and BigScreen's own back/forward-chevron labels)
// use — supersedes the v1 bracketed-text placeholder (tracked as an open
// item in the BigScreen plan, milestone M1: real icon fonts were skipped
// there for lack of a sourced/licensed asset). The user supplied
// `fa-solid-900.ttf` (Font Awesome 7.2.0 Free Solid, the same file PCSX2's
// own AppImage ships at resources/fonts/) — see thirdparty/THIRDPARTY.md
// for licensing.
//
// Every codepoint below was read directly out of that exact font file's own
// `cmap`/glyph-name tables (via fontTools), not guessed or copied from an
// unrelated FA version's header — Font Awesome has renumbered icons across
// major versions before, so a mismatched codepoint would silently render
// the wrong glyph (or nothing). Kept under 0x10000 deliberately: BigScreen's
// vendored Dear ImGui builds with the default 16-bit ImWchar
// (IMGUI_USE_WCHAR32 is off in thirdparty/imgui/imconfig.h), so every glyph
// here uses FA's classic Private-Use-Area codepoint alias where the
// "canonical" modern one lives above the Basic Multilingual Plane (e.g.
// `folder`'s primary codepoint is U+1F5BF, but U+F07B — an older FA
// codepoint the font also maps to the same glyph — works with a 16-bit
// atlas and renders identically).
#pragma once

#define ICON_FA_CHECK "\xef\x80\x8c"          // U+F00C
#define ICON_FA_FILE "\xef\x80\x96"           // U+F016
#define ICON_FA_FOLDER "\xef\x81\xbb"         // U+F07B
#define ICON_FA_FOLDER_OPEN "\xef\x81\xbc"    // U+F07C
#define ICON_FA_FOLDER_PLUS "\xef\x99\x9e"    // U+F65E
#define ICON_FA_SQUARE "\xef\x83\x88"         // U+F0C8
#define ICON_FA_SQUARE_CHECK "\xef\x85\x8a"   // U+F14A
#define ICON_FA_SQUARE_XMARK "\xef\x8b\x93"   // U+F2D3
#define ICON_FA_XMARK "\xef\x80\x8d"          // U+F00D
#define ICON_FA_CHEVRON_LEFT "\xef\x81\x93"   // U+F053
#define ICON_FA_CHEVRON_RIGHT "\xef\x81\x94"  // U+F054
#define ICON_FA_CHEVRON_UP "\xef\x81\xb7"     // U+F077
#define ICON_FA_CHEVRON_DOWN "\xef\x81\xb8"   // U+F078
