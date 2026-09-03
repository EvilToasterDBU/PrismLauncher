// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's common/Image.h RGBA8Image, covering
// only the subset the vendored ImGuiFullscreen toolkit calls. Decodes with
// vendored stb_image (public domain) rather than PCSX2's real image backend.
#pragma once

#include "Pcsx2Defs.h"

#include <cstring>
#include <string>
#include <vector>

#include <stb_image.h>

class RGBA8Image {
public:
    RGBA8Image() = default;
    RGBA8Image(u32 width, u32 height, std::vector<u32> pixels) : m_width(width), m_height(height), m_pixels(std::move(pixels)) {}

    bool LoadFromBuffer(const char* path, const void* buffer, size_t buffer_size)
    {
        int width = 0, height = 0, channels = 0;
        stbi_uc* decoded =
            stbi_load_from_memory(static_cast<const stbi_uc*>(buffer), static_cast<int>(buffer_size), &width, &height, &channels, 4);
        if (!decoded)
            return false;

        m_width = static_cast<u32>(width);
        m_height = static_cast<u32>(height);
        m_pixels.resize(static_cast<size_t>(width) * height);
        std::memcpy(m_pixels.data(), decoded, m_pixels.size() * sizeof(u32));
        stbi_image_free(decoded);
        (void)path;
        return true;
    }

    u32 GetWidth() const { return m_width; }
    u32 GetHeight() const { return m_height; }
    const void* GetPixels() const { return m_pixels.data(); }
    u32 GetPitch() const { return m_width * sizeof(u32); }

private:
    u32 m_width = 0;
    u32 m_height = 0;
    std::vector<u32> m_pixels;
};
