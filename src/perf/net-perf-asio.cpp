#include "common.h"

#include <silk/util/assert.h>
#include <silk/util/init.h>
#include <silk/util/logger.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

#include <cxxopts.hpp>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

static bool isExpectedShutdown(const boost::system::error_code & ec)
{
    return ec == asio::error::eof || isExpectedShutdown(ec.value());
}

//
// Server
//

struct ServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 7777;
    uint32_t msgSize = 64;
    uint64_t delayNs = 0;
};

class Server
{
public:
    explicit Server(asio::io_context & ioc, const ServerConfig & cfg);

    void start();
    void stop();

private:
    static constexpr int LISTEN_BACKLOG = 64;

    struct Connection
    {
        std::shared_ptr<tcp::socket> socket;
    };

    asio::awaitable<void> acceptLoop();
    static asio::awaitable<void> handleConnection(Server * server, std::shared_ptr<tcp::socket> socket);

    asio::io_context & ioc;
    ServerConfig cfg;
    tcp::acceptor acceptor;
    std::mutex mutex;
    std::list<Connection> connections;
};

Server::Server(asio::io_context & ioc, const ServerConfig & cfg)
    : ioc(ioc)
    , cfg(cfg)
    , acceptor(ioc)
{
    acceptor.open(tcp::v4());
    acceptor.set_option(asio::socket_base::reuse_address(true));
    acceptor.bind(tcp::endpoint(asio::ip::make_address(cfg.host), cfg.port));
    acceptor.listen(LISTEN_BACKLOG);
}

void Server::start()
{
    asio::co_spawn(ioc, acceptLoop(), asio::detached);
}

void Server::stop()
{
    // Post to an io_context thread to safely close the acceptor and all
    // connection sockets. The caller then calls ioc.stop() + join.
    asio::post(
        ioc,
        [this]
        {
            boost::system::error_code ec;
            acceptor.close(ec);

            std::lock_guard lock(mutex);
            for (Connection & conn : connections)
            {
                conn.socket->close(ec);
            }
        });
}

asio::awaitable<void> Server::acceptLoop()
{
    for (;;)
    {
        boost::system::error_code ec;
        auto socket = std::make_shared<tcp::socket>(co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, ec)));
        if (ec)
        {
            if (!isExpectedShutdown(ec))
            {
                SILK_ERROR("accept failed: %s", ec.message().c_str());
            }
            break;
        }

        socket->set_option(tcp::no_delay(true));

        {
            std::lock_guard lock(mutex);
            connections.push_back({socket});
        }

        asio::co_spawn(ioc, handleConnection(this, socket), asio::detached);
    }
}

asio::awaitable<void> Server::handleConnection(Server * server, std::shared_ptr<tcp::socket> socket)
{
    SILK_ASSERT(server->cfg.msgSize >= sizeof(uint32_t));
    auto buf = std::make_unique<char[]>(server->cfg.msgSize);
    boost::system::error_code ec;
    for (;;)
    {
        std::size_t n = 0;
        while (n < server->cfg.msgSize)
        {
            std::size_t got = co_await socket->async_read_some(
                asio::buffer(buf.get() + n, server->cfg.msgSize - n), asio::redirect_error(asio::use_awaitable, ec));
            if (ec)
            {
                if (!isExpectedShutdown(ec))
                {
                    SILK_ERROR("read failed: %s", ec.message().c_str());
                }
                co_return;
            }
            n += got;
        }

        // First 4 bytes carry a per-message stall budget in nanoseconds.
        // Busy-loop for that long; this thread's executor cannot service
        // other connections during the stall, demonstrating HOL blocking.
        busyLoopForStall(buf.get());

        if (server->cfg.delayNs)
        {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::nanoseconds(server->cfg.delayNs));
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (ec)
            {
                if (!isExpectedShutdown(ec))
                {
                    SILK_ERROR("sleep failed: %s", ec.message().c_str());
                }
                co_return;
            }
        }

        n = 0;
        while (n < server->cfg.msgSize)
        {
            std::size_t sent = co_await socket->async_write_some(
                asio::buffer(buf.get() + n, server->cfg.msgSize - n), asio::redirect_error(asio::use_awaitable, ec));
            if (ec)
            {
                if (!isExpectedShutdown(ec))
                {
                    SILK_ERROR("write failed: %s", ec.message().c_str());
                }
                co_return;
            }
            n += sent;
        }
    }
}

