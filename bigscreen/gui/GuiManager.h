// SPDX-License-Identifier: GPL-3.0-only
// BigScreen's own minimal manager on top of imgui_impl_sdl2/imgui_impl_opengl3
// (M1 seed — see the BigScreen plan). Owns the pieces PCSX2's real
// ImGuiManager would own in their build (fonts, layout scale) that the
// vendored ImGuiFullscreen toolkit expects as globals, without any of
// PCSX2's GSDevice/Host/VMManager coupling.
#pragma once

#include <memory>

class GSTexture;
class QImage;

struct SDL_Window;

namespace BigScreenGui {

// Loads fonts and populates ImGuiFullscreen::g_{standard,medium,large}_font
// and g_layout_scale/g_rcp_layout_scale. Must run after ImGui_ImplSDL2_Init
// and before the first NewFrame(); safe to call again to reload/rescale.
bool LoadFonts(float scale);

// Uploads a Qt QImage (e.g. from IconList::getIcon(key).pixmap(...)) as a
// GSTexture, for cases ImGuiFullscreen's own file-path-based texture cache
// doesn't cover (instance/account icons that only exist as in-memory QIcons
// or come from arbitrary absolute paths outside EmuFolders::Resources).
std::shared_ptr<GSTexture> UploadQImage(const QImage& image);

}  // namespace BigScreenGui
