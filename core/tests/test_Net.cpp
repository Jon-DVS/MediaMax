// TCP layer and the control server/client pair, tested over loopback.
//
// All listeners bind 127.0.0.1 with port 0 (ephemeral) so tests never clash
// with each other or with a running playback engine.

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "mediamax/net/ControlClient.h"
#include "mediamax/net/ControlServer.h"
#include "mediamax/net/Tcp.h"
#include "mediamax/protocol/Protocol.h"

using namespace mediamax;
using namespace std::chrono_literals;

namespace
{

// A handler that records every method it sees and answers with the echo text.
struct TestHandler
{
    std::mutex m_Mutex;
    std::vector<std::string> m_Methods;

    protocol::Response handle(const protocol::Command& cmd)
    {
        {
            std::lock_guard lock(m_Mutex);
            m_Methods.push_back(cmd.method);
        }
        if (cmd.method == "echo")
        {
            nlohmann::json result;
            result["echo"] = cmd.params.value("text", std::string());
            return protocol::Response::success(cmd.id, std::move(result));
        }
        if (cmd.method == "fail")
        {
            return protocol::Response::failure(cmd.id, "intentional failure");
        }
        if (cmd.method == "sleep")
        {
            // Hold the handler long enough to prove requests are serialized.
            std::this_thread::sleep_for(50ms);
            return protocol::Response::success(cmd.id, {{"ok", true}});
        }
        return protocol::Response::failure(cmd.id, "unknown method: " + cmd.method);
    }

    std::vector<std::string> methods()
    {
        std::lock_guard lock(m_Mutex);
        return m_Methods;
    }
};

} // namespace

TEST_SUITE("Net")
{

TEST_CASE("TcpListener: bind on an ephemeral loopback port")
{
    net::TcpListener listener;
    CHECK(listener.bind("127.0.0.1", 0));
    CHECK(listener.valid());
    CHECK(listener.boundPort() != 0);
    listener.close();
    CHECK_FALSE(listener.valid());
}

TEST_CASE("TcpConnection: connect to a closed port fails quickly")
{
    // Find a port that is not listening.
    net::TcpListener probe;
    REQUIRE(probe.bind("127.0.0.1", 0));
    const auto port = probe.boundPort();
    probe.close();

    net::TcpConnection connection;
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(connection.connect("127.0.0.1", port, 2000));
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    CHECK_FALSE(connection.valid());
}

TEST_CASE("TcpConnection: line round trip over loopback")
{
    net::TcpListener listener;
    REQUIRE(listener.bind("127.0.0.1", 0));
    const auto port = listener.boundPort();

    net::TcpConnection client;
    REQUIRE(client.connect("127.0.0.1", port, 3000));

    net::TcpConnection serverSide;
    REQUIRE(listener.accept(serverSide, 3000));

    // Client -> server, including a multi-buffer payload (larger than the
    // 8 KiB receive chunks).
    const std::string longLine(200'000, 'x');
    REQUIRE(client.sendLine(longLine));
    std::string received;
    REQUIRE(serverSide.receiveLine(received, 3000));
    CHECK(received == longLine);

    // Server -> client with CRLF terminator.
    REQUIRE(serverSide.sendAll("line-one\r\n", 10));
    REQUIRE(serverSide.sendLine("line-two"));
    REQUIRE(serverSide.sendLine(longLine));

    std::string line;
    REQUIRE(serverSide.valid());
    REQUIRE(client.receiveLine(line, 3000));
    CHECK(line == "line-one"); // \r stripped
    REQUIRE(client.receiveLine(line, 3000));
    CHECK(line == "line-two");
    REQUIRE(client.receiveLine(line, 3000));
    CHECK(line == longLine);
}

TEST_CASE("TcpConnection: receiveLine times out on a quiet connection")
{
    net::TcpListener listener;
    REQUIRE(listener.bind("127.0.0.1", 0));
    const auto port = listener.boundPort();

    net::TcpConnection client;
    REQUIRE(client.connect("127.0.0.1", port, 3000));
    net::TcpConnection serverSide;
    REQUIRE(listener.accept(serverSide, 3000));

    const auto start = std::chrono::steady_clock::now();
    std::string line;
    CHECK_FALSE(client.receiveLine(line, 150));
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start).count() >= 100);
}

TEST_CASE("TcpConnection: receiveLine distinguishes timeout from EOF")
{
    net::TcpListener listener;
    REQUIRE(listener.bind("127.0.0.1", 0));
    const auto port = listener.boundPort();

    net::TcpConnection client;
    REQUIRE(client.connect("127.0.0.1", port, 3000));
    net::TcpConnection serverSide;
    REQUIRE(listener.accept(serverSide, 3000));

    std::string line;
    bool timedOut = false;
    CHECK_FALSE(client.receiveLine(line, 150, &timedOut));
    CHECK(timedOut); // timeout, not a hard failure
    CHECK(client.valid()); // the connection is still usable

    serverSide.close();
    timedOut = true; // must be reset on a hard failure
    CHECK_FALSE(client.receiveLine(line, 3000, &timedOut));
    CHECK_FALSE(timedOut);
}

