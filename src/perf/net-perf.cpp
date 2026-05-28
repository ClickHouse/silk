#include "common.h"

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/init.h>
#include <silk/util/list.h>
#include <silk/util/logger.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <cxxopts.hpp>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

// Use silk::FiberScheduler::read/write (io_uring) instead of recv/send + poll.
#define USE_IO_URING_RW

/**
 * TcpConnection - fiber-aware socket backed by silk::FiberScheduler::poll.
 */
class TcpConnection
{
public:
    TcpConnection() noexcept = default;
    explicit TcpConnection(int connFd) noexcept;
    ~TcpConnection() noexcept;

    TcpConnection(TcpConnection && other) noexcept;
    TcpConnection & operator=(TcpConnection && other) noexcept;
    TcpConnection(const TcpConnection &) = delete;
    TcpConnection & operator=(const TcpConnection &) = delete;

    static int listen(const char * host, uint16_t port, int backlog, TcpConnection * out) noexcept;
    static int connect(const char * host, uint16_t port, TcpConnection * out) noexcept;

    void close() noexcept;
    int accept(TcpConnection * out) noexcept;
    int write(const void * buf, uint64_t len, uint64_t * bytesWritten = nullptr) noexcept;
    int writeAll(const void * buf, uint64_t len) noexcept;
    int read(void * buf, uint64_t maxLen, uint64_t * bytesRead) noexcept;
    int readAll(void * buf, uint64_t len) noexcept;

    int getFd() const noexcept { return connFd; }

private:
    int connFd = -1;
};

TcpConnection::TcpConnection(int connFd_) noexcept
    : connFd(connFd_)
{
}

TcpConnection::~TcpConnection() noexcept
{
    if (connFd >= 0)
    {
        ::close(connFd);
    }
}

TcpConnection::TcpConnection(TcpConnection && other) noexcept
    : connFd(other.connFd)
{
    other.connFd = -1;
}

TcpConnection & TcpConnection::operator=(TcpConnection && other) noexcept
{
    if (this != &other)
    {
        if (connFd >= 0)
        {
            ::close(connFd);
        }
        connFd = other.connFd;
        other.connFd = -1;
    }
    return *this;
}

void TcpConnection::close() noexcept
{
    if (connFd >= 0)
    {
        ::shutdown(connFd, SHUT_RDWR);
    }
}

int TcpConnection::connect(const char * host, uint16_t port, TcpConnection * out) noexcept
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);

    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        SILK_ERROR("inet_pton failed: invalid address %s", host);
        return EINVAL;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        int r = errno;
        SILK_ERROR("socket failed: %s", std::strerror(r));
        return r;
    }

    int value = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)))
    {
        int r = errno;
        SILK_ERROR("setsockopt TCP_NODELAY failed: %s", std::strerror(r));
        ::close(fd);
        return r;
    }

    int r = silk::FiberScheduler::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    if (r)
    {
        SILK_ERROR("connect failed: %s", std::strerror(r));
        ::close(fd);
        return r;
    }

    *out = TcpConnection(fd);
    return 0;
}

int TcpConnection::listen(const char * host, uint16_t port, int backlog, TcpConnection * out) noexcept
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);

    if (host == nullptr || host[0] == '\0')
    {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    else if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        SILK_ERROR("inet_pton failed: invalid address %s", host);
        return EINVAL;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        int r = errno;
        SILK_ERROR("socket failed: %s", std::strerror(r));
        return r;
    }

    int value = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)))
    {
        int r = errno;
        SILK_ERROR("setsockopt TCP_NODELAY failed: %s", std::strerror(r));
        ::close(fd);
        return r;
    }

    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)))
    {
        int r = errno;
        SILK_ERROR("setsockopt SO_REUSEADDR failed: %s", std::strerror(r));
        ::close(fd);
        return r;
    }

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
    {
        int r = errno;
        SILK_ERROR("bind failed: %s", std::strerror(r));
        ::close(fd);
        return r;
    }

    if (::listen(fd, backlog))
    {
        int r = errno;
        SILK_ERROR("listen failed: %s", std::strerror(r));
        ::close(fd);
        return r;
    }

    *out = TcpConnection(fd);
    return 0;
}

int TcpConnection::accept(TcpConnection * out) noexcept
{
    uint64_t acceptedFd = 0;
    int r = silk::FiberScheduler::accept(connFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC, &acceptedFd);
    if (r)
    {
        return r;
    }
    int fd = static_cast<int>(acceptedFd);

    int value = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)))
    {
        int r = errno;
        SILK_ERROR("setsockopt TCP_NODELAY failed: %s", std::strerror(r));
        ::close(fd);
        return r;
    }

    *out = TcpConnection(fd);
    return 0;
}

