#include "gui/SystemConnection.h"

#include <cctype>
#include <chrono>
#include <set>

#include "mediamax/util/Base64.h"

namespace mediamax::gui
{

namespace
{

constexpr auto kPollIntervalTicks = 10;
constexpr auto kPollTick = std::chrono::milliseconds(50); // 10 x 50 ms = 500 ms cycle
constexpr std::uint32_t kConnectTimeoutMs = 2500;
constexpr std::uint32_t kPollTimeoutMs = 2000;

// Thumbnail fetch: the requested box (aspect-fit on the engine side). A 4K
// seek + decode can take a few seconds, so the budget is generous; at most
// one fetch is in flight per poll cycle (the rest wait), which keeps the
// status cadence roughly intact.
constexpr int kThumbnailWidth = 320;
constexpr int kThumbnailHeight = 180;
constexpr std::uint32_t kThumbnailTimeoutMs = 15000;
constexpr auto kThumbnailRetryDelay = std::chrono::seconds(10);
// Engine log lines may accumulate between UI pulls (the Log tab is hidden);
// the queue is capped to the newest of these.
constexpr std::size_t kLogPendingCap = 1000;

// Case-insensitive name comparison (the engine matches render device
// names case-insensitively; keep the re-apply decision consistent).
bool sameName(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

} // namespace

SystemConnection::SystemConnection(std::string name, std::string host, std::uint16_t port,
                                   std::string renderDevice)
    : m_Name(std::move(name)), m_Host(std::move(host)), m_Port(port),
      m_RenderDevice(std::move(renderDevice))
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
        m_Thumbnails.clear();
        m_PendingThumbnails.clear();
        m_ThumbnailRetry = std::chrono::steady_clock::now();
        // A fresh connection cycle: the engine (possibly restarted) must
        // be re-checked against the persisted render device selection.
        m_DeviceReapplied = false;
    }
    // A fresh connection cycle also starts the engine log from the current
    // ring: the UI already delivered (or never saw) whatever queued up
    // before, and a first sighting must not be reported as a restart.
    {
        std::lock_guard lock(m_LogMutex);
        m_LogSince = 0;
        m_LogStarted = 0.0;
        m_NewLogLines.clear();
    }
    m_LogTruncated.store(false);
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
        m_Thumbnails.clear();
        m_PendingThumbnails.clear();
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

            // Re-apply the persisted render device selection (system.
            // setRenderDevice): the selection must survive engine restarts.
            // The engine reports its current device in the status; a
            // mismatch is pushed to it here on the poll connection (the UI
            // never blocks on it). Skipped while the engine is mid-switch
            // and against engines that do not report render device
            // information (older builds).
            if (!m_DeviceReapplied)
            {
                std::string wanted;
                std::string current;
                bool hasInfo = false;
                bool switching = false;
                {
                    std::lock_guard lock(m_SnapMutex);
                    wanted = m_RenderDevice;
                    if (m_Snap.haveStatus && m_Snap.status.contains("renderDevice") &&
                        m_Snap.status["renderDevice"].is_object())
                    {
                        const auto& rd = m_Snap.status["renderDevice"];
                        hasInfo = true;
                        switching = rd.value("switching", false);
                        current = rd.value("current", std::string());
                    }
                }
                if (wanted.empty())
                {
                    m_DeviceReapplied = true; // no selection to apply
                }
                else if (hasInfo && !switching && sameName(wanted, current))
                {
                    m_DeviceReapplied = true; // the engine already runs it
                }
                else if (hasInfo && !switching)
                {
                    nlohmann::json deviceResult;
                    std::string deviceError;
                    if (m_PollClient.request("system.setRenderDevice", {{"device", wanted}},
                                             deviceResult, deviceError, kPollTimeoutMs))
                    {
                        m_DeviceReapplied = true; // the engine switches (async)
                    }
                    // Failure (engine busy, mid-reconnect): retry next cycle.
                }
                // !hasInfo or switching: wait for the engine to report its
                // devices / finish the in-flight switch.
            }

            nlohmann::json scenes;
            std::string sceneError;
            if (m_PollClient.request("scene.list", nlohmann::json::object(), scenes, sceneError,
                                     kPollTimeoutMs))
            {
                std::lock_guard lock(m_SnapMutex);
                m_Snap.scenes = std::move(scenes);
            }

            // frame.state: the show view's live data (display rects + the
            // resolved layer quads). A failure is not an error here: an
            // engine without the command (an older build) just never sets
            // haveFrameState and the show view falls back to status only.
            nlohmann::json frameState;
            std::string frameError;
            if (m_PollClient.request("frame.state", nlohmann::json::object(), frameState,
                                     frameError, kPollTimeoutMs))
            {
                std::lock_guard lock(m_SnapMutex);
                m_Snap.frameState = std::move(frameState);
                m_Snap.haveFrameState = true;
            }

