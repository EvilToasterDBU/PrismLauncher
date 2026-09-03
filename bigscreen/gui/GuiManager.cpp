// SPDX-License-Identifier: GPL-3.0-only
#include "GuiManager.h"

#include "GS/Renderers/Common/GSDevice.h"
#include "ImGui/ImGuiFullscreen.h"

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
    ImFontConfig cfg;
    cfg.SizePixels = 22.0f * scale;
    ImFont* font = io.Fonts->AddFontDefault(&cfg);
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
