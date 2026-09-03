// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's GS/Renderers/Common/GSTexture.h,
// covering only the subset the vendored ImGuiFullscreen toolkit calls.
// PCSX2's real GSTexture abstracts over D3D11/D3D12/Vulkan/OpenGL/Metal;
// BigScreen only ever runs on OpenGL (v1 is Linux/SteamOS-only), so this is
// a plain GL texture, not a re-implementation of their backend abstraction.
#pragma once

#include "common/Pcsx2Defs.h"

#include <GL/gl.h>

struct GSVector4i {
    int x, y, z, w;
    GSVector4i(int x_, int y_, int z_, int w_) : x(x_), y(y_), z(z_), w(w_) {}
};

class GSTexture {
public:
    enum class Format {
        Color,
    };

    GSTexture(u32 width, u32 height, GLuint handle) : m_width(width), m_height(height), m_handle(handle) {}
    ~GSTexture() { glDeleteTextures(1, &m_handle); }

    u32 GetWidth() const { return m_width; }
    u32 GetHeight() const { return m_height; }
    GLuint GetNativeHandle() const { return m_handle; }

    bool Update(const GSVector4i& rect, const void* data, u32 pitch)
    {
        glBindTexture(GL_TEXTURE_2D, m_handle);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(pitch / sizeof(u32)));
        glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.z - rect.x, rect.w - rect.y, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        return true;
    }

private:
    u32 m_width;
    u32 m_height;
    GLuint m_handle;
};
