#pragma once

// Small file IO helpers. Returns false instead of throwing so that callers
// in the media pipeline can handle missing media files gracefully.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mediamax
{

// Reads the entire file at `path` into a string. Returns nullopt on failure.
std::optional<std::string> readFile(const std::filesystem::path& path);

// Writes `contents` to `path`, creating parent directories as needed.
// Overwrites an existing file. Returns false on failure.
bool writeFile(const std::filesystem::path& path, const std::string& contents);

// Writes `data` verbatim to `path` (binary safe). Returns false on failure.
bool writeBinaryFile(const std::filesystem::path& path, const void* data, size_t size);

// Reads `path` as binary. Returns nullopt on failure.
std::optional<std::vector<unsigned char>> readBinaryFile(const std::filesystem::path& path);

} // namespace mediamax
