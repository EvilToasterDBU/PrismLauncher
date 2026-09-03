// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's Input/InputManager.h, covering only
// the subset the vendored ImGuiFullscreen toolkit calls. BigScreen's own
// gamepad input handling (M1) lives elsewhere and does not go through this.
#pragma once

#include "common/Pcsx2Defs.h"

enum class InputLayout : u8 {
    Unknown,
    Generic,
    Xbox,
    PlayStation,
    Nintendo,
};

namespace InputManager {

// v1: no user-configurable icon preference yet, always defer to whatever
// controller BigScreen actually detected (ImGuiFullscreen falls back to
// Generic when this returns Unknown).
inline InputLayout GetGamepadIconPreference()
{
    return InputLayout::Unknown;
}

}  // namespace InputManager
