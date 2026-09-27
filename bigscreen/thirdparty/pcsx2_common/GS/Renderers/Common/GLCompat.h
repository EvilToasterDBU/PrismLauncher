// SPDX-License-Identifier: GPL-3.0-only
// The one place BigScreen's own GL code (GSTexture/GSDevice, the sampler
// overrides in ImGuiFullscreen.cpp, main.cpp's clear/viewport) gets its GL
// declarations from, so the per-platform difference lives here only.
//
// Linux: GLES3 — the whole Linux build runs a GLES3 context (see main.cpp;
// real ARM hardware only exposes GLES through EGL), and GLES3's header
// declares everything used here, sampler objects included.
//
// Windows: desktop GL. opengl32.lib only exports GL 1.1 entry points, and
// everything used here is GL 1.1 except the three GL 3.3 sampler-object
// functions — those are loaded at runtime through LoadExtensions() (called
// from main.cpp right after the context is created) rather than pulling in a
// whole loader library for three functions. Deliberately does NOT include
// <windows.h>: its macros (DrawText, CreateWindow, min/max, ...) would
// silently rename identifiers in every TU that includes this header but not
// in the ones that don't. <GL/gl.h> only needs APIENTRY/WINGDIAPI, which
// are defined just for its duration below (the same technique glfw3.h uses).
#pragma once

#ifdef _WIN32

#ifndef APIENTRY
#define APIENTRY __stdcall
#define BIGSCREEN_DEFINED_APIENTRY
#endif
#ifndef WINGDIAPI
#define WINGDIAPI __declspec(dllimport)
#define BIGSCREEN_DEFINED_WINGDIAPI
#endif

#include <GL/gl.h>

#ifdef BIGSCREEN_DEFINED_APIENTRY
#undef APIENTRY
#undef BIGSCREEN_DEFINED_APIENTRY
#endif
#ifdef BIGSCREEN_DEFINED_WINGDIAPI
#undef WINGDIAPI
#undef BIGSCREEN_DEFINED_WINGDIAPI
#endif

namespace BigScreenGL {

// Each is a safe no-op when the driver doesn't provide the function (a
// pre-3.3 context): textures then just fall back to their own per-texture
// GL_TEXTURE_MIN/MAG_FILTER, which is also what Dear ImGui's backend does
// on such a context, since it only binds sampler objects on GL 3.3+.
void GenSamplers(GLsizei n, GLuint* samplers);
void BindSampler(GLuint unit, GLuint sampler);
void SamplerParameteri(GLuint sampler, GLenum pname, GLint param);

// getProc is SDL_GL_GetProcAddress in practice; taken as a plain function
// pointer so this toolkit doesn't depend on SDL itself. Returns whether all
// three sampler functions were found.
bool LoadExtensions(void* (*getProc)(const char*));

}  // namespace BigScreenGL

#define glGenSamplers BigScreenGL::GenSamplers
#define glBindSampler BigScreenGL::BindSampler
#define glSamplerParameteri BigScreenGL::SamplerParameteri

#else

#include <GLES3/gl3.h>

namespace BigScreenGL {
// Nothing to load: GLES3 declares and exports everything directly.
inline bool LoadExtensions(void* (*)(const char*))
{
    return true;
}
}  // namespace BigScreenGL

#endif
