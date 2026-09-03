// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's GS/Renderers/Common/GSDevice.h,
// covering only the subset the vendored ImGuiFullscreen toolkit calls
// (texture creation/recycling for icons and cover art). See GSTexture.h.
#pragma once

#include "GSTexture.h"

#include <memory>

class GSDeviceCompat {
public:
    GSTexture* CreateTexture(u32 width, u32 height, u32 levels, GSTexture::Format format)
    {
        (void)levels;
        (void)format;
        GLuint handle = 0;
        glGenTextures(1, &handle);
        glBindTexture(GL_TEXTURE_2D, handle);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        return new GSTexture(width, height, handle);
    }

    void Recycle(GSTexture* texture) { delete texture; }
};

inline GSDeviceCompat g_gs_device_instance;
inline GSDeviceCompat* g_gs_device = &g_gs_device_instance;
