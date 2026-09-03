// SPDX-License-Identifier: GPL-3.0-only
// Real codepoints for the PromptFont (github/codeberg.org/Shinmera/promptfont)
// gamepad-button glyphs the vendored ImGuiFullscreen toolkit uses (see its
// GetGamepadGlyphs()/GamepadGlyphs, ImGuiFullscreen.cpp) — supersedes the v1
// bracketed-text placeholder (open item from the BigScreen plan, milestone
// M1). The user supplied `promptfont.otf` (v1.0, the same file PCSX2's own
// AppImage ships at resources/fonts/) — see thirdparty/THIRDPARTY.md for
// licensing.
//
// Every codepoint below was cross-checked against PromptFont's own official
// `glyphs.json` manifest (fetched from its Codeberg repo — each entry names
// its exact codepoint, a human-readable name, and category/tag metadata),
// not guessed. All comfortably fit in the Basic Multilingual Plane, so no
// IMGUI_USE_WCHAR32 concern here (unlike a couple of IconsFontAwesome.h's
// glyphs — see that header's own note).
//
// Most call sites should prefer ImGuiFullscreen::GetGamepadGlyphs() (already
// vendored, ImGuiFullscreen.h) over reaching for a specific
// ICON_PF_BUTTON_CIRCLE-style macro directly — it auto-selects the right
// glyph set (Xbox/PlayStation/Nintendo/generic) for whatever controller
// BigScreen actually detected (see bigscreen/main.cpp's
// ImGuiFullscreen::ReportGamepadLayout() call). The ICON_PF_XBOX_LB/RB/LT/RT
// shoulder-button macros below are a BigScreen-only addition — PromptFont
// has no generic/position-based shoulder-button glyph the way it does for
// face buttons and the d-pad (only per-brand ones exist), and every part of
// BigScreen's own UI that mentions shoulder buttons already always says
// "LB/RB/LT/RT" regardless of the connected controller's actual brand (see
// e.g. DrawSettings()'s category-switch hint), so there was no
// GamepadGlyphs-style auto-selection to preserve here.
#pragma once

#define ICON_PF_BURGER_MENU "\xe2\x87\xbb"            // U+21FB xbox-menu ("Burger Menu")
// GetGamepadGlyphs() (ImGuiFullscreen.cpp) uses these four for BOTH the xbox
// and nintendo branches — both brands print literal "A"/"B"/"X"/"Y" on their
// face buttons (just at different physical positions), and PromptFont only
// has one lettered badge glyph per letter, tagged 'xbox' in glyphs.json (no
// separate 'nintendo' one exists) — so this is the real, branded, round
// letter-badge glyph, DISTINCT from ICON_PF_BUTTON_DOWN_A/UP_Y/LEFT_X/
// RIGHT_B below (the unbranded, position-only diamond glyphs GetGamepadGlyphs()
// falls back to for an undetected controller). An earlier version of this
// header wrongly aliased these four to the same codepoints as their _DOWN_A/
// _UP_Y/_LEFT_X/_RIGHT_B counterparts, which made the xbox/nintendo branch
// and the unknown/generic fallback render identically — Xbox and PlayStation
// controllers looked the same as "no controller detected" (reported as
// "always shows universal buttons" regardless of what's actually connected).
#define ICON_PF_BUTTON_A "\xe2\x87\x93"                // U+21D3 xbox-a ("Button A")
#define ICON_PF_BUTTON_B "\xe2\x87\x92"                // U+21D2 xbox-b ("Button B")
#define ICON_PF_BUTTON_X "\xe2\x87\x90"                // U+21D0 xbox-x ("Button X")
#define ICON_PF_BUTTON_Y "\xe2\x87\x91"                // U+21D1 xbox-y ("Button Y")
#define ICON_PF_BUTTON_CIRCLE "\xe2\x87\xa2"            // U+21E2 sony-b ("Button Circle")
#define ICON_PF_BUTTON_CROSS "\xe2\x87\xa3"             // U+21E3 sony-a ("Button Cross")
#define ICON_PF_BUTTON_SQUARE "\xe2\x87\xa0"            // U+21E0 sony-x ("Button Square")
#define ICON_PF_BUTTON_TRIANGLE "\xe2\x87\xa1"          // U+21E1 sony-y ("Button Triangle")
#define ICON_PF_BUTTON_UP_Y "\xe2\x86\xa5"              // U+21A5 (alias of gamepad-y)
#define ICON_PF_BUTTON_DOWN_A "\xe2\x86\xa7"            // U+21A7 (alias of gamepad-a)
#define ICON_PF_BUTTON_LEFT_X "\xe2\x86\xa4"            // U+21A4 (alias of gamepad-x)
#define ICON_PF_BUTTON_RIGHT_B "\xe2\x86\xa6"           // U+21A6 (alias of gamepad-b)
#define ICON_PF_DPAD "\xe2\x87\x8e"                     // U+21CE dpad ("Dpad")
#define ICON_PF_DPAD_LEFT_RIGHT "\xe2\x86\xa2"          // U+21A2 dpad-left-right
#define ICON_PF_DPAD_UP_DOWN "\xe2\x86\xa3"             // U+21A3 dpad-up-down
#define ICON_PF_XBOX_DPAD "\xe2\x8a\x84"                // U+2284 xbox-dpad
#define ICON_PF_XBOX_DPAD_LEFT_RIGHT "\xe2\x89\xbe"     // U+227E xbox-dpad-left-right
#define ICON_PF_XBOX_DPAD_UP_DOWN "\xe2\x89\xbf"        // U+227F xbox-dpad-up-down
#define ICON_PF_MINUS "\xe2\x87\xbd"                    // U+21FD nintendo-minus
#define ICON_PF_PLUS "\xe2\x87\xbe"                     // U+21FE nintendo-plus
#define ICON_PF_SELECT_SHARE "\xe2\x87\xb7"             // U+21F7 gamepad-select ("Select/Share")
#define ICON_PF_SHARE_CAPTURE "\xe2\x87\xba"            // U+21FA xbox-view ("Share/Capture/View")
#define ICON_PF_START "\xe2\x87\xb8"                    // U+21F8 gamepad-start ("Start")
#define ICON_PF_XBOX_LB "\xe2\x86\x98"                  // U+2198 xbox-left-shoulder ("Left Shoulder (LB)")
#define ICON_PF_XBOX_RB "\xe2\x86\x99"                  // U+2199 xbox-right-shoulder ("Right Shoulder (RB)")
#define ICON_PF_XBOX_LT "\xe2\x86\x96"                  // U+2196 xbox-left-trigger ("Left Trigger (LT)")
#define ICON_PF_XBOX_RT "\xe2\x86\x97"                  // U+2197 xbox-right-trigger ("Right Trigger (RT)")