            // log.tail: the engine's serious messages (warning and above)
            // for the control's Log tab, pulled with a sequence cursor so
            // each line is delivered exactly once. A failure is not an
            // error: an engine without the command (an older build) just
            // never delivers log lines.
            {
                std::uint64_t since = 0;
                {
                    std::lock_guard lock(m_LogMutex);
                    since = m_LogSince;
                }
                nlohmann::json logResult;
                std::string logError;
                if (m_PollClient.request("log.tail", {{"since", since}}, logResult, logError,
                                         kPollTimeoutMs))
                {
                    const std::uint64_t next = logResult.value("next", since);
                    const bool truncated = logResult.value("truncated", false);
                    const double started = logResult.value("started", 0.0);
                    std::lock_guard lock(m_LogMutex);
                    const bool firstSighting = (m_LogStarted == 0.0);
                    const bool newRun =
                        started != 0.0 && !firstSighting && started != m_LogStarted;
                    if (started != 0.0)
                    {
                        m_LogStarted = started;
                    }
                    if (newRun)
                    {
                        // The engine restarted: its sequence numbers went
                        // back to 1, so the old cursor is void (this
                        // response's entries were filtered against it -
                        // the next cycle re-fetches the fresh ring from
                        // the start). Mark the gap for the Log tab.
                        m_LogSince = 0;
                        m_LogTruncated.store(true);
                    }
                    else
                    {
                        m_LogSince = next;
                        if (truncated)
                        {
                            m_LogTruncated.store(true); // the ring dropped unseen lines
                        }
                        const auto entries =
                            logResult.value("entries", nlohmann::json::array());
                        for (const auto& e : entries)
                        {
                            m_NewLogLines.push_back({e.value("seq", std::uint64_t{0}),
                                                     e.value("level", std::string()),
                                                     e.value("time", 0.0),
                                                     e.value("message", std::string())});
                        }
                        // Cap the queue while the UI is not pulling (a
                        // hidden window): keep the newest lines.
                        while (m_NewLogLines.size() > kLogPendingCap)
                        {
                            m_NewLogLines.erase(m_NewLogLines.begin());
                        }
                    }
                }
            }

            // Thumbnails: drop cache entries for media the engine no longer
            // has, then fetch at most one pending request (a 4K decode can
            // block for seconds; one in flight keeps the status cadence
            // roughly intact).
            {
                std::pair<std::uint32_t, std::string> pending{0, {}};
                {
                    std::lock_guard lock(m_SnapMutex);
                    if (m_Snap.haveStatus && m_Snap.status.contains("media") &&
                        m_Snap.status["media"].is_array())
                    {
                        std::set<std::uint32_t> live;
                        for (const auto& m : m_Snap.status["media"])
                        {
                            if (m.contains("id"))
                            {
                                live.insert(static_cast<std::uint32_t>(m["id"].get<double>()));
                            }
                        }
                        for (auto it = m_Thumbnails.begin(); it != m_Thumbnails.end();)
                        {
                            it = live.count(it->first) ? std::next(it) : m_Thumbnails.erase(it);
                        }
                        for (auto it = m_PendingThumbnails.begin();
                             it != m_PendingThumbnails.end();)
                        {
                            it = live.count(it->first) ? std::next(it)
                                                       : m_PendingThumbnails.erase(it);
                        }
                    }
                    if (!m_PendingThumbnails.empty())
                    {
                        const auto it = m_PendingThumbnails.begin();
                        pending = {it->first, it->second};
                        m_PendingThumbnails.erase(it);
                    }
                }
                if (pending.first != 0)
                {
                    nlohmann::json thumb;
                    std::string thumbError;
                    const bool ok = m_PollClient.request(
                        "media.thumbnail",
                        {{"id", pending.first},
                         {"width", kThumbnailWidth},
                         {"height", kThumbnailHeight}},
                        thumb, thumbError, kThumbnailTimeoutMs);
                    std::lock_guard lock(m_SnapMutex);
                    if (ok)
                    {
                        const int w = thumb.value("width", 0);
                        const int h = thumb.value("height", 0);
                        std::vector<std::uint8_t> rgba;
                        if (thumb.contains("data") &&
                            mediamax::util::base64Decode(thumb["data"].get<std::string>(), rgba) &&
                            rgba.size() == static_cast<std::size_t>(w) * h * 4)
                        {
                            Thumbnail t;
                            t.path = pending.second;
                            t.width = w;
                            t.height = h;
                            t.rgba = std::move(rgba);
                            m_Thumbnails[pending.first] = std::move(t);
                        }
                        else
                        {
                            m_ThumbnailRetry =
                                std::chrono::steady_clock::now() + kThumbnailRetryDelay;
                        }
                    }
                    else
                    {
                        m_ThumbnailRetry =
                            std::chrono::steady_clock::now() + kThumbnailRetryDelay;
                    }
                }
            }
        }

        // The show view wants live data (10 Hz); otherwise the classic 500 ms
        // cadence.
        const int ticks = m_LivePoll ? 1 : kPollIntervalTicks;
        for (int tick = 0; tick < ticks && m_Running; ++tick)
        {
            std::this_thread::sleep_for(kPollTick);
        }
    }
}

void SystemConnection::requestThumbnail(std::uint32_t mediaId, const std::string& path)
{
    std::lock_guard lock(m_SnapMutex);
    if (!m_Running)
    {
        return; // nothing to serve it; the next connect starts fresh
    }
    const auto cached = m_Thumbnails.find(mediaId);
    if (cached != m_Thumbnails.end() && cached->second.path == path)
    {
        return; // already have it
    }
    if (m_PendingThumbnails.count(mediaId) > 0)
    {
        return; // a fetch is in flight
    }
    if (std::chrono::steady_clock::now() < m_ThumbnailRetry)
    {
        return; // a recent fetch failed; back off (the UI re-asks each frame)
    }
    m_PendingThumbnails[mediaId] = path;
}

bool SystemConnection::thumbnailFor(std::uint32_t mediaId, int& width, int& height,
                                    std::vector<std::uint8_t>& rgba) const
{
    std::lock_guard lock(m_SnapMutex);
    const auto it = m_Thumbnails.find(mediaId);
    if (it == m_Thumbnails.end())
    {
        return false;
    }
    width = it->second.width;
    height = it->second.height;
    rgba = it->second.rgba;
    return true;
}

std::vector<SystemConnection::LogLine> SystemConnection::takeNewLogLines()
{
    std::lock_guard lock(m_LogMutex);
    return std::move(m_NewLogLines);
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
    error.clear(); // success: never leave a stale message from an earlier call
    return true;
}

} // namespace mediamax::gui
