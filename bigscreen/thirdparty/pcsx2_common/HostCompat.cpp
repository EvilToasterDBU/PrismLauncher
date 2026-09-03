// SPDX-License-Identifier: GPL-3.0-only
// Definitions for the `Host::` functions the vendored ImGuiFullscreen.h
// (bigscreen/thirdparty/pcsx2_common/ImGui/ImGuiFullscreen.h) forward-
// declares for its file selector widget. v1 always uses ImGuiFullscreen's
// own in-UI file selector popup rather than a native OS dialog, so
// OpenHostFileSelectorAsync is unreachable and only needs to exist to link.
#include "ImGui/ImGuiFullscreen.h"

namespace Host {

bool ShouldPreferHostFileSelector()
{
    return false;
}

void OpenHostFileSelectorAsync(std::string_view, bool, FileSelectorCallback, FileSelectorFilters, std::string_view)
{
    // Unreachable: ShouldPreferHostFileSelector() always returns false in v1.
}

}  // namespace Host
