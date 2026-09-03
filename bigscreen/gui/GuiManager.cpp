// SPDX-License-Identifier: GPL-3.0-only
#include "GuiManager.h"

#include "GS/Renderers/Common/GSDevice.h"
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
// hints for the choice/file-selector/input-dialog popups). Minimal v1 text
// until BigScreen has its own glyph-aware footer hints.
void GetChoiceDialogHelpText(SmallStringBase& dest)
{
    dest.clear();
    dest.append("A: Select    B: Cancel");
}

void GetFileSelectorHelpText(SmallStringBase& dest)
{
    dest.clear();
    dest.append("A: Select    B: Cancel");
}

void GetInputDialogHelpText(SmallStringBase& dest)
{
    dest.clear();
    dest.append("A: OK    B: Cancel");
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

    // v1: one loaded font reused for standard/medium/large — good enough
    // for a navigable placeholder UI; a real distinct medium/large face can
    // come later.
    ImGuiFullscreen::g_standard_font.first = font;
    ImGuiFullscreen::g_medium_font.first = font;
    ImGuiFullscreen::g_large_font.first = font;
    return true;
}

std::shared_ptr<GSTexture> UploadQImage(const QImage& image)
{
    if (image.isNull())
        return nullptr;

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    GSTexture* texture =
        g_gs_device->CreateTexture(static_cast<u32>(rgba.width()), static_cast<u32>(rgba.height()), 1, GSTexture::Format::Color);
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
