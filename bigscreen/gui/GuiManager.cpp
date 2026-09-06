// SPDX-License-Identifier: GPL-3.0-only
#include "GuiManager.h"

#include "GS/Renderers/Common/GSDevice.h"
#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "ImGui/ImGuiFullscreen.h"
#include "common/Console.h"
#include "common/EmuFolders.h"
#include "common/Path.h"

#include "imgui.h"

#include <QImage>

// g_{standard,medium,large}_font / g_layout_scale / g_rcp_layout_scale are
// already defined in the vendored ImGuiFullscreen.cpp itself — we only
// assign to them here, not define them.

namespace ImGuiFullscreen {

// Declared in ImGuiFullscreen.h, never defined there: PCSX2's real
// ImGuiManager.cpp supplies these (context-sensitive controller button
// hints for the choice/file-selector/input-dialog popups). Built from
// GetGamepadGlyphs() (real controller-button glyphs, matching whatever
// gamepad DetectGamepadLayout() found in main.cpp) via the same
// CreateFooterTextString() every other footer hint in bigscreen/main.cpp
// goes through, rather than plain "A: Select    B: Cancel" text.
void GetChoiceDialogHelpText(SmallStringBase& dest)
{
    const GamepadGlyphs glyphs = GetGamepadGlyphs();
    // Checkable (multi-select) dialogs don't have a separate "confirm"
    // button — B both closes AND commits whatever's checked (see
    // BigScreenDialogs::ChooseMultiple()'s own comment for why), so the
    // hint reads "Toggle / Confirm" here instead of "Select / Cancel".
    if (IsChoiceDialogCheckable()) {
        const std::pair<const char*, std::string_view> items[] = { { glyphs.confirm(false), "Toggle" },
                                                                     { glyphs.cancel(false), "Confirm" } };
        CreateFooterTextString(dest, items);
        return;
    }
    const std::pair<const char*, std::string_view> items[] = { { glyphs.confirm(false), "Select" }, { glyphs.cancel(false), "Cancel" } };
    CreateFooterTextString(dest, items);
}

void GetFileSelectorHelpText(SmallStringBase& dest)
{
    GetChoiceDialogHelpText(dest);
}

void GetInputDialogHelpText(SmallStringBase& dest)
{
    const GamepadGlyphs glyphs = GetGamepadGlyphs();
    const std::pair<const char*, std::string_view> items[] = { { glyphs.confirm(false), "OK" }, { glyphs.cancel(false), "Cancel" } };
    CreateFooterTextString(dest, items);
}

}  // namespace ImGuiFullscreen

