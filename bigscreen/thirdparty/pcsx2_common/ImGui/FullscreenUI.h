// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's ImGui/FullscreenUI.h, covering only
// the one symbol the vendored ImGuiFullscreen.cpp calls. PCSX2's real
// FullscreenUI is their own emulator settings/game-list screens built on
// top of ImGuiFullscreen — not vendored (see the BigScreen plan); BigScreen
// grows its own screens instead.
#pragma once

namespace FullscreenUI {

// Called when ImGuiFullscreen detects the connected gamepad's button layout
// changed (e.g. Xbox vs PlayStation vs Nintendo), so any layout-dependent UI
// can refresh. BigScreen doesn't have layout-dependent screens yet.
void GamepadLayoutChanged();

}  // namespace FullscreenUI
