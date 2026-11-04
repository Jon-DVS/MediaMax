#pragma once

// Transferring a local media file to a playback system's media cache (the
// media.put / media.putChunk commands, see docs/PROTOCOL.md).
//
// The control interface's deploy feature runs this from worker threads:
// each transfer opens its own short-lived connection (the GUI's command
// connection is UI-thread only), so several files can deploy in parallel
// without touching the UI's client.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace mediamax::net
{

// Transfer progress: bytes sent so far and the total (known up front -
// the file's size). Called after each chunk completes; the already-current
// skip path calls it once with (total, total).
using MediaTransferProgressFunction =
    std::function<void(std::uint64_t bytesSent, std::uint64_t totalBytes)>;

// Transfers `localFile` to `cachePath` (relative to the system's media
// cache) on the system at `host:port`.
//
// Flow: hash the local file (sha256) -> media.cacheCheck (the transfer is
// skipped when the cache is already current) -> media.put (returns a
// transfer token, or state "current" / "conflict") -> stream the file in
// 1 MiB base64 chunks through media.putChunk (the server verifies the
// running sha256 on the final chunk and moves the temp file into place).
//
// Returns false (with `error`) on connect, protocol or verification
// failure; an in-flight stream is cancelled best-effort first. The
// progress callback may be empty.
bool transferMediaFile(const std::string& host, std::uint16_t port,
                       const std::filesystem::path& localFile, const std::string& cachePath,
                       bool overwrite, const MediaTransferProgressFunction& progress,
                       std::string& error);

} // namespace mediamax::net
