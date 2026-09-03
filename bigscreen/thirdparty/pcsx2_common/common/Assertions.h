// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for the pxAssert*/pxFail* macros used by the
// vendored ImGuiFullscreen toolkit. Not a copy of PCSX2's file.
#pragma once

#include <cstdio>
#include <cstdlib>

inline void pxOnAssertFail(const char* file, int line, const char* func, const char* msg)
{
    std::fprintf(stderr, "Assertion failed (%s:%d in %s): %s\n", file, line, func, msg);
}

#define pxAssertMsg(cond, msg) \
    do { \
        if (!(cond)) { \
            pxOnAssertFail(__FILE__, __LINE__, __func__, msg); \
        } \
    } while (0)

#define pxAssert(cond) pxAssertMsg(cond, #cond)
#define pxAssumeMsg(cond, msg) pxAssertMsg(cond, msg)
#define pxAssume(cond) pxAssert(cond)
#define pxFail(msg) pxOnAssertFail(__FILE__, __LINE__, __func__, msg)
#define pxFailRel(msg) pxOnAssertFail(__FILE__, __LINE__, __func__, msg)
#define pxAssertRel(cond, msg) pxAssertMsg(cond, msg)
