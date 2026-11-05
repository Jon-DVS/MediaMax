#pragma once

// The media deploy tracker: bookkeeping for the media transfers the control
// interface runs toward the playback systems' media caches (see
// net/MediaTransfer, the media.put command pair).
//
// The GUI's deploy feature runs each transfer on its own worker thread
// (transferMediaFile opens a short-lived connection), so the progress state
// lives here, thread-safe: the workers call begin/progress/finish, the GUI's
// render thread takes a snapshot (entries()) for the Systems tab's
// per-system progress list. Pure bookkeeping - no network, no engine access
// - which keeps it unit-testable in the core.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mediamax::net
{

// One media deploy (a transferMediaFile toward one system's cache) as the
// tracker records it.
struct MediaDeployEntry
{
    std::uint32_t handle = 0; // the worker's handle (begin/progress/finish)
    std::string systemName; // the target system's display name
    std::string fileName; // the local file's name (the UI's label)
    std::uint64_t bytesSent = 0; // transferred so far (monotone)
    std::uint64_t totalBytes = 0; // the file's size (0 when unknown)
    std::string state; // "sending", "done" or "failed"
    std::string error; // the failure message (empty unless failed)
};

// A thread-safe registry of in-flight and recently finished media deploys.
// Entry order is start order; finished entries are kept (the UI shows the
// outcome) until clearFinished()/reset()/clearSystem() drops them.
class MediaDeployTracker
{
public:
    // Registers a deploy that is about to run (state "sending", 0 bytes).
    // Returns the handle the worker passes to progress()/finish().
    std::uint32_t begin(const std::string& systemName, const std::string& fileName,
                        std::uint64_t totalBytes);

    // Updates the deploy's transferred byte count (kept monotone; clamped
    // to totalBytes when that is known). A no-op for an unknown or already
    // finished handle.
    void progress(std::uint32_t handle, std::uint64_t bytesSent);

    // Marks the deploy finished. An empty error means success (state
    // "done"); otherwise the state is "failed" and `error` is recorded.
    // A no-op for an unknown handle.
    void finish(std::uint32_t handle, const std::string& error = "");

    // A snapshot of all tracked deploys, in start order (the UI reads it
    // every frame; copies, so it is safe outside any lock).
    std::vector<MediaDeployEntry> entries() const;

    // Drops the finished deploys (the in-flight ones stay).
    void clearFinished();

    // Drops every deploy of the named system (it left the system list).
    void clearSystem(const std::string& systemName);

    // Drops everything (the system list was replaced, e.g. a new show
    // opened: every entry's system is gone or was renamed).
    void reset();

private:
    mutable std::mutex m_Mutex;
    std::vector<MediaDeployEntry> m_Entries; // under m_Mutex
    std::uint32_t m_NextHandle = 1; // under m_Mutex
};

} // namespace mediamax::net
