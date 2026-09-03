// SPDX-License-Identifier: GPL-3.0-only
// v1 placeholder for PCSX2's IconsFontAwesome.h: plain-text fallbacks for the
// handful of Font Awesome glyphs the vendored ImGuiFullscreen toolkit uses,
// so BigScreen doesn't need to bundle/license a real icon font yet (tracked
// as an open item in the BigScreen plan, milestone M1). Swap these defines
// for the real codepoints once a font is sourced — call sites don't change.
#pragma once

#define ICON_FA_CHECK "[v]"
#define ICON_FA_FILE "[File]"
#define ICON_FA_FOLDER "[Dir]"
#define ICON_FA_FOLDER_OPEN "[Dir]"
#define ICON_FA_FOLDER_PLUS "[Dir+]"
#define ICON_FA_SQUARE "[ ]"
#define ICON_FA_SQUARE_CHECK "[x]"
#define ICON_FA_SQUARE_XMARK "[x]"
#define ICON_FA_XMARK "[x]"
