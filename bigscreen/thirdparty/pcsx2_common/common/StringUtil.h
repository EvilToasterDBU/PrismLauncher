// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's common/StringUtil.h, covering only
// the subset the vendored ImGuiFullscreen toolkit calls.
#pragma once

#include <cctype>
#include <cstring>

namespace StringUtil {

// Standard glob-style match: '*' matches any run of characters, '?' matches
// exactly one character.
inline bool WildcardMatch(const char* subject, const char* mask, bool case_sensitive = true)
{
    const char* s = subject;
    const char* m = mask;
    const char* star_mask = nullptr;
    const char* star_subject = nullptr;

    auto eq = [case_sensitive](char a, char b) {
        if (case_sensitive)
            return a == b;
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };

    while (*s) {
        if (*m == '?' || (*m && eq(*m, *s))) {
            ++s;
            ++m;
        } else if (*m == '*') {
            star_mask = m++;
            star_subject = s;
        } else if (star_mask) {
            m = star_mask + 1;
            s = ++star_subject;
        } else {
            return false;
        }
    }
    while (*m == '*')
        ++m;
    return *m == '\0';
}

}  // namespace StringUtil
