// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's common/Path.h, covering only the
// subset the vendored ImGuiFullscreen toolkit calls. Linux-only (BigScreen
// v1 targets Linux/SteamOS), so no Windows drive-letter/UNC handling.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Path {

inline std::string Combine(const std::string_view base, const std::string_view next)
{
    if (base.empty())
        return std::string(next);
    if (next.empty())
        return std::string(base);
    std::filesystem::path p(base);
    p /= next;
    return p.lexically_normal().string();
}

inline bool IsAbsolute(const std::string_view path)
{
    return std::filesystem::path(path).is_absolute();
}

}  // namespace Path