//
// Client
//

struct ClientConfig
{
    std::string host = "127.0.0.1";
    uint16_t port = 7777;
    uint32_t numConnections = 16;
    uint32_t msgSize = 64;
    uint64_t durationNs = 10'000'000'000ULL;
    uint64_t warmupNs = 2'000'000'000ULL;
    double stallRateHz = 0.0;
    uint64_t stallNs = 0;
    bool printCounters = false;
};

class Client
{
public:
    explicit Client(asio::io_context & ioc, const ClientConfig & cfg);

    void start();
    void stop();

    std::vector<uint64_t> collectLatencies();

private:
    struct Connection
    {
        std::shared_ptr<tcp::socket> socket;
        std::vector<uint64_t> latencies;
    };

    static asio::awaitable<void> clientConnection(Client * client, Connection * connection);

    asio::io_context & ioc;
    ClientConfig cfg;
    std::vector<Connection> connections;
    std::atomic<uint64_t> warmupEndCycles{UINT64_MAX};
};

Client::Client(asio::io_context & ioc, const ClientConfig & cfg)
    : ioc(ioc)
    , cfg(cfg)
    , connections(cfg.numConnections)
{
}

void Client::start()
{
    for (Connection & conn : connections)
    {
        conn.socket = std::make_shared<tcp::socket>(ioc);
        tcp::resolver resolver(ioc);
        auto endpoints = resolver.resolve(cfg.host, std::to_string(cfg.port));
        asio::connect(*conn.socket, endpoints);
        conn.socket->set_option(tcp::no_delay(true));
        asio::co_spawn(ioc, clientConnection(this, &conn), asio::detached);
    }

    warmupEndCycles.store(silk::Tsc::getCycles() + silk::Tsc::nanosecondsToCycles(cfg.warmupNs), std::memory_order_relaxed);
}

void Client::stop()
{
    // Post to an io_context thread to safely close all sockets.
    // The caller then calls ioc.stop() + join.
    asio::post(
        ioc,
        [this]
        {
            boost::system::error_code ec;
            for (Connection & conn : connections)
            {
                conn.socket->close(ec);
            }
        });
}

asio::awaitable<void> Client::clientConnection(Client * client, Connection * connection)
{
    SILK_ASSERT(client->cfg.msgSize >= sizeof(uint32_t));
    auto buf = std::make_unique<char[]>(client->cfg.msgSize);
    std::memset(buf.get(), 0xAB, client->cfg.msgSize);
    boost::system::error_code ec;

    StallScheduler stalls(client->cfg.stallRateHz, client->cfg.stallNs, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(connection)));

    for (;;)
    {
        uint32_t stallNs = stalls.next();
        std::memcpy(buf.get(), &stallNs, sizeof(stallNs));

        uint64_t start = silk::Tsc::getCycles();

        std::size_t n = 0;
        while (n < client->cfg.msgSize)
        {
            std::size_t sent = co_await connection->socket->async_write_some(
                asio::buffer(buf.get() + n, client->cfg.msgSize - n), asio::redirect_error(asio::use_awaitable, ec));
            if (ec)
            {
                if (!isExpectedShutdown(ec))
                {
                    SILK_ERROR("write failed: %s", ec.message().c_str());
                }
                co_return;
            }
            n += sent;
        }

        n = 0;
        while (n < client->cfg.msgSize)
        {
            std::size_t got = co_await connection->socket->async_read_some(
                asio::buffer(buf.get() + n, client->cfg.msgSize - n), asio::redirect_error(asio::use_awaitable, ec));
            if (ec)
            {
                if (!isExpectedShutdown(ec))
                {
                    SILK_ERROR("read failed: %s", ec.message().c_str());
                }
                co_return;
            }
            n += got;
        }

        if (start >= client->warmupEndCycles.load(std::memory_order_relaxed))
        {
            connection->latencies.push_back(silk::Tsc::cyclesToNanoseconds(silk::Tsc::getCycles() - start));
        }
    }
}

