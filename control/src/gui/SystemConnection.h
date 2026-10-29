#pragma once

// A per-system control connection for the GUI.
//
// Two independent control-protocol connections, so the UI never blocks on a
// stale poll:
//   - the poll connection is owned by a background thread that refreshes the
//     "status" + "scene.list" snapshot every 500 ms;
//   - the command connection is owned by the UI thread (lazy connect) and
//     carries interactive commands.
// The snapshot is shared under a mutex; each ControlClient is touched by
// exactly one thread.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "mediamax/net/ControlClient.h"

namespace mediamax::gui
{

class SystemConnection
{
public:
    SystemConnection(std::string name, std::string host, std::uint16_t port);
    ~SystemConnection();

    SystemConnection(const SystemConnection&) = delete;
    SystemConnection& operator=(const SystemConnection&) = delete;

    const std::string& name() const { return m_Name; }
    const std::string& host() const { return m_Host; }
    std::uint16_t port() const { return m_Port; }

    // Starts the poll thread (which dials the poll connection). Idempotent.
    void connect();

    // Stops the poll thread and drops both connections.
    void disconnect();

    // Re-targets the entry: the identity (name) and the address
    // (host:port). Stops the poll thread first (it reads m_Host/m_Port on
    // every (re)connect, so the fields must not change under it), updates
    // the fields, and restarts the thread when it was running (issue 16:
    // the system settings dialog).
    void retarget(std::string name, std::string host, std::uint16_t port);

    // True when the poll thread is running (a connect was requested).
    bool active() const { return m_Running; }

    // True when the poll connection is up (the engine answered a poll).
    bool pollConnected() const { return m_PollConnected; }

    // Sends a command on the UI thread's connection (lazy connect). On
    // failure `error` explains what happened; a dead command connection is
    // dropped so the next send retries.
    bool send(const std::string& method, const nlohmann::json& params,
              nlohmann::json& outResult, std::string& error, std::uint32_t timeoutMs = 5000);

    // A cached media thumbnail (RGBA8). Fetched by the poll thread through
    // the media.thumbnail command for the show view's layer previews.
    struct Thumbnail
    {
        std::string path; // the asset path the thumbnail was made from
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;
    };

    // The last poll snapshot (the UI reads it every frame).
    struct Snapshot
    {
        nlohmann::json status;     // result of the last successful "status"
        nlohmann::json scenes;     // result of the last successful "scene.list"
        nlohmann::json frameState; // result of the last successful "frame.state"
        bool haveStatus = false;
        bool haveFrameState = false;
        std::string lastError;  // empty when the last poll was healthy
        double lastUpdate = 0.0; // steady_clock seconds of the last good poll
    };
    Snapshot snapshot() const;

    // Asks the poll thread to fetch the thumbnail of the given media asset
    // (the show view's layer previews). Cheap and idempotent: a no-op when
    // the thumbnail is already cached for that path or a fetch is in flight;
    // a failed fetch backs off (kThumbnailRetryDelay) before retrying.
    void requestThumbnail(std::uint32_t mediaId, const std::string& path);

    // Copies the cached thumbnail of `mediaId` (a single entry, so the UI
    // pays the RGBA copy only when its own texture cache misses).
    bool thumbnailFor(std::uint32_t mediaId, int& width, int& height,
                      std::vector<std::uint8_t>& rgba) const;

    // While true the poll loop sleeps one 50 ms tick per cycle (10 Hz)
    // instead of 500 ms, so the show view animates smoothly. Set by the UI
    // thread while the show view window is visible (atomic; no lock).
    void setLivePoll(bool live) { m_LivePoll.store(live); }

private:
    void pollLoop();
    void setPollConnected(bool value);
    void setLastError(const std::string& error);

    std::string m_Name;
    std::string m_Host;
    std::uint16_t m_Port;

    mediamax::ControlClient m_PollClient; // poll thread only
    mediamax::ControlClient m_CmdClient;  // UI thread only
    std::thread m_PollThread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_PollConnected{false};

    mutable std::mutex m_SnapMutex;
    Snapshot m_Snap;
    std::map<std::uint32_t, Thumbnail> m_Thumbnails;              // under m_SnapMutex
    std::map<std::uint32_t, std::string> m_PendingThumbnails;     // id -> path, under m_SnapMutex
    std::chrono::steady_clock::time_point m_ThumbnailRetry{std::chrono::steady_clock::now()}; // under
                                                                  // m_SnapMutex
    std::atomic<bool> m_LivePoll{false};
};

} // namespace mediamax::gui
