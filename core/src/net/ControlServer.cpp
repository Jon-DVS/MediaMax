#include "mediamax/net/ControlServer.h"

#include "mediamax/util/Logger.h"

namespace mediamax
{

ControlServer::ControlServer(std::string host, std::uint16_t port, std::uint32_t idleCheckMs)
    : m_Host(std::move(host))
    , m_Port(port)
    , m_IdleCheckMs(idleCheckMs)
{
}

bool ControlServer::start()
{
    if (!m_Listener.bind(m_Host, m_Port))
    {
        Logger::instance().error("Control server: failed to bind {}:{} ", m_Host, m_Port);
        return false;
    }
    m_Port = m_Listener.boundPort();
    m_Running->store(true);
    m_AcceptThread = std::thread([this] { acceptLoop(); });
    Logger::instance().info("Control server listening on {}:{} ", m_Host, m_Port);
    return true;
}

void ControlServer::stop()
{
    m_Stopping->store(true);
    m_Running->store(false);
    m_Listener.close(); // unblocks accept()

    if (m_AcceptThread.joinable())
    {
        m_AcceptThread.join();
    }

    // Close all client connections so in-flight client threads wake up and
    // terminate promptly, then join them: stop() completes only after every
    // in-flight command has finished.
    std::vector<std::shared_ptr<net::TcpConnection>> clients;
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(m_ClientsMutex);
        clients.swap(m_Clients);
        threads.swap(m_ClientThreads);
    }
    for (auto& client : clients)
    {
        client->close();
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    Logger::instance().info("Control server stopped");
}

void ControlServer::setHandler(CommandHandler handler)
{
    std::lock_guard lock(*m_HandlerMutex);
    *m_Handler = std::move(handler);
}

void ControlServer::acceptLoop()
{
    while (m_Running->load() && !m_Stopping->load())
    {
        net::TcpConnection connection;
        if (!m_Listener.accept(connection, 200))
        {
            continue;
        }
        if (!m_Running->load())
        {
            connection.close();
            break;
        }

        auto shared = std::make_shared<net::TcpConnection>(std::move(connection));
        std::thread clientThread([shared, running = m_Running, mutex = m_HandlerMutex,
                                  handler = m_Handler, idleCheckMs = m_IdleCheckMs]() {
            serveClient(shared, running, mutex, handler, idleCheckMs);
        });
        {
            std::lock_guard lock(m_ClientsMutex);
            m_Clients.push_back(shared);
            m_ClientThreads.push_back(std::move(clientThread));
        }
    }
}

void ControlServer::serveClient(std::shared_ptr<net::TcpConnection> client,
                                std::shared_ptr<std::atomic<bool>> running,
                                std::shared_ptr<std::mutex> handlerMutex,
                                std::shared_ptr<CommandHandler> handler, std::uint32_t idleCheckMs)
{
    Logger::instance().info("Control client connected");
    for (;;)
    {
        if (!running->load())
        {
            break;
        }
        std::string line;
        bool timedOut = false;
        if (!client->receiveLine(line, idleCheckMs, &timedOut))
        {
            if (timedOut)
            {
                // Idle: keep the connection. Command connections (the GUI's
                // interactive one in particular) sit between clicks for
                // minutes; closing them here made the next send fail.
                continue;
            }
            break; // EOF or error
        }
        if (line.empty())
        {
            continue;
        }

        protocol::Command cmd;
        protocol::Response resp;
        if (!protocol::decodeCommand(line, cmd))
        {
            resp = protocol::Response::failure(0, "invalid command");
        }
        else
        {
            std::lock_guard lock(*handlerMutex);
            if (*handler)
            {
                resp = (*handler)(cmd);
            }
            else
            {
                resp = protocol::Response::failure(cmd.id, "no command handler installed");
            }
        }

        if (!client->sendLine(protocol::encodeResponse(resp)))
        {
            break;
        }
    }
    client->close();
    Logger::instance().info("Control client disconnected");
}

} // namespace mediamax
