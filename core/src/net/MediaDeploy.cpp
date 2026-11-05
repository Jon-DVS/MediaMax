#include "mediamax/net/MediaDeploy.h"

#include <algorithm>

namespace mediamax::net
{

std::uint32_t MediaDeployTracker::begin(const std::string& systemName, const std::string& fileName,
                                        std::uint64_t totalBytes)
{
    std::lock_guard lock(m_Mutex);
    MediaDeployEntry entry;
    entry.handle = m_NextHandle++;
    entry.systemName = systemName;
    entry.fileName = fileName;
    entry.totalBytes = totalBytes;
    entry.state = "sending";
    m_Entries.push_back(std::move(entry));
    return m_Entries.back().handle;
}

void MediaDeployTracker::progress(std::uint32_t handle, std::uint64_t bytesSent)
{
    std::lock_guard lock(m_Mutex);
    for (auto& entry : m_Entries)
    {
        if (entry.handle == handle && entry.state == "sending")
        {
            entry.bytesSent = std::max(entry.bytesSent, bytesSent); // monotone
            if (entry.totalBytes > 0)
            {
                entry.bytesSent = std::min(entry.bytesSent, entry.totalBytes);
            }
            return;
        }
    }
}

void MediaDeployTracker::finish(std::uint32_t handle, const std::string& error)
{
    std::lock_guard lock(m_Mutex);
    for (auto& entry : m_Entries)
    {
        if (entry.handle != handle)
        {
            continue;
        }
        if (entry.state != "sending")
        {
            return; // already finished: keep the first outcome
        }
        if (error.empty())
        {
            entry.state = "done";
            if (entry.totalBytes > 0)
            {
                entry.bytesSent = entry.totalBytes; // a success sends everything
            }
        }
        else
        {
            entry.state = "failed";
            entry.error = error;
        }
        return;
    }
}

std::vector<MediaDeployEntry> MediaDeployTracker::entries() const
{
    std::lock_guard lock(m_Mutex);
    return m_Entries;
}

void MediaDeployTracker::clearFinished()
{
    std::lock_guard lock(m_Mutex);
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                                   [](const MediaDeployEntry& e) { return e.state != "sending"; }),
                    m_Entries.end());
}

void MediaDeployTracker::clearSystem(const std::string& systemName)
{
    std::lock_guard lock(m_Mutex);
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                                   [&systemName](const MediaDeployEntry& e) {
                                       return e.systemName == systemName;
                                   }),
                    m_Entries.end());
}

void MediaDeployTracker::reset()
{
    std::lock_guard lock(m_Mutex);
    m_Entries.clear();
}

} // namespace mediamax::net
