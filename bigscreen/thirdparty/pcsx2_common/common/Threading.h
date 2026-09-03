// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's common/Threading.h, covering only
// the subset the vendored ImGuiFullscreen toolkit calls (a single named
// background thread for async texture loading).
#pragma once

#include <functional>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#endif

namespace Threading {

inline void SetNameOfCurrentThread(const char* name)
{
#ifdef __linux__
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

class Thread {
public:
    Thread() = default;
    ~Thread()
    {
        if (Joinable())
            Join();
    }

    void Start(std::function<void()> entry_point)
    {
        m_thread = std::thread(std::move(entry_point));
    }

    bool Joinable() const { return m_thread.joinable(); }
    void Join() { m_thread.join(); }

private:
    std::thread m_thread;
};

}  // namespace Threading