std::vector<uint64_t> Client::collectLatencies()
{
    std::vector<uint64_t> all;
    for (Connection & conn : connections)
    {
        all.insert(all.end(), conn.latencies.begin(), conn.latencies.end());
    }
    return all;
}

//
// Output
//

static void printJson(std::vector<uint64_t> & latNs, const ClientConfig & cfg)
{
    uint64_t total = latNs.size();
    double durationS = static_cast<double>(cfg.durationNs) / 1e9;
    double rps = static_cast<double>(total) / durationS;
    double bwBytesS = rps * cfg.msgSize;

    printf("{\n");
    printf("  \"connections\": %u,\n", cfg.numConnections);
    printf("  \"msg_size_bytes\": %u,\n", cfg.msgSize);
    printf("  \"host\": \"%s\",\n", cfg.host.c_str());
    printf("  \"port\": %u,\n", cfg.port);
    printf("  \"duration_s\": %.3f,\n", durationS);
    printf("  \"total\": %lu,\n", total);
    printf("  \"rps\": %.1f,\n", rps);
    printf("  \"bw_bytes\": %.0f,\n", bwBytesS);
    printLatencyUs(latNs);
    if (cfg.printCounters)
    {
        printf(",");
        printCounters();
    }
    printf("}\n");
}

//
// Entry points
//

static void runServer(int argc, char ** argv)
{
    ServerConfig cfg;
    std::string delayStr = "0";
    bool verbose = false;

    cxxopts::Options cli("net-perf-asio server", "net-perf-asio server options");

    // clang-format off
    cli.add_options()
        ("h,help",    "show this help")
        ("host",      "listen host",                                     cxxopts::value<std::string>(cfg.host))
        ("port",      "listen port",                                     cxxopts::value<uint16_t>(cfg.port))
        ("msg-size",  "echo message size in bytes",                      cxxopts::value<uint32_t>(cfg.msgSize))
        ("delay",     "server-side delay per message (e.g. 1ms, 100us)", cxxopts::value<std::string>(delayStr))
        ("v,verbose", "enable debug logging",                            cxxopts::value<bool>(verbose))
        ;
    // clang-format on

    try
    {
        auto result = cli.parse(argc, argv);
        if (result.count("help"))
        {
            std::cout << cli.help() << "\n";
            return;
        }
        cfg.delayNs = parseDuration(delayStr);
        if (verbose)
        {
            silk::Logger::setLevel(silk::LogLevel::DEBUG);
        }
    }
    catch (const cxxopts::exceptions::exception & ex)
    {
        std::cerr << "error: " << ex.what() << "\n" << cli.help() << "\n";
        exit(1);
    }

    sigset_t mask = blockSignals();

    asio::io_context ioc;
    auto work = asio::make_work_guard(ioc);

    uint32_t numThreads = silk::getAvailableProcessorCount();
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < numThreads; ++i)
    {
        threads.emplace_back([&] { ioc.run(); });
    }

    Server server(ioc, cfg);
    server.start();

    SILK_INFO("starting server on %s:%u", cfg.host.c_str(), cfg.port);

    int sig = 0;
    sigwait(&mask, &sig);
    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    SILK_INFO("stopping server");
    server.stop();

    ioc.stop();
    for (auto & thread : threads)
    {
        thread.join();
    }
}

