// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's global `Console` logger object,
// covering only the subset the vendored ImGuiFullscreen toolkit calls.
#pragma once

#include <cstdarg>
#include <cstdio>

class ConsoleLogger {
public:
    void Error(const char* fmt, ...) const
    {
        va_list args;
        va_start(args, fmt);
        std::fprintf(stderr, "[BigScreen] ");
        std::vfprintf(stderr, fmt, args);
        std::fprintf(stderr, "\n");
        va_end(args);
    }

    void Warning(const char* fmt, ...) const
    {
        va_list args;
        va_start(args, fmt);
        std::fprintf(stderr, "[BigScreen] ");
        std::vfprintf(stderr, fmt, args);
        std::fprintf(stderr, "\n");
        va_end(args);
    }

    void WriteLn(const char* fmt, ...) const
    {
        va_list args;
        va_start(args, fmt);
        std::fprintf(stderr, "[BigScreen] ");
        std::vfprintf(stderr, fmt, args);
        std::fprintf(stderr, "\n");
        va_end(args);
    }
};

inline ConsoleLogger Console;
// PCSX2 distinguishes a "dev" console (verbose, stripped from release
// builds) from the always-on Console; BigScreen doesn't need that split yet.
inline ConsoleLogger& DevCon = Console;