int TcpConnection::write(const void * buf, uint64_t len, uint64_t * bytesWritten) noexcept
{
#if defined(USE_IO_URING_RW)
    return silk::FiberScheduler::write(connFd, buf, len, 0, bytesWritten);
#else
    for (;;)
    {
        ssize_t count = ::send(connFd, buf, len, MSG_NOSIGNAL);
        if (count >= 0)
        {
            if (bytesWritten)
            {
                *bytesWritten = static_cast<uint64_t>(count);
            }
            return 0;
        }

        int r = errno;
        if (r == EAGAIN)
        {
            r = silk::FiberScheduler::poll(connFd, POLLOUT);
            if (!r)
            {
                continue;
            }
        }
        return r;
    }
#endif
}

int TcpConnection::writeAll(const void * buf, uint64_t len) noexcept
{
    uint64_t total = 0;
    const char * ptr = static_cast<const char *>(buf);
    while (total < len)
    {
        uint64_t written = 0;
        int r = write(ptr, len - total, &written);
        if (r)
        {
            return r;
        }
        if (written == 0)
        {
            return ECONNRESET;
        }
        total += written;
        ptr += written;
    }
    return 0;
}

int TcpConnection::read(void * buf, uint64_t maxLen, uint64_t * bytesRead) noexcept
{
#if defined(USE_IO_URING_RW)
    return silk::FiberScheduler::read(connFd, buf, maxLen, 0, bytesRead);
#else
    for (;;)
    {
        ssize_t count = ::recv(connFd, buf, maxLen, 0);
        if (count >= 0)
        {
            if (bytesRead)
            {
                *bytesRead = static_cast<uint64_t>(count);
            }
            return 0;
        }

        int r = errno;
        if (r == EAGAIN)
        {
            r = silk::FiberScheduler::poll(connFd, POLLIN);
            if (!r)
            {
                continue;
            }
        }
        return r;
    }
#endif
}

int TcpConnection::readAll(void * buf, uint64_t len) noexcept
{
    uint64_t total = 0;
    char * ptr = static_cast<char *>(buf);
    while (total < len)
    {
        uint64_t n = 0;
        int r = read(ptr, len - total, &n);
        if (r)
        {
            return r;
        }
        if (n == 0)
        {
            return ECONNRESET;
        }
        total += n;
        ptr += n;
    }
    return 0;
}

//
// Benchmark
//

struct ServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 7777;
    uint32_t msgSize = 64;
    uint64_t delayNs = 0;
    bool printCounters = false;
};

class Server
{
public:
    explicit Server(const ServerConfig & cfg);
    ~Server();

    void start();
    void stop();

private:
    static constexpr int LISTEN_BACKLOG = 64;

    struct Connection
    {
        silk::ListEntry listEntry;
        TcpConnection conn;
        silk::FiberFuture future;
    };

    //
    // Fiber main functions.
    //

    struct AcceptFiberParams
    {
        Server * server;
    };
    static int acceptFiberMain(AcceptFiberParams * params) noexcept;

    struct ServerFiberParams
    {
        Server * server;
        Connection * connection;
    };
    static int serverFiberMain(ServerFiberParams * params) noexcept;

    //
    // State.
    //

    ServerConfig cfg;
    TcpConnection listener;
    bool acceptStarted = false;
    silk::FiberFuture acceptFuture;
    silk::List<Connection, &Connection::listEntry> connections;
};

Server::Server(const ServerConfig & cfg)
    : cfg(cfg)
{
    int r = TcpConnection::listen(cfg.host.c_str(), cfg.port, LISTEN_BACKLOG, &listener);
    SILK_ASSERT(!r, "listen failed: %s", std::strerror(r));
}

Server::~Server() = default;

void Server::start()
{
    int r = silk::FiberScheduler::run(acceptFiberMain, {this}, &acceptFuture);
    SILK_ASSERT(!r, "cannot start fiber: %s", std::strerror(r));

    acceptStarted = true;
}

void Server::stop()
{
    listener.close();

    if (acceptStarted)
    {
        int r = acceptFuture.wait();
        SILK_ASSERT(!r);
    }

    while (Connection * connection = connections.pop_front())
    {
        connection->conn.close();

        int r = connection->future.wait();
        SILK_ASSERT(!r);

        delete connection;
    }
}

int Server::acceptFiberMain(AcceptFiberParams * params) noexcept
{
    Server * server = params->server;

    for (;;)
    {
        Connection * connection = new Connection();
        int r = server->listener.accept(&connection->conn);
        if (r)
        {
            delete connection;
            if (!isExpectedShutdown(r))
            {
                SILK_ERROR("accept failed: %s", strerror(r));
            }
            break;
        }

        server->connections.push_back(connection);

        r = silk::FiberScheduler::run(serverFiberMain, {server, connection}, &connection->future);
        if (r)
        {
            SILK_ERROR("cannot start fiber: %s", std::strerror(r));
            break;
        }
    }

    return 0;
}

