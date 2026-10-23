#include "mediamax/util/File.h"

#include <fstream>

#include "mediamax/util/Logger.h"

namespace mediamax
{

std::optional<std::string> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return contents;
}

bool writeFile(const std::filesystem::path& path, const std::string& contents)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        Logger::instance().error("Failed to open file for writing: {}", path.string());
        return false;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(out);
}

bool writeBinaryFile(const std::filesystem::path& path, const void* data, size_t size)
{
    return writeFile(path, std::string(static_cast<const char*>(data), size));
}

std::optional<std::vector<unsigned char>> readBinaryFile(const std::filesystem::path& path)
{
    auto text = readFile(path);
    if (!text)
    {
        return std::nullopt;
    }
    return std::vector<unsigned char>(text->begin(), text->end());
}

} // namespace mediamax