namespace BigScreenGui {

bool LoadFonts(float scale)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // font.second (in every ImGuiFullscreen g_*_font pair) is an explicit
    // pixel size passed straight to ImGui::PushFont(font, size) — NOT a
    // scale multiplier. The actual per-frame sizes come from
    // ImGuiFullscreen::UpdateFontScale() (called every frame in main.cpp's
    // loop, right after UpdateLayoutScale()), which reads
    // LAYOUT_{MEDIUM,LARGE}_FONT_SIZE and ImGuiManager::GetFontSizeStandard()
    // below. This just needs to load one scalable font source; modern Dear
    // ImGui bakes whatever pixel sizes are actually requested on demand.
    //
    // Roboto instead of Dear ImGui's default (ProggyClean, a tiny
    // fixed-size bitmap font, Basic Latin only) — needed once a non-English
    // language is selected (see TranslationsModel, applied automatically at
    // Application startup same as the desktop build): without real
    // Cyrillic/etc. glyphs, translated text would render as boxes/nothing
    // regardless of the translation itself working correctly. Roboto
    // covers the Cyrillic block (confirmed via fontTools — see
    // thirdparty/THIRDPARTY.md); GetGlyphRangesCyrillic() bakes Basic Latin
    // + Cyrillic glyphs into the atlas up front so both render immediately
    // without a reload when the language changes.
    const std::string fontPath = Path::Combine(EmuFolders::Resources, "fonts/Roboto-Medium.ttf");
    const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesCyrillic();

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 22.0f * scale, &cfg, glyphRanges);
    if (!font) {
        // Fall back to the built-in bitmap font rather than failing to
        // start at all if the TTF couldn't be read for some reason — no
        // Cyrillic, but still a usable (English) UI.
        Console.Error("Failed to load '%s', falling back to the default font (no Cyrillic support)", fontPath.c_str());
        ImFontConfig fallbackCfg;
        fallbackCfg.SizePixels = 22.0f * scale;
        font = io.Fonts->AddFontDefault(&fallbackCfg);
    }
    if (!font)
        return false;

    // Merge Font Awesome + PromptFont glyphs directly into the same font
    // (MergeMode = true bakes them into the same atlas slot range as the
    // text font already loaded above), so any ICON_FA_*/ICON_PF_* macro
    // (IconsFontAwesome.h/IconsPromptFont.h) just works inline in an
    // ordinary ImGui::Text()/MenuButton() string — same technique
    // real-world Dear ImGui icon-font integrations use, and the same two
    // fonts PCSX2's own reference UI ships (resources/fonts/{fa-solid-900,
    // promptfont}) for exactly this purpose (real controller-button glyphs
    // and menu icons instead of plain-text "[A]"/"<" placeholders — see
    // thirdparty/THIRDPARTY.md for provenance/licensing).
    //
    // Explicit per-font ranges (not the whole font) keep the baked atlas
    // small: only the codepoints IconsFontAwesome.h/IconsPromptFont.h
    // actually name are requested, each confirmed to exist in the
    // respective font file (see those headers' own comments) and to fit
    // under 0x10000 (BigScreen's Dear ImGui build uses the default 16-bit
    // ImWchar — IMGUI_USE_WCHAR32 is off).
    static const ImWchar promptFontRanges[] = {
        0x2196, 0x2199, 0x21A2, 0x21A7, 0x21CE, 0x21CE, 0x21D0, 0x21D3, 0x21E0, 0x21E3,
        0x21F7, 0x21F8, 0x21FA, 0x21FB, 0x21FD, 0x21FE, 0x227E, 0x227F, 0x2284, 0x2284, 0,
    };
    static const ImWchar fontAwesomeRanges[] = {
        0xF00C, 0xF00D, 0xF016, 0xF016, 0xF053, 0xF054, 0xF077, 0xF078, 0xF07B, 0xF07C,
        0xF0C8, 0xF0C8, 0xF0E7, 0xF0E7, 0xF14A, 0xF14A, 0xF240, 0xF244, 0xF2D3, 0xF2D3, 0xF65E, 0xF65E, 0,
    };

    ImFontConfig iconCfg;
    iconCfg.MergeMode = true;
    iconCfg.OversampleH = 2;
    iconCfg.OversampleV = 2;
    iconCfg.GlyphMinAdvanceX = 22.0f * scale;  // render icons at a consistent width, like a monospace glyph

    const std::string promptFontPath = Path::Combine(EmuFolders::Resources, "fonts/promptfont.otf");
    if (!io.Fonts->AddFontFromFileTTF(promptFontPath.c_str(), 22.0f * scale, &iconCfg, promptFontRanges))
        Console.Error("Failed to load '%s' — gamepad button glyphs will be missing/blank.", promptFontPath.c_str());

    const std::string fontAwesomePath = Path::Combine(EmuFolders::Resources, "fonts/fa-solid-900.ttf");
    if (!io.Fonts->AddFontFromFileTTF(fontAwesomePath.c_str(), 22.0f * scale, &iconCfg, fontAwesomeRanges))
        Console.Error("Failed to load '%s' — menu/file icons will be missing/blank.", fontAwesomePath.c_str());

    // v1: one loaded font reused for standard/medium/large — good enough
    // for a navigable placeholder UI; a real distinct medium/large face can
    // come later.
    ImGuiFullscreen::g_standard_font.first = font;
    ImGuiFullscreen::g_medium_font.first = font;
    ImGuiFullscreen::g_large_font.first = font;
    return true;
}

std::shared_ptr<GSTexture> UploadQImage(const QImage& image, bool nearest)
{
    if (image.isNull())
        return nullptr;

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    GSTexture* texture = g_gs_device->CreateTexture(static_cast<u32>(rgba.width()), static_cast<u32>(rgba.height()), 1,
                                                     GSTexture::Format::Color, nearest);
    if (!texture)
        return nullptr;

    const GSVector4i rect(0, 0, rgba.width(), rgba.height());
    if (!texture->Update(rect, rgba.constBits(), static_cast<u32>(rgba.bytesPerLine()))) {
        g_gs_device->Recycle(texture);
        return nullptr;
    }

    return std::shared_ptr<GSTexture>(texture, [](GSTexture* tex) { g_gs_device->Recycle(tex); });
}

}  // namespace BigScreenGui