TEST_CASE("TcpConnection: EOF is detected after the peer closes")
{
    net::TcpListener listener;
    REQUIRE(listener.bind("127.0.0.1", 0));
    const auto port = listener.boundPort();

    net::TcpConnection client;
    REQUIRE(client.connect("127.0.0.1", port, 3000));
    net::TcpConnection serverSide;
    REQUIRE(listener.accept(serverSide, 3000));

    serverSide.close();
    std::string line;
    CHECK_FALSE(client.receiveLine(line, 3000));
}

TEST_CASE("TcpConnection: sending on a closed connection fails")
{
    net::TcpConnection connection;
    CHECK_FALSE(connection.sendLine("hello"));
    CHECK_FALSE(connection.sendAll("abc", 3));
}

// --- ControlServer + ControlClient end-to-end --------------------------------

TEST_CASE("ControlServer: start, request, stop")
{
    TestHandler handler;
    ControlServer server("127.0.0.1", 0);
    server.setHandler([&handler](const protocol::Command& cmd) { return handler.handle(cmd); });
    REQUIRE(server.start());
    CHECK(server.running());
    CHECK(server.port() != 0);

    ControlClient client;
    std::string error;
    REQUIRE(client.connect("127.0.0.1", server.port(), 3000));
    CHECK(client.connected());

    nlohmann::json result;
    REQUIRE(client.request("echo", {{"text", "hello world"}}, result, error));
    CHECK(result.at("echo").get<std::string>() == "hello world");

    server.stop();
    CHECK_FALSE(server.running());
}

TEST_CASE("ControlServer: failure responses carry the error text")
{
    TestHandler handler;
    ControlServer server("127.0.0.1", 0);
    server.setHandler([&handler](const protocol::Command& cmd) { return handler.handle(cmd); });
    REQUIRE(server.start());

    ControlClient client;
    std::string error;
    nlohmann::json result;
    REQUIRE(client.connect("127.0.0.1", server.port(), 3000));

    CHECK_FALSE(client.request("fail", {}, result, error));
    CHECK(error == "intentional failure");

    // Unknown method also fails cleanly.
    CHECK_FALSE(client.request("nope", {}, result, error));
    CHECK(error == "unknown method: nope");
}

TEST_CASE("ControlServer: request ids increment and correlate")
{
    TestHandler handler;
    ControlServer server("127.0.0.1", 0);
    server.setHandler([&handler](const protocol::Command& cmd) { return handler.handle(cmd); });
    REQUIRE(server.start());

    ControlClient client;
    std::string error;
    nlohmann::json result;
    REQUIRE(client.connect("127.0.0.1", server.port(), 3000));

    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(client.request("echo", {{"text", std::to_string(i)}}, result, error));
        CHECK(result.at("echo").get<std::string>() == std::to_string(i));
    }
    CHECK(handler.methods().size() == 10);
}

TEST_CASE("ControlServer: serves multiple clients concurrently")
{
    TestHandler handler;
    ControlServer server("127.0.0.1", 0);
    server.setHandler([&handler](const protocol::Command& cmd) { return handler.handle(cmd); });
    REQUIRE(server.start());

    constexpr int kClients = 4;
    constexpr int kRequestsPerClient = 10;

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c)
    {
        threads.emplace_back([&, c]() {
            ControlClient client;
            std::string error;
            nlohmann::json result;
            if (!client.connect("127.0.0.1", server.port(), 5000))
            {
                ++failures;
                return;
            }
            for (int i = 0; i < kRequestsPerClient; ++i)
            {
                if (!client.request("echo", {{"text", std::to_string(c * 100 + i)}}, result, error) ||
                    result.at("echo").get<std::string>() != std::to_string(c * 100 + i))
                {
                    ++failures;
                }
            }
        });
    }
    for (auto& t : threads)
    {
        t.join();
    }

    CHECK(failures.load() == 0);
    CHECK(handler.methods().size() == kClients * kRequestsPerClient);
}

