#include "mediamax/net/MediaTransfer.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

#include "mediamax/net/ControlClient.h"
#include "mediamax/util/Base64.h"
#include "mediamax/util/Sha256.h"

namespace mediamax::net
{

namespace
{

// One media.putChunk payload: the server's per-chunk limit, so a file is
// never split coarser than the protocol allows (and each request stays
// well under the protocol's line limit).
constexpr std::size_t kChunkBytes = 1u << 20; // 1 MiB

// Chunk requests carry up to 1.35 MiB of JSON: allow generous round-trip
// times for slow links without stalling the whole transfer.
constexpr std::uint32_t kChunkTimeoutMs = 30000;

} // namespace

bool transferMediaFile(const std::string& host, std::uint16_t port,
                       const std::filesystem::path& localFile, const std::string& cachePath,
                       bool overwrite, const MediaTransferProgressFunction& progress,
                       std::string& error)
{
    // 1. Hash and measure the local file (the checksum is what the server
    // verifies the landed file against).
    std::error_code ec;
    if (!std::filesystem::is_regular_file(localFile, ec) || ec)
    {
        error = "cannot read " + localFile.string() + " for transfer";
        return false;
    }
    const std::uint64_t bytes = std::filesystem::file_size(localFile, ec);
    if (ec)
    {
        error = "cannot stat " + localFile.string() + ": " + ec.message();
        return false;
    }
    std::string checksum;
    if (!util::sha256File(localFile, checksum, error))
    {
        return false;
    }

    ControlClient client;
    if (!client.connect(host, port, 5000))
    {
        error = "cannot reach " + host + ":" + std::to_string(port);
        return false;
    }

    nlohmann::json result;

    // 2. Pre-check: skip the transfer when the cache already is current.
    // A cacheCheck failure (no cache configured, a bad path) is fatal -
    // media.put would fail the same way.
    if (!client.request("media.cacheCheck", {{"path", cachePath}, {"checksum", checksum}},
                        result, error))
    {
        return false;
    }
    if (result.value("present", false))
    {
        if (progress)
        {
            progress(bytes, bytes);
        }
        return true;
    }

    // 3. Start the transfer (token, or state "current" / "conflict").
    if (!client.request("media.put",
                        {{"path", cachePath},
                         {"bytes", bytes},
                         {"checksum", checksum},
                         {"overwrite", overwrite}},
                        result, error))
    {
        return false;
    }
    if (result.value("state", std::string()) == "current")
    {
        if (progress)
        {
            progress(bytes, bytes);
        }
        return true;
    }
    if (!result.contains("token"))
    {
        error = "media.put did not return a transfer token: " +
                result.value("error", std::string("unexpected response"));
        return false;
    }
    const std::uint32_t token = result.at("token").get<std::uint32_t>();

    // The stream is ours until it completes or we give up: cancel it on
    // the way out of every failure (best effort - the server may already
    // have torn it down).
    const auto cancelBestEffort = [&]() {
        nlohmann::json cancelResult;
        std::string cancelError;
        client.request("media.putCancel", {{"token", token}}, cancelResult, cancelError, 2000);
    };

    // 4. Stream the file, 1 MiB chunks, the last one flagged done.
    std::ifstream in(localFile, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + localFile.string() + " for transfer";
        cancelBestEffort();
        return false;
    }
    std::vector<std::uint8_t> chunk(kChunkBytes);
    std::uint64_t offset = 0;
    for (;;)
    {
        const std::uint64_t remaining = bytes - offset;
        const std::size_t n =
            remaining < kChunkBytes ? static_cast<std::size_t>(remaining) : kChunkBytes;
        if (n > 0)
        {
            in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(n));
            if (in.gcount() != static_cast<std::streamsize>(n))
            {
                error = "read failed: " + localFile.string();
                cancelBestEffort();
                return false;
            }
        }
        const bool done = offset + n == bytes;
        if (!client.request("media.putChunk",
                            {{"token", token},
                             {"offset", offset},
                             {"data", util::base64Encode(chunk.data(), n)},
                             {"done", done}},
                            result, error, kChunkTimeoutMs))
        {
            cancelBestEffort();
            return false;
        }
        offset += n;
        if (progress)
        {
            progress(offset, bytes);
        }
        if (done)
        {
            if (result.value("state", std::string()) != "done")
            {
                error = "the transfer ended in state '" +
                        result.value("state", std::string("?")) + "'";
                return false; // the server already cleaned up its side
            }
            return true;
        }
    }
}

} // namespace mediamax::net
