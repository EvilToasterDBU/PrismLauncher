// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's common/Timer.h, covering only the
// subset the vendored ImGuiFullscreen toolkit calls.
#pragma once

#include <chrono>
#include <cstdint>

namespace Common {

class Timer {
public:
    using Value = std::uint64_t;

    static Value GetCurrentValue()
    {
        return static_cast<Value>(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    static double ConvertValueToSeconds(Value value)
    {
        using Period = std::chrono::steady_clock::duration::period;
        return static_cast<double>(value) * Period::num / Period::den;
    }

    static Value ConvertSecondsToValue(double s)
    {
        using Period = std::chrono::steady_clock::duration::period;
        return static_cast<Value>(s * Period::den / Period::num);
    }
};

}  // namespace Common

// PCSX2 exposes Timer both as Common::Timer and (via a `using` in their
// Pcsx2Defs-adjacent headers) unqualified `Timer` — the vendored toolkit
// uses the unqualified name.
using Timer = Common::Timer;
