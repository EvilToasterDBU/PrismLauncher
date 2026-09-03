// SPDX-License-Identifier: GPL-3.0-only
// Minimal compatibility shim for PCSX2's common/FileSystem.h, covering only
// the subset the vendored ImGuiFullscreen toolkit calls (file selector
// widget + binary asset loading). Linux-only, implemented on std::filesystem.
#pragma once

#include "Pcsx2Defs.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Linux-only, so always '/' (PCSX2's real header picks '\\' on Windows).
constexpr char FS_OSPATH_SEPARATOR_CHARACTER = '/';

// Only need these to exist for signature compatibility; the vendored code
// never dereferences them in the paths BigScreen uses (v1 has no cancellable
// long-running filesystem scans, and never surfaces detailed I/O errors here).
class Error {};
class ProgressCallback {};

enum FILESYSTEM_FILE_ATTRIBUTES {
    FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY = 1,
    FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY = 2,
    FILESYSTEM_FILE_ATTRIBUTE_COMPRESSED = 4,
};

enum FILESYSTEM_FIND_FLAGS {
    FILESYSTEM_FIND_RECURSIVE = (1 << 0),
    FILESYSTEM_FIND_RELATIVE_PATHS = (1 << 1),
    FILESYSTEM_FIND_HIDDEN_FILES = (1 << 2),
    FILESYSTEM_FIND_FOLDERS = (1 << 3),
    FILESYSTEM_FIND_FILES = (1 << 4),
    FILESYSTEM_FIND_KEEP_ARRAY = (1 << 5),
    FILESYSTEM_FIND_SORT_BY_NAME = (1 << 6),
};

struct FILESYSTEM_FIND_DATA {
    std::time_t CreationTime = 0;
    std::time_t ModificationTime = 0;
    std::string FileName;
    s64 Size = 0;
    u32 Attributes = 0;
};

namespace FileSystem {

using FindResultsArray = std::vector<FILESYSTEM_FIND_DATA>;

inline std::vector<std::string> GetRootDirectoryList()
{
    return { "/" };
}

inline bool DirectoryExists(const char* path)
{
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

inline std::string GetWorkingDirectory()
{
    std::error_code ec;
    return std::filesystem::current_path(ec).string();
}

inline bool FindFiles(const char* path, const char* pattern, u32 flags, FindResultsArray* results, ProgressCallback* = nullptr)
{
    if (!(flags & FILESYSTEM_FIND_KEEP_ARRAY))
        results->clear();

    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec))
        return false;

    const auto add_entry = [&](const std::filesystem::directory_entry& entry) {
        std::error_code entry_ec;
        const bool is_dir = entry.is_directory(entry_ec);
        if (is_dir && !(flags & FILESYSTEM_FIND_FOLDERS))
            return;
        if (!is_dir && !(flags & FILESYSTEM_FIND_FILES))
            return;

        const std::string name = entry.path().filename().string();
        if (!(flags & FILESYSTEM_FIND_HIDDEN_FILES) && !name.empty() && name[0] == '.')
            return;

        FILESYSTEM_FIND_DATA fd;
        fd.FileName = (flags & FILESYSTEM_FIND_RELATIVE_PATHS) ? name : entry.path().string();
        fd.Attributes = is_dir ? FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY : 0;
        std::error_code size_ec;
        fd.Size = is_dir ? 0 : static_cast<s64>(entry.file_size(size_ec));
        results->push_back(std::move(fd));
    };

    if (flags & FILESYSTEM_FIND_RECURSIVE) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path, ec))
            add_entry(entry);
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(path, ec))
            add_entry(entry);
    }

    if (flags & FILESYSTEM_FIND_SORT_BY_NAME) {
        std::sort(results->begin(), results->end(),
                  [](const FILESYSTEM_FIND_DATA& a, const FILESYSTEM_FIND_DATA& b) { return a.FileName < b.FileName; });
    }

    return true;
}

inline std::optional<std::vector<u8>> ReadBinaryFile(const char* filename)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return std::nullopt;

    const std::streamsize size = file.tellg();
    if (size < 0)
        return std::nullopt;
    file.seekg(0, std::ios::beg);

    std::vector<u8> data(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size))
        return std::nullopt;
    return data;
}

inline std::optional<std::vector<u8>> ReadBinaryFile(std::FILE* fp)
{
    if (!fp)
        return std::nullopt;

    const long start = std::ftell(fp);
    if (start < 0 || std::fseek(fp, 0, SEEK_END) != 0)
        return std::nullopt;
    const long end = std::ftell(fp);
    if (end < 0 || std::fseek(fp, start, SEEK_SET) != 0)
        return std::nullopt;

    std::vector<u8> data(static_cast<size_t>(end - start));
    if (!data.empty() && std::fread(data.data(), 1, data.size(), fp) != data.size())
        return std::nullopt;
    return data;
}

}  // namespace FileSystem
