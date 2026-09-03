// SPDX-License-Identifier: GPL-3.0-only
// v1 placeholder for PCSX2's IconsPromptFont.h: plain-text fallbacks for the
// gamepad button glyphs the vendored ImGuiFullscreen toolkit uses, so
// BigScreen doesn't need to bundle/license a real prompt font yet (tracked
// as an open item in the BigScreen plan, milestone M1). Swap these defines
// for real glyph codepoints once a font is sourced — call sites don't change.
#pragma once

#define ICON_PF_BURGER_MENU "[Menu]"
#define ICON_PF_BUTTON_A "[A]"
#define ICON_PF_BUTTON_B "[B]"
#define ICON_PF_BUTTON_X "[X]"
#define ICON_PF_BUTTON_Y "[Y]"
#define ICON_PF_BUTTON_CIRCLE "[O]"
#define ICON_PF_BUTTON_CROSS "[X]"
#define ICON_PF_BUTTON_SQUARE "[Sq]"
#define ICON_PF_BUTTON_TRIANGLE "[Tri]"
#define ICON_PF_BUTTON_UP_Y "[^Y]"
#define ICON_PF_BUTTON_DOWN_A "[vA]"
#define ICON_PF_BUTTON_LEFT_X "[<X]"
#define ICON_PF_BUTTON_RIGHT_B "[>B]"
#define ICON_PF_DPAD "[D-Pad]"
#define ICON_PF_DPAD_LEFT_RIGHT "[<->]"
#define ICON_PF_DPAD_UP_DOWN "[^v]"
#define ICON_PF_XBOX_DPAD "[D-Pad]"
#define ICON_PF_XBOX_DPAD_LEFT_RIGHT "[<->]"
#define ICON_PF_XBOX_DPAD_UP_DOWN "[^v]"
#define ICON_PF_MINUS "[-]"
#define ICON_PF_PLUS "[+]"
#define ICON_PF_SELECT_SHARE "[Select]"
#define ICON_PF_SHARE_CAPTURE "[Share]"
#define ICON_PF_START "[Start]"
