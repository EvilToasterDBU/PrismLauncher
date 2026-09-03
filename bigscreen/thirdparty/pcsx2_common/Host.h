// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's Host.h, covering only the
// declarations the vendored ImGuiFullscreen.cpp needs (matching what
// ImGui/ImGuiFullscreen.h itself forward-declares — see HostCompat.cpp for
// the definitions). BigScreen has no other use for PCSX2's much larger real
// Host abstraction (save states, audio, OSD messages, etc.).
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Host {

bool ShouldPreferHostFileSelector();

using FileSelectorCallback = std::function<void(const std::string& path)>;
using FileSelectorFilters = std::vector<std::string>;
// Defaults are specified once, on ImGuiFullscreen.h's matching declaration
// (included after this one) — repeating them here is an error in C++.
void OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                FileSelectorFilters filters, std::string_view initial_directory);

}  // namespace Host
