// SPDX-License-Identifier: GPL-3.0-only
#include "GLCompat.h"

#ifdef _WIN32

namespace BigScreenGL {

namespace {
using PFNGenSamplers = void(__stdcall*)(GLsizei, GLuint*);
using PFNBindSampler = void(__stdcall*)(GLuint, GLuint);
using PFNSamplerParameteri = void(__stdcall*)(GLuint, GLenum, GLint);

PFNGenSamplers s_genSamplers = nullptr;
PFNBindSampler s_bindSampler = nullptr;
PFNSamplerParameteri s_samplerParameteri = nullptr;
}  // namespace

void GenSamplers(GLsizei n, GLuint* samplers)
{
    if (s_genSamplers) {
        s_genSamplers(n, samplers);
        return;
    }
    // Leaves every handle as 0, which BindSampler() below and GL itself both
    // treat as "no sampler object — use the texture's own parameters".
    for (GLsizei i = 0; i < n; ++i)
        samplers[i] = 0;
}

void BindSampler(GLuint unit, GLuint sampler)
{
    if (s_bindSampler)
        s_bindSampler(unit, sampler);
}

void SamplerParameteri(GLuint sampler, GLenum pname, GLint param)
{
    if (s_samplerParameteri && sampler != 0)
        s_samplerParameteri(sampler, pname, param);
}

bool LoadExtensions(void* (*getProc)(const char*))
{
    s_genSamplers = reinterpret_cast<PFNGenSamplers>(getProc("glGenSamplers"));
    s_bindSampler = reinterpret_cast<PFNBindSampler>(getProc("glBindSampler"));
    s_samplerParameteri = reinterpret_cast<PFNSamplerParameteri>(getProc("glSamplerParameteri"));
    // All-or-nothing: a partial set (e.g. Gen without Bind) would create
    // sampler objects that never get used, so treat that as "unavailable".
    if (!s_genSamplers || !s_bindSampler || !s_samplerParameteri) {
        s_genSamplers = nullptr;
        s_bindSampler = nullptr;
        s_samplerParameteri = nullptr;
        return false;
    }
    return true;
}

}  // namespace BigScreenGL

#endif
