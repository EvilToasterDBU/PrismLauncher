// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's ImGui/ImGuiManager.h.
//
// PCSX2's real ImGuiManager is wired directly into their GSDevice/Host/
// VMManager emulator core and is not reusable as-is (confirmed by reading
// it — see the BigScreen plan, milestone M1). BigScreen instead grows its
// own manager on top of imgui_impl_sdl2/imgui_impl_opengl3; this header
// only provides the two ImGuiManager:: entry points ImGuiFullscreen.cpp
// actually calls, so the vendored toolkit links today. Expect this file to
// be superseded by a fuller `bigscreen/gui/GuiManager.*` as M1 continues
// (real font metrics, input binding text, DPI scale).
#pragma once

#include "ImGui/ImGuiFullscreen.h"

#include <string>
#include <string_view>

namespace ImGuiManager {

// TODO(M1): replace with the real loaded font's size once BigScreen owns
// font loading; 24px is a reasonable 1x placeholder. BigScreen note: scaled
// by g_layout_scale, unlike PCSX2's original (which had its own DPI/font
// metrics feeding this) — without it, standard-size text stayed a fixed
// 24px regardless of window size or Settings > Appearance's "Scale" while
// medium/large text (sized via LayoutScale() already) responded to both.
inline float GetFontSizeStandard()
{
    return ImGuiFullscreen::LayoutScale(24.0f);
}

// TODO(M1): once BigScreen has its own icon glyphs, strip their codepoint
// range here. The v1 IconsFontAwesome.h/IconsPromptFont.h placeholders are
// already plain bracketed text, so there is nothing icon-specific to strip.
inline std::string StripIconCharacters(std::string_view str)
{
    return std::string(str);
}

}  // namespace ImGuiManager