int Server::serverFiberMain(ServerFiberParams * params) noexcept
{
    Server * server = params->server;
    TcpConnection & conn = params->connection->conn;

    SILK_ASSERT(server->cfg.msgSize >= sizeof(uint32_t));
    auto buf = std::make_unique<char[]>(server->cfg.msgSize);

    for (;;)
    {
        int r = conn.readAll(buf.get(), server->cfg.msgSize);
        if (r)
        {
            if (!isExpectedShutdown(r))
            {
                SILK_ERROR("read failed: %s", strerror(r));
            }
            break;
        }

        // The first 4 bytes carry a per-message stall budget in nanoseconds.
        // Busy-loop for that long to simulate compute-bound work; the fiber
        // holds its CPU during the stall so other ready fibers on this CPU
        // are visible to the work-stealing path on idle CPUs.
        busyLoopForStall(buf.get());

        if (server->cfg.delayNs)
        {
            silk::FiberScheduler::sleep(server->cfg.delayNs);
        }

        r = conn.writeAll(buf.get(), server->cfg.msgSize);
        if (r)
        {
            if (!isExpectedShutdown(r))
            {
                SILK_ERROR("write failed: %s", strerror(r));
            }
            break;
        }
    }

    return 0;
}

struct ClientConfig
{
    std::string host = "127.0.0.1";
    uint16_t port = 7777;
    uint32_t numConnections = 16;
    uint32_t msgSize = 64;
    uint64_t durationNs = 10'000'000'000ULL;
    uint64_t warmupNs = 2'000'000'000ULL;
    // Per-connection Poisson rate of stall messages (Hz). 0 disables stalls.
    // The first 4 bytes of each message are the stall duration in nanoseconds;
    // server busy-loops for that long before echoing.
    double stallRateHz = 0.0;
    uint64_t stallNs = 0;
    bool printCounters = false;
};

class Client
{
public:
    explicit Client(const ClientConfig & cfg);

    void start();
    void stop();

    std::vector<uint64_t> collectLatencies();

private:
    struct Connection
    {
        TcpConnection conn;
        silk::FiberFuture future;
        std::vector<uint64_t> latencies;
    };

    //
    // Fiber main functions.
    //

    struct ClientFiberParams
    {
        Client * client;
        Connection * connection;
    };
    static int clientFiberMain(ClientFiberParams * params) noexcept;

    //
    // State.
    //

    ClientConfig cfg;
    std::atomic<uint64_t> warmupEndCycles;
    std::vector<Connection> connections;
};

Client::Client(const ClientConfig & cfg)
    : cfg(cfg)
    , warmupEndCycles(UINT64_MAX)
    , connections(cfg.numConnections)
{
}

void Client::start()
{
    for (Connection & connection : connections)
    {
        int r = TcpConnection::connect(cfg.host.c_str(), cfg.port, &connection.conn);
        SILK_ASSERT(!r, "connect failed: %s", std::strerror(r));
    }

    for (Connection & connection : connections)
    {
        int r = silk::FiberScheduler::run(clientFiberMain, {this, &connection}, &connection.future);
        SILK_ASSERT(!r, "cannot start fiber: %s", std::strerror(r));
    }

    warmupEndCycles.store(silk::Tsc::getCycles() + silk::Tsc::nanosecondsToCycles(cfg.warmupNs), std::memory_order_relaxed);
}

void Client::stop()
{
    for (Connection & connection : connections)
    {
        connection.conn.close();
    }

    for (Connection & connection : connections)
    {
        int r = connection.future.wait();
        SILK_ASSERT(!r);
    }
}

std::vector<uint64_t> Client::collectLatencies()
{
    std::vector<uint64_t> all;
    for (Connection & connection : connections)
    {
        all.insert(all.end(), connection.latencies.begin(), connection.latencies.end());
    }
    return all;
}

