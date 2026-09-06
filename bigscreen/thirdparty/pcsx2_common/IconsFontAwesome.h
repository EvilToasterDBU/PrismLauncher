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
#define ICON_FA_BATTERY_FULL "\xef\x89\x80"            // U+F240
#define ICON_FA_BATTERY_THREE_QUARTERS "\xef\x89\x81"  // U+F241
#define ICON_FA_BATTERY_HALF "\xef\x89\x82"            // U+F242
#define ICON_FA_BATTERY_QUARTER "\xef\x89\x83"         // U+F243
#define ICON_FA_BATTERY_EMPTY "\xef\x89\x84"           // U+F244
#define ICON_FA_BOLT "\xef\x83\xa7"                    // U+F0E7

// Menu-card and top-bar category icons — replace the plain PNG textures
// Landing/Quit/Settings/Instance-Settings used until now (BigScreen plan's
// "Известные, осознанные пробелы" follow-up item). Verified the same way as
// the set above: read out of fa-solid-900.ttf's own tables, then rendered
// and eyeballed before use (see bigscreen/CLAUDE.md for the render check).
#define ICON_FA_GAMEPAD "\xef\x84\x9b"         // U+F11B
#define ICON_FA_USER "\xef\x80\x87"            // U+F007
#define ICON_FA_GEAR "\xef\x80\x93"            // U+F013
#define ICON_FA_POWER_OFF "\xef\x80\x91"       // U+F011
#define ICON_FA_DESKTOP "\xef\x84\x88"         // U+F108
#define ICON_FA_HOUSE "\xef\x80\x95"           // U+F015
#define ICON_FA_PAINTBRUSH "\xef\x87\xbc"      // U+F1FC
#define ICON_FA_CUBE "\xef\x86\xb2"            // U+F1B2
#define ICON_FA_MUG_HOT "\xef\x9e\xb6"         // U+F7B6
#define ICON_FA_GLOBE "\xef\x82\xac"           // U+F0AC
#define ICON_FA_MICROCHIP "\xef\x8b\x9b"       // U+F2DB
#define ICON_FA_LANGUAGE "\xef\x86\xab"        // U+F1AB
#define ICON_FA_PLUG "\xef\x87\xa6"            // U+F1E6
#define ICON_FA_WRENCH "\xef\x82\xad"          // U+F0AD
#define ICON_FA_PUZZLE_PIECE "\xef\x84\xae"    // U+F12E
#define ICON_FA_SWATCHBOOK "\xef\x97\x83"      // U+F5C3
#define ICON_FA_LAYER_GROUP "\xef\x97\xbd"     // U+F5FD
#define ICON_FA_DATABASE "\xef\x87\x80"        // U+F1C0
#define ICON_FA_EARTH_AMERICAS "\xef\x95\xbd"  // U+F57D
#define ICON_FA_FILE_LINES "\xef\x83\xb6"      // U+F0F6
#define ICON_FA_CODE_BRANCH "\xef\x84\xa6"     // U+F126
#define ICON_FA_SERVER "\xef\x88\xb3"          // U+F233
#define ICON_FA_CAMERA "\xef\x80\xb0"          // U+F030
