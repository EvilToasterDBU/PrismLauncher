// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's GS/Renderers/Common/GSDevice.h,
// covering only the subset the vendored ImGuiFullscreen toolkit calls
// (texture creation/recycling for icons and cover art). See GSTexture.h.
#pragma once

#include "GSTexture.h"

#include <memory>

class GSDeviceCompat {
public:
    // nearest=true skips the linear min/mag filtering every other texture in
    // this app uses (icons, screenshots, gallery thumbnails — ordinary
    // smooth-edged raster images that look better interpolated when scaled)
    // — Minecraft skin/cape textures are tiny, deliberately blocky pixel art
    // that Minecraft itself always renders with nearest-neighbor sampling;
    // linear-filtering one when it's blown up to a 150px preview is exactly
    // what produces a blurry, smeared look instead of crisp pixels.
    GSTexture* CreateTexture(u32 width, u32 height, u32 levels, GSTexture::Format format, bool nearest = false)
    {
        (void)levels;
        (void)format;
        GLuint handle = 0;
        glGenTextures(1, &handle);
        glBindTexture(GL_TEXTURE_2D, handle);
        const GLint filter = nearest ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        return new GSTexture(width, height, handle);
    }

    void Recycle(GSTexture* texture) { delete texture; }
};

inline GSDeviceCompat g_gs_device_instance;
inline GSDeviceCompat* g_gs_device = &g_gs_device_instance;