TEST_CASE("ControlServer: concurrent clients are serialized by the handler lock")
{
    // The "sleep" handler holds the shared handler mutex; with 3 clients
    // sending 4 requests each the handler runs sequentially (12 x 50 ms).
    TestHandler handler;
    ControlServer server("127.0.0.1", 0);
    server.setHandler([&handler](const protocol::Command& cmd) { return handler.handle(cmd); });
    REQUIRE(server.start());

    constexpr int kClients = 3;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c)
    {
        threads.emplace_back([&, c]() {
            ControlClient client;
            std::string error;
            nlohmann::json result;
            if (!client.connect("127.0.0.1", server.port(), 5000))
            {
                ++failures;
                return;
            }
            for (int i = 0; i < 4; ++i)
            {
                if (!client.request("sleep", {}, result, error))
                {
                    ++failures;
                }
            }
        });
    }
    for (auto& t : threads)
    {
        t.join();
    }
    CHECK(failures.load() == 0);
    CHECK(handler.methods().size() == 12);
}

TEST_CASE("ControlServer: malformed lines get an invalid-command response")
{
    ControlServer server("127.0.0.1", 0);
    server.setHandler([](const protocol::Command&) {
        return protocol::Response::success(0, nlohmann::json::object());
    });
    REQUIRE(server.start());

    net::TcpConnection raw;
    REQUIRE(raw.connect("127.0.0.1", server.port(), 3000));
    REQUIRE(raw.sendLine("this is not json"));

    std::string line;
    REQUIRE(raw.receiveLine(line, 3000));
    protocol::Response resp;
    REQUIRE(protocol::decodeResponse(line, resp));
    CHECK_FALSE(resp.ok);
    CHECK(resp.error == "invalid command");
}

TEST_CASE("ControlServer: stop disconnects active clients promptly")
{
    TestHandler handler;
    ControlServer server("127.0.0.1", 0);
    server.setHandler([&handler](const protocol::Command& cmd) { return handler.handle(cmd); });
    REQUIRE(server.start());

    net::TcpConnection raw;
    REQUIRE(raw.connect("127.0.0.1", server.port(), 3000));
    // A client is connected and idle; stop() must not wait for its receive
    // timeout, it closes the socket and returns.
    const auto start = std::chrono::steady_clock::now();
    server.stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed < 2000);
}

TEST_CASE("ControlServer: idle connections are kept open")
{
    // The GUI's command connection sits idle between interactions. The
    // server must not close it when the receive wait times out (the old
    // 5 s idle kill made the next send fail with "failed to send command");
    // the timeout only re-checks the running flag.
    TestHandler handler;
    ControlServer server("127.0.0.1", 0, /*idleCheckMs=*/100);
    server.setHandler([&handler](const protocol::Command& cmd) { return handler.handle(cmd); });
    REQUIRE(server.start());

    ControlClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 3000));
    std::string error;
    nlohmann::json result;
    REQUIRE(client.request("echo", {{"text", "one"}}, result, error));

    std::this_thread::sleep_for(600ms); // six idle re-checks go by
    REQUIRE(client.request("echo", {{"text", "two"}}, result, error));
    CHECK(result.at("echo").get<std::string>() == "two");

    server.stop();
}

TEST_CASE("ControlClient: a dropped connection is re-established transparently")
{
    // The engine went away (peer closed the connection) while the client
    // still believed it was connected. The next request(s) must re-connect
    // to the same endpoint without the caller noticing; depending on RST
    // timing the first request either retries its own send or fails with
    // EOF and the second one reconnects lazily - either way the second
    // request must complete over the fresh connection.
    net::TcpListener listener;
    REQUIRE(listener.bind("127.0.0.1", 0));
    const auto port = listener.boundPort();

    ControlClient client;
    REQUIRE(client.connect("127.0.0.1", port, 3000));

    // The peer closes the connection (engine shutdown/restart).
    net::TcpConnection deadPeer;
    REQUIRE(listener.accept(deadPeer, 3000));
    deadPeer.close();

    // The "new" engine instance: accepts the reconnection and answers up to
    // two command lines (both requests may travel over it, when the first
    // request retried its send).
    std::atomic<int> answered{0};
    std::thread responder([&]() {
        net::TcpConnection peer;
        if (!listener.accept(peer, 10000))
        {
            return;
        }
        for (int i = 0; i < 2; ++i)
        {
            std::string line;
            if (!peer.receiveLine(line, 1000))
            {
                break;
            }
            protocol::Command cmd;
            if (!protocol::decodeCommand(line, cmd))
            {
                break;
            }
            peer.sendLine(protocol::encodeResponse(protocol::Response::success(
                cmd.id, {{"echo", cmd.params.value("text", std::string())}})));
            ++answered;
        }
    });

    std::string error;
    nlohmann::json result;
    client.request("echo", {{"text", "one"}}, result, error);
    const bool secondOk = client.request("echo", {{"text", "two"}}, result, error);

    responder.join();
    CHECK(answered.load() >= 1);
    REQUIRE(secondOk);
    CHECK(result.at("echo").get<std::string>() == "two");
}

} // TEST_SUITE("Net")
