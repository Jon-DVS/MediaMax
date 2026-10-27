#include "gui/SystemConnection.h"

#include <chrono>

namespace mediamax::gui
{

namespace
{

constexpr auto kPollIntervalTicks = 10;
constexpr auto kPollTick = std::chrono::milliseconds(50); // 10 x 50 ms = 500 ms cycle
constexpr std::uint32_t kConnectTimeoutMs = 2500;
constexpr std::uint32_t kPollTimeoutMs = 2000;

} // namespace

SystemConnection::SystemConnection(std::string name, std::string host, std::uint16_t port)
    : m_Name(std::move(name)), m_Host(std::move(host)), m_Port(port)
{
}

SystemConnection::~SystemConnection()
{
    disconnect();
}

void SystemConnection::connect()
{
    if (m_Running)
    {
        return;
    }
    {
        std::lock_guard lock(m_SnapMutex);
        m_Snap = Snapshot{};
        m_Snap.lastError = "connecting...";
    }
    m_Running = true;
    m_PollThread = std::thread([this] { pollLoop(); });
}

void SystemConnection::disconnect()
{
    m_Running = false;
    if (m_PollThread.joinable())
    {
        m_PollThread.join();
    }
    setPollConnected(false);
    m_PollClient.disconnect();
    m_CmdClient.disconnect();
    {
        std::lock_guard lock(m_SnapMutex);
        m_Snap = Snapshot{};
    }
}

void SystemConnection::retarget(std::string name, std::string host, std::uint16_t port)
{
    const bool wasRunning = m_Running;
    disconnect(); // stops the poll thread, which reads m_Host/m_Port
    m_Name = std::move(name);
    m_Host = std::move(host);
    m_Port = port;
    if (wasRunning)
    {
        connect();
    }
}

SystemConnection::Snapshot SystemConnection::snapshot() const
{
    std::lock_guard lock(m_SnapMutex);
    return m_Snap;
}

void SystemConnection::setPollConnected(bool value)
{
    m_PollConnected = value;
}

void SystemConnection::setLastError(const std::string& error)
{
    std::lock_guard lock(m_SnapMutex);
    m_Snap.lastError = error;
}

void SystemConnection::pollLoop()
{
    while (m_Running)
    {
        if (!m_PollClient.connected())
        {
            if (m_PollClient.connect(m_Host, m_Port, kConnectTimeoutMs))
            {
                setPollConnected(true);
                std::lock_guard lock(m_SnapMutex);
                m_Snap.lastError.clear();
            }
            else
            {
                setPollConnected(false);
                setLastError("cannot connect to " + m_Host + ":" + std::to_string(m_Port) +
                             " - is the playback engine running?");
            }
        }
        else
        {
            nlohmann::json result;
            std::string error;
            if (m_PollClient.request("status", nlohmann::json::object(), result, error,
                                     kPollTimeoutMs))
            {
                setPollConnected(true);
                const double now =
                    std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                std::lock_guard lock(m_SnapMutex);
                m_Snap.status = std::move(result);
                m_Snap.haveStatus = true;
                m_Snap.lastError.clear();
                m_Snap.lastUpdate = now;
            }
            else
            {
                // The socket died or the engine shut down mid-poll.
                m_PollClient.disconnect();
                setPollConnected(false);
                setLastError("poll failed: " + error);
            }

            nlohmann::json scenes;
            std::string sceneError;
            if (m_PollClient.request("scene.list", nlohmann::json::object(), scenes, sceneError,
                                     kPollTimeoutMs))
            {
                std::lock_guard lock(m_SnapMutex);
                m_Snap.scenes = std::move(scenes);
            }
        }

        for (int tick = 0; tick < kPollIntervalTicks && m_Running; ++tick)
        {
            std::this_thread::sleep_for(kPollTick);
        }
    }
}

bool SystemConnection::send(const std::string& method, const nlohmann::json& params,
                            nlohmann::json& outResult, std::string& error, std::uint32_t timeoutMs)
{
    if (!m_CmdClient.connected() &&
        !m_CmdClient.connect(m_Host, m_Port, kConnectTimeoutMs))
    {
        error = "not connected to " + m_Name + " (" + m_Host + ":" + std::to_string(m_Port) +
                ")";
        return false;
    }
    if (!m_CmdClient.request(method, params, outResult, error, timeoutMs))
    {
        m_CmdClient.disconnect(); // stale; the next send reconnects
        return false;
    }
    return true;
}

} // namespace mediamax::gui
