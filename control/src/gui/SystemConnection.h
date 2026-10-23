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
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

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

    // True when the poll thread is running (a connect was requested).
    bool active() const { return m_Running; }

    // True when the poll connection is up (the engine answered a poll).
    bool pollConnected() const { return m_PollConnected; }

    // Sends a command on the UI thread's connection (lazy connect). On
    // failure `error` explains what happened; a dead command connection is
    // dropped so the next send retries.
    bool send(const std::string& method, const nlohmann::json& params,
              nlohmann::json& outResult, std::string& error, std::uint32_t timeoutMs = 5000);

    // The last poll snapshot (the UI reads it every frame).
    struct Snapshot
    {
        nlohmann::json status;  // result of the last successful "status"
        nlohmann::json scenes;  // result of the last successful "scene.list"
        bool haveStatus = false;
        std::string lastError;  // empty when the last poll was healthy
        double lastUpdate = 0.0; // steady_clock seconds of the last good poll
    };
    Snapshot snapshot() const;

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
};

} // namespace mediamax::gui
