// SPDX-License-Identifier: GPL-3.0-only
#include "common/FileSystem.h"

#ifdef _WIN32

#include <windows.h>

std::vector<std::string> FileSystem::GetRootDirectoryList()
{
    std::vector<std::string> roots;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (mask & (1u << i))
            roots.push_back(std::string(1, static_cast<char>('A' + i)) + ":\\");
    }
    return roots;
}

#endif
