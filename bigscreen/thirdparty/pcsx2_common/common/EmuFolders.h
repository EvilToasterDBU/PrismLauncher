// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's global EmuFolders config, covering
// only the one path the vendored ImGuiFullscreen toolkit reads (where to
// look up relative icon/image asset paths).
#pragma once

#include <string>

namespace EmuFolders {

// Defined in EmuFoldersCompat.cpp. TODO(M1+): point this at BigScreen's own
// bundled resources directory once it has one; "resources" (relative to the
// working directory) is a placeholder.
extern std::string Resources;

}  // namespace EmuFolders