int Client::clientFiberMain(ClientFiberParams * params) noexcept
{
    Client * client = params->client;
    Connection * connection = params->connection;

    SILK_ASSERT(client->cfg.msgSize >= sizeof(uint32_t));
    auto buf = std::make_unique<char[]>(client->cfg.msgSize);
    std::memset(buf.get(), 0xAB, client->cfg.msgSize);

    StallScheduler stalls(client->cfg.stallRateHz, client->cfg.stallNs, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(connection)));

    for (;;)
    {
        uint32_t stallNs = stalls.next();
        std::memcpy(buf.get(), &stallNs, sizeof(stallNs));

        uint64_t start = silk::Tsc::getCycles();

        int r = connection->conn.writeAll(buf.get(), client->cfg.msgSize);
        if (r)
        {
            if (!isExpectedShutdown(r))
            {
                SILK_ERROR("write failed: %s", strerror(r));
            }
            break;
        }

        r = connection->conn.readAll(buf.get(), client->cfg.msgSize);
        if (r)
        {
            if (!isExpectedShutdown(r))
            {
                SILK_ERROR("read failed: %s", strerror(r));
            }
            break;
        }

        if (start >= client->warmupEndCycles.load(std::memory_order_relaxed))
        {
            uint64_t end = silk::Tsc::getCycles();
            connection->latencies.push_back(silk::Tsc::cyclesToNanoseconds(end - start));
        }
    }

    return 0;
}

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
        printSchedulerLatency();
        printf(",");
        printCounters();
    }
    printf("}\n");
}

/**
 * Server entry point.
 */
static void runServer(int argc, char ** argv)
{
    ServerConfig cfg;
    bool verbose = false;

    cxxopts::Options cli("net-perf server", "net-perf server options");

    std::string delayStr = "0";

    // clang-format off
    cli.add_options()
        ("h,help",         "show this help")
        ("host",           "listen host",                                                                   cxxopts::value<std::string>(cfg.host))
        ("port",           "listen port",                                                                   cxxopts::value<uint16_t>(cfg.port))
        ("msg-size",       "echo message size in bytes",                                                    cxxopts::value<uint32_t>(cfg.msgSize))
        ("delay",          "server-side delay per message (e.g. 1ms, 100us)",                               cxxopts::value<std::string>(delayStr))
        ("print-counters", "enable per-CPU profiler and include counters in the JSON report",               cxxopts::value<bool>(cfg.printCounters))
        ("v,verbose",      "enable debug logging",                                                          cxxopts::value<bool>(verbose))
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

    silk::initialize();
    silk::FiberScheduler::Options options{.enableProfiler = cfg.printCounters};
    silk::FiberScheduler::initialize(&options);

    SILK_INFO("starting server on %s:%u", cfg.host.c_str(), cfg.port);

    Server server(cfg);
    server.start();

    int sig = 0;
    sigwait(&mask, &sig);
    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    SILK_INFO("stopping server");
    server.stop();

    if (cfg.printCounters)
    {
        printf("{\n");
        printSchedulerLatency();
        printf(",");
        printCounters();
        printf("}\n");
    }

    silk::FiberScheduler::destroy();
    silk::destroy();
}

/**
 * Client entry point.
 */
static void runClient(int argc, char ** argv)
{
    ClientConfig cfg;
    std::string durationStr = "10s";
    std::string warmupStr = "2s";
    std::string stallDurationStr = "0";
    bool verbose = false;

    cxxopts::Options cli("net-perf client", "net-perf client options");

    // clang-format off
    cli.add_options()
        ("h,help",         "show this help")
        ("host",           "server host",                                                                   cxxopts::value<std::string>(cfg.host))
        ("port",           "server port",                                                                   cxxopts::value<uint16_t>(cfg.port))
        ("connections",    "parallel connections",                                                          cxxopts::value<uint32_t>(cfg.numConnections))
        ("msg-size",       "message size in bytes",                                                         cxxopts::value<uint32_t>(cfg.msgSize))
        ("duration",       "measurement duration (e.g. 10s, 500ms)",                                        cxxopts::value<std::string>(durationStr))
        ("warmup",         "warmup duration (e.g. 2s, 500ms)",                                              cxxopts::value<std::string>(warmupStr))
        ("stall-rate",     "per-connection Poisson rate of stall messages (Hz, 0 disables)",                cxxopts::value<double>(cfg.stallRateHz))
        ("stall-duration", "stall duration per stall event (e.g. 100us, 1ms)",                              cxxopts::value<std::string>(stallDurationStr))
        ("print-counters", "enable per-CPU profiler and include counters in the JSON report",               cxxopts::value<bool>(cfg.printCounters))
        ("v,verbose",      "enable debug logging",                                                          cxxopts::value<bool>(verbose))
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

    silk::initialize();
    silk::FiberScheduler::Options options{.enableProfiler = cfg.printCounters};
    silk::FiberScheduler::initialize(&options);

    SILK_INFO("starting client on %s:%u", cfg.host.c_str(), cfg.port);

    Client client(cfg);
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

    std::vector<uint64_t> allLat = client.collectLatencies();
    printJson(allLat, cfg);

    silk::FiberScheduler::destroy();
    silk::destroy();
}

/**
 * Main entry point.
 */
int main(int argc, char ** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: net-perf <server|client> [options]\n"
                  << "       net-perf <server|client> --help\n";
        return 1;
    }

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
                  << "usage: net-perf <server|client> [options]\n";
        return 1;
    }
    return 0;
}
