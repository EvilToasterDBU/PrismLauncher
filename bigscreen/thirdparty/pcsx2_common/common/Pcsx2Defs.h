// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim satisfying the subset of PCSX2's common/Pcsx2Defs.h
// that the vendored ImGuiFullscreen toolkit (bigscreen/thirdparty/pcsx2_common/ImGui)
// actually depends on. Not a copy of PCSX2's file — written fresh for BigScreen.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include <fmt/format.h>

// PCSX2 uses this as a forced-inline marker; BigScreen doesn't need the
// force-inline semantics, just something that's a valid specifier here.
#define __fi inline
#define __ri inline

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using sptr = std::intptr_t;
using uptr = std::uintptr_t;

// Minimal stand-in for PCSX2's common/SmallString.h SmallStringBase: the
// vendored ImGuiFullscreen toolkit only ever clear()s, empty()-checks, and
// append()s to one of these, so a thin std::string wrapper is sufficient.
class SmallStringBase : public std::string {
public:
    using std::string::string;
    using std::string::append;
    void append(char c) { push_back(c); }
    const char* end_ptr() const { return data() + size(); }
};

// Minimal stand-in for PCSX2's concrete SmallString (extends SmallStringBase
// with a printf/fmt-style factory). The vendored toolkit only ever calls
// SmallString::from_format(...).c_str(), nothing else.
class SmallString : public SmallStringBase {
public:
    template <typename... Args>
    static SmallString from_format(fmt::format_string<Args...> fmt, Args&&... args)
    {
        SmallString s;
        s.assign(fmt::format(fmt, std::forward<Args>(args)...));
        return s;
    }
};

#ifndef DeclareNoncopyableObject
#define DeclareNoncopyableObject(classname) \
    public: \
        classname(const classname&) = delete; \
        classname& operator=(const classname&) = delete
#endif