static void runClient(int argc, char ** argv)
{
    ClientConfig cfg;
    bool verbose = false;

    cxxopts::Options cli("net-perf-asio client", "net-perf-asio client options");

    std::string durationStr = "10s";
    std::string warmupStr = "2s";
    std::string stallDurationStr = "0";

    // clang-format off
    cli.add_options()
        ("h,help",         "show this help")
        ("host",           "server host",                                                    cxxopts::value<std::string>(cfg.host))
        ("port",           "server port",                                                    cxxopts::value<uint16_t>(cfg.port))
        ("connections",    "parallel connections",                                           cxxopts::value<uint32_t>(cfg.numConnections))
        ("msg-size",       "message size in bytes",                                          cxxopts::value<uint32_t>(cfg.msgSize))
        ("duration",       "measurement duration (e.g. 10s, 500ms)",                         cxxopts::value<std::string>(durationStr))
        ("warmup",         "warmup duration (e.g. 2s, 500ms)",                               cxxopts::value<std::string>(warmupStr))
        ("stall-rate",     "per-connection Poisson rate of stall messages (Hz, 0 disables)", cxxopts::value<double>(cfg.stallRateHz))
        ("stall-duration", "stall duration per stall event (e.g. 100us, 1ms)",               cxxopts::value<std::string>(stallDurationStr))
        ("print-counters", "include counters in the JSON report",                            cxxopts::value<bool>(cfg.printCounters))
        ("v,verbose",      "enable debug logging",                                           cxxopts::value<bool>(verbose))
        ;
    // clang-format on

    try
    {
        auto result = cli.parse(argc, argv);
        if (result.count("help"))
        {
            std::cout << cli.help() << "\n";
            return;
        }
        cfg.durationNs = parseDuration(durationStr);
        cfg.warmupNs = parseDuration(warmupStr);
        cfg.stallNs = parseDuration(stallDurationStr);
        if (verbose)
        {
            silk::Logger::setLevel(silk::LogLevel::DEBUG);
        }
    }
    catch (const cxxopts::exceptions::exception & ex)
    {
        std::cerr << "error: " << ex.what() << "\n" << cli.help() << "\n";
        exit(1);
    }

    sigset_t mask = blockSignals();

    asio::io_context ioc;
    auto work = asio::make_work_guard(ioc);

    uint32_t numThreads = silk::getAvailableProcessorCount();
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < numThreads; ++i)
    {
        threads.emplace_back([&] { ioc.run(); });
    }

    Client client(ioc, cfg);
    client.start();

    bool signalled = false;

    if (cfg.warmupNs > 0)
    {
        SILK_INFO("warming up for %s...", formatDuration(cfg.warmupNs).c_str());
        signalled = sigwaitFor(mask, cfg.warmupNs);
    }

    if (!signalled)
    {
        SILK_INFO("measuring for %s...", formatDuration(cfg.durationNs).c_str());
        sigwaitFor(mask, cfg.durationNs);
    }

    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    SILK_INFO("stopping client");
    client.stop();

    ioc.stop();
    for (auto & thread : threads)
    {
        thread.join();
    }

    std::vector<uint64_t> allLat = client.collectLatencies();
    printJson(allLat, cfg);
}

int main(int argc, char ** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: net-perf-asio <server|client> [options]\n"
                  << "       net-perf-asio <server|client> --help\n";
        return 1;
    }

    silk::initialize();

    const char * subcmd = argv[1];
    if (strcmp(subcmd, "server") == 0)
    {
        runServer(argc - 1, argv + 1);
    }
    else if (strcmp(subcmd, "client") == 0)
    {
        runClient(argc - 1, argv + 1);
    }
    else
    {
        std::cerr << "unknown subcommand: " << subcmd << "\n"
                  << "usage: net-perf-asio <server|client> [options]\n";
        return 1;
    }

    silk::destroy();
    return 0;
}
