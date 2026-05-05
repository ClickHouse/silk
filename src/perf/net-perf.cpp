#include "common.h"

#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/list.h>
#include <silk/util/logger.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/queue.h>
#include <silk/util/tsc.h>

#include <boost/program_options.hpp>

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <arpa/inet.h>
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
    explicit TcpConnection(int connFd) noexcept;
    ~TcpConnection() noexcept;

    static int listen(const char * host, uint16_t port, int backlog, TcpConnection ** listener) noexcept;
    static int connect(const char * host, uint16_t port, TcpConnection ** conn) noexcept;

    void close() noexcept;
    int accept(TcpConnection ** conn) noexcept;
    int write(const void * buf, uint64_t len, uint64_t * bytesWritten = nullptr) noexcept;
    int writeAll(const void * buf, uint64_t len) noexcept;
    int read(void * buf, uint64_t maxLen, uint64_t * bytesRead) noexcept;
    int readAll(void * buf, uint64_t len) noexcept;

private:
    int connFd;
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
        connFd = -1;
    }
}

void TcpConnection::close() noexcept
{
    if (connFd >= 0)
    {
        ::shutdown(connFd, SHUT_RDWR);
    }
}

int TcpConnection::connect(const char * host, uint16_t port, TcpConnection ** conn) noexcept
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);

    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        LOG_ERROR("inet_pton failed: invalid address {}", host);
        return EINVAL;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        int r = errno;
        LOG_ERROR("socket failed: {}", std::strerror(r));
        return r;
    }

    int value = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)))
    {
        int r = errno;
        LOG_ERROR("setsockopt TCP_NODELAY failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    int r = ::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    if (r < 0)
    {
        r = errno;
        if (r != EINPROGRESS)
        {
            LOG_ERROR("connect failed: {}", std::strerror(r));
            ::close(fd);
            return r;
        }

        r = silk::FiberScheduler::poll(fd, POLLOUT);
        if (r)
        {
            LOG_ERROR("poll failed: {}", std::strerror(r));
            ::close(fd);
            return r;
        }
    }

    r = 0;
    socklen_t len = sizeof(r);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &r, &len))
    {
        r = errno;
        LOG_ERROR("getsockopt SO_ERROR failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }
    if (r)
    {
        LOG_ERROR("connect error: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    *conn = new TcpConnection(fd);
    return 0;
}

int TcpConnection::listen(const char * host, uint16_t port, int backlog, TcpConnection ** listener) noexcept
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
        LOG_ERROR("inet_pton failed: invalid address {}", host);
        return EINVAL;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        int r = errno;
        LOG_ERROR("socket failed: {}", std::strerror(r));
        return r;
    }

    int value = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)))
    {
        int r = errno;
        LOG_ERROR("setsockopt TCP_NODELAY failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)))
    {
        int r = errno;
        LOG_ERROR("setsockopt SO_REUSEADDR failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
    {
        int r = errno;
        LOG_ERROR("bind failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    if (::listen(fd, backlog))
    {
        int r = errno;
        LOG_ERROR("listen failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    *listener = new TcpConnection(fd);
    return 0;
}

int TcpConnection::accept(TcpConnection ** conn) noexcept
{
    int fd;
    for (;;)
    {
        fd = ::accept4(connFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0)
        {
            break;
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

    int value = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)))
    {
        int r = errno;
        LOG_ERROR("setsockopt TCP_NODELAY failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    *conn = new TcpConnection(fd);
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
        TcpConnection * conn;
        silk::FiberFuture future;
    };

    //
    // silk::Fiber main functions.
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
    TcpConnection * listener;
    bool acceptStarted = false;
    silk::FiberFuture acceptFuture;
    silk::List<Connection, &Connection::listEntry> connections;
};

Server::Server(const ServerConfig & cfg)
    : cfg(cfg)
{
    int r = TcpConnection::listen(cfg.host.c_str(), cfg.port, LISTEN_BACKLOG, &listener);
    ASSERT(!r, "listen failed: {}", std::strerror(r));
}

Server::~Server()
{
    delete listener;
}

void Server::start()
{
    int r = silk::FiberScheduler::run(acceptFiberMain, {this}, &acceptFuture);
    ASSERT(!r, "cannot start fiber: {}", std::strerror(r));

    acceptStarted = true;
}

void Server::stop()
{
    listener->close();

    if (acceptStarted)
    {
        int r = acceptFuture.wait();
        ASSERT(!r);
    }

    while (Connection * connection = connections.pop_front())
    {
        if (connection->conn)
        {
            connection->conn->close();
        }

        int r = connection->future.wait();
        ASSERT(!r);

        delete connection->conn;
        delete connection;
    }
}

int Server::acceptFiberMain(AcceptFiberParams * params) noexcept
{
    Server * server = params->server;

    for (;;)
    {
        TcpConnection * conn = nullptr;
        int r = server->listener->accept(&conn);
        if (r)
        {
            if (!isExpectedShutdown(r))
            {
                LOG_ERROR("accept failed: {}", strerror(r));
            }
            break;
        }

        Connection * connection = new Connection();
        connection->conn = conn;
        server->connections.push_back(connection);

        r = silk::FiberScheduler::run(serverFiberMain, {server, connection}, &connection->future);
        if (r)
        {
            LOG_ERROR("cannot start fiber: {}", std::strerror(r));
            break;
        }
    }

    return 0;
}

int Server::serverFiberMain(ServerFiberParams * params) noexcept
{
    Server * server = params->server;
    TcpConnection * conn = params->connection->conn;

    auto buf = std::make_unique<char[]>(server->cfg.msgSize);

    for (;;)
    {
        int r = conn->readAll(buf.get(), server->cfg.msgSize);
        if (r)
        {
            if (!isExpectedShutdown(r))
            {
                LOG_ERROR("read failed: {}", strerror(r));
            }
            break;
        }

        if (server->cfg.delayNs)
        {
            silk::FiberScheduler::sleep(server->cfg.delayNs);
        }

        r = conn->writeAll(buf.get(), server->cfg.msgSize);
        if (r)
        {
            if (!isExpectedShutdown(r))
            {
                LOG_ERROR("write failed: {}", strerror(r));
            }
            break;
        }
    }

    delete conn;
    params->connection->conn = nullptr;

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
        TcpConnection * conn = nullptr;
        silk::FiberFuture future;
        std::vector<uint64_t> latencies;
    };

    //
    // silk::Fiber main functions.
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
        ASSERT(!r, "connect failed: {}", std::strerror(r));
    }

    for (Connection & connection : connections)
    {
        int r = silk::FiberScheduler::run(clientFiberMain, {this, &connection}, &connection.future);
        ASSERT(!r, "cannot start fiber: {}", std::strerror(r));
    }

    warmupEndCycles.store(silk::Tsc::getCycles() + silk::Tsc::nanosecondsToCycles(cfg.warmupNs), std::memory_order_relaxed);
}

void Client::stop()
{
    for (Connection & connection : connections)
    {
        connection.conn->close();
    }

    for (Connection & connection : connections)
    {
        int r = connection.future.wait();
        ASSERT(!r);

        delete connection.conn;
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

    auto buf = std::make_unique<char[]>(client->cfg.msgSize);
    std::memset(buf.get(), 0xAB, client->cfg.msgSize);

    for (;;)
    {
        uint64_t start = silk::Tsc::getCycles();

        int r = connection->conn->writeAll(buf.get(), client->cfg.msgSize);
        if (r)
        {
            if (!isExpectedShutdown(r))
            {
                LOG_ERROR("write failed: {}", strerror(r));
            }
            break;
        }

        r = connection->conn->readAll(buf.get(), client->cfg.msgSize);
        if (r)
        {
            if (!isExpectedShutdown(r))
            {
                LOG_ERROR("read failed: {}", strerror(r));
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
    printCounters();
    printf("}\n");
}

/**
 * Server entry point.
 */
static void runServer(int argc, char ** argv)
{
    ServerConfig cfg;
    bool verbose = false;

    namespace po = boost::program_options;
    po::options_description desc("net-perf server options");

    std::string delayStr = "0";

    // clang-format off
    desc.add_options()
        ("help,h", "show this help")
        ("host",     po::value(&cfg.host),    "listen host")
        ("port",     po::value(&cfg.port),    "listen port")
        ("msg-size", po::value(&cfg.msgSize), "echo message size in bytes")
        ("delay",    po::value(&delayStr),    "server-side delay per message (e.g. 1ms, 100us)")
        ("verbose,v", po::bool_switch(&verbose), "enable debug logging")
        ;
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << "usage: net-perf server [options]\n" << desc << "\n";
            return;
        }
        po::notify(vm);
        cfg.delayNs = parseDuration(delayStr);
        if (verbose)
        {
            silk::Logger::setLevel(silk::LogLevel::DEBUG);
        }
    }
    catch (const po::error & ex)
    {
        std::cerr << "error: " << ex.what() << "\n" << desc << "\n";
        exit(1);
    }

    sigset_t mask = blockSignals();

    silk::initRseq();
    silk::Perf::initialize();
    silk::QueueBase::initialize();
    silk::FiberScheduler::initialize();

    LOG_INFO("starting server on {}:{}", cfg.host, cfg.port);

    Server server(cfg);
    server.start();

    int sig = 0;
    sigwait(&mask, &sig);
    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    LOG_INFO("stopping server");
    server.stop();

    silk::FiberScheduler::destroy();
    silk::QueueBase::destroy();
    silk::Perf::destroy();
}

/**
 * Client entry point.
 */
static void runClient(int argc, char ** argv)
{
    ClientConfig cfg;
    bool verbose = false;

    namespace po = boost::program_options;
    po::options_description desc("net-perf client options");

    std::string durationStr = "10s";
    std::string warmupStr = "2s";

    // clang-format off
    desc.add_options()
        ("help,h", "show this help")
        ("host",        po::value(&cfg.host),           "server host")
        ("port",        po::value(&cfg.port),           "server port")
        ("connections", po::value(&cfg.numConnections), "parallel connections")
        ("msg-size",    po::value(&cfg.msgSize),        "message size in bytes")
        ("duration",    po::value(&durationStr),        "measurement duration (e.g. 10s, 500ms)")
        ("warmup",      po::value(&warmupStr),          "warmup duration (e.g. 2s, 500ms)")
        ("verbose,v",   po::bool_switch(&verbose),      "enable debug logging")
        ;
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << "usage: net-perf client [options]\n" << desc << "\n";
            return;
        }
        po::notify(vm);
        cfg.durationNs = parseDuration(durationStr);
        cfg.warmupNs = parseDuration(warmupStr);
        if (verbose)
        {
            silk::Logger::setLevel(silk::LogLevel::DEBUG);
        }
    }
    catch (const po::error & ex)
    {
        std::cerr << "error: " << ex.what() << "\n" << desc << "\n";
        exit(1);
    }

    sigset_t mask = blockSignals();

    silk::initRseq();
    silk::Perf::initialize();
    silk::FiberScheduler::initialize();

    LOG_INFO("starting client on {}:{}", cfg.host, cfg.port);

    Client client(cfg);
    client.start();

    bool signalled = false;

    if (cfg.warmupNs > 0)
    {
        LOG_INFO("warming up for {}...", formatDuration(cfg.warmupNs));
        signalled = sigwaitFor(mask, cfg.warmupNs);
    }

    if (!signalled)
    {
        LOG_INFO("measuring for {}...", formatDuration(cfg.durationNs));
        sigwaitFor(mask, cfg.durationNs);
    }

    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    LOG_INFO("stopping client");
    client.stop();

    std::vector<uint64_t> allLat = client.collectLatencies();
    printJson(allLat, cfg);

    silk::FiberScheduler::destroy();
    silk::Perf::destroy();
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
