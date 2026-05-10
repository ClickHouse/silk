#include "common.h"

#include <silk/util/assert.h>
#include <silk/util/init.h>
#include <silk/util/logger.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <boost/program_options.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

/** Tag stored as the first field of every epoll_event::data::ptr target. */
enum class EventTag : uint32_t
{
    Listener,
    Connection,
    Stop,
};

struct ListenerTag
{
    EventTag tag = EventTag::Listener;
};

struct StopTag
{
    EventTag tag = EventTag::Stop;
};

static bool eventClosesConnection(uint32_t mask) noexcept
{
    return (mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
}

/**
 * Event - owning wrapper around an eventfd.
 */
class Event
{
public:
    Event() noexcept = default;
    explicit Event(int eventFd) noexcept;
    ~Event() noexcept;

    Event(Event && other) noexcept;
    Event & operator=(Event && other) noexcept;
    Event(const Event &) = delete;
    Event & operator=(const Event &) = delete;

    static int create(Event * out) noexcept;

    int trigger() noexcept;
    int getFd() const noexcept { return eventFd; }

private:
    int eventFd = -1;
};

Event::Event(int eventFd_) noexcept
    : eventFd(eventFd_)
{
}

Event::~Event() noexcept
{
    if (eventFd >= 0)
    {
        ::close(eventFd);
    }
}

Event::Event(Event && other) noexcept
    : eventFd(other.eventFd)
{
    other.eventFd = -1;
}

Event & Event::operator=(Event && other) noexcept
{
    if (this != &other)
    {
        if (eventFd >= 0)
        {
            ::close(eventFd);
        }
        eventFd = other.eventFd;
        other.eventFd = -1;
    }
    return *this;
}

int Event::create(Event * out) noexcept
{
    int fd = ::eventfd(0, EFD_CLOEXEC);
    if (fd < 0)
    {
        int r = errno;
        SILK_ERROR("eventfd failed: {}", std::strerror(r));
        return r;
    }
    *out = Event(fd);
    return 0;
}

int Event::trigger() noexcept
{
    uint64_t value = 1;
    ssize_t n = ::write(eventFd, &value, sizeof(value));
    if (n != sizeof(value))
    {
        int r = errno;
        SILK_ERROR("write eventfd failed: {}", std::strerror(r));
        return r;
    }
    return 0;
}

/**
 * Epoll - owning wrapper around an epoll instance.
 */
class Epoll
{
public:
    Epoll() noexcept = default;
    explicit Epoll(int epfd) noexcept;
    ~Epoll() noexcept;

    Epoll(Epoll && other) noexcept;
    Epoll & operator=(Epoll && other) noexcept;
    Epoll(const Epoll &) = delete;
    Epoll & operator=(const Epoll &) = delete;

    static int create(Epoll * out) noexcept;

    int add(int fd, uint32_t events, void * ptr) noexcept;
    int del(int fd) noexcept;
    int wait(epoll_event * events, int maxEvents, int timeoutMs, int * outCount) noexcept;

    int getFd() const noexcept { return epfd; }

private:
    int epfd = -1;
};

Epoll::Epoll(int epfd_) noexcept
    : epfd(epfd_)
{
}

Epoll::~Epoll() noexcept
{
    if (epfd >= 0)
    {
        ::close(epfd);
    }
}

Epoll::Epoll(Epoll && other) noexcept
    : epfd(other.epfd)
{
    other.epfd = -1;
}

Epoll & Epoll::operator=(Epoll && other) noexcept
{
    if (this != &other)
    {
        if (epfd >= 0)
        {
            ::close(epfd);
        }
        epfd = other.epfd;
        other.epfd = -1;
    }
    return *this;
}

int Epoll::create(Epoll * out) noexcept
{
    int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0)
    {
        int r = errno;
        SILK_ERROR("epoll_create1 failed: {}", std::strerror(r));
        return r;
    }
    *out = Epoll(fd);
    return 0;
}

int Epoll::add(int fd, uint32_t events, void * ptr) noexcept
{
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = ptr;
    int r = ::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    if (r)
    {
        r = errno;
        SILK_ERROR("epoll_ctl ADD failed: {}", std::strerror(r));
        return r;
    }
    return 0;
}

int Epoll::del(int fd) noexcept
{
    int r = ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    if (r)
    {
        r = errno;
        SILK_ERROR("epoll_ctl DEL failed: {}", std::strerror(r));
        return r;
    }
    return 0;
}

int Epoll::wait(epoll_event * events, int maxEvents, int timeoutMs, int * outCount) noexcept
{
    int n = ::epoll_wait(epfd, events, maxEvents, timeoutMs);
    if (n < 0)
    {
        int r = errno;
        if (r != EINTR)
        {
            SILK_ERROR("epoll_wait failed: {}", std::strerror(r));
        }
        return r;
    }
    *outCount = n;
    return 0;
}

/**
 * TcpConnection - owning wrapper around a TCP socket.
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
    int read(void * buf, uint64_t maxLen, uint64_t * bytesRead) noexcept;

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

    int r = ::inet_pton(AF_INET, host, &addr.sin_addr);
    if (r != 1)
    {
        SILK_ERROR("inet_pton failed: invalid address {}", host);
        return EINVAL;
    }

    // Connect synchronously (loopback connects are effectively instant; even
    // remote endpoints only block once during setup), then switch to
    // non-blocking for the hot path.
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        r = errno;
        SILK_ERROR("socket failed: {}", std::strerror(r));
        return r;
    }

    int value = 1;
    r = ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
    if (r)
    {
        r = errno;
        SILK_ERROR("setsockopt TCP_NODELAY failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    r = ::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    if (r)
    {
        r = errno;
        SILK_ERROR("connect failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        r = errno;
        SILK_ERROR("fcntl F_GETFL failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    r = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (r)
    {
        r = errno;
        SILK_ERROR("fcntl F_SETFL O_NONBLOCK failed: {}", std::strerror(r));
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
    else
    {
        int r = ::inet_pton(AF_INET, host, &addr.sin_addr);
        if (r != 1)
        {
            SILK_ERROR("inet_pton failed: invalid address {}", host);
            return EINVAL;
        }
    }

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        int r = errno;
        SILK_ERROR("socket failed: {}", std::strerror(r));
        return r;
    }

    int value = 1;
    int r = ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
    if (r)
    {
        r = errno;
        SILK_ERROR("setsockopt TCP_NODELAY failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    r = ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
    if (r)
    {
        r = errno;
        SILK_ERROR("setsockopt SO_REUSEADDR failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    // SO_REUSEPORT lets each worker bind its own listener on the same port;
    // the kernel hashes incoming SYNs across them. Harmless in single-thread
    // mode.
    r = ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value));
    if (r)
    {
        r = errno;
        SILK_ERROR("setsockopt SO_REUSEPORT failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    r = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (r)
    {
        r = errno;
        SILK_ERROR("bind failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    r = ::listen(fd, backlog);
    if (r)
    {
        r = errno;
        SILK_ERROR("listen failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    *out = TcpConnection(fd);
    return 0;
}

int TcpConnection::accept(TcpConnection * out) noexcept
{
    int fd = ::accept4(connFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
    {
        int r = errno;
        return r;
    }

    int value = 1;
    int r = ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
    if (r)
    {
        r = errno;
        SILK_ERROR("setsockopt TCP_NODELAY failed: {}", std::strerror(r));
        ::close(fd);
        return r;
    }

    *out = TcpConnection(fd);
    return 0;
}

int TcpConnection::write(const void * buf, uint64_t len, uint64_t * bytesWritten) noexcept
{
    ssize_t count = ::send(connFd, buf, len, MSG_NOSIGNAL);
    if (count < 0)
    {
        int r = errno;
        return r;
    }
    if (bytesWritten)
    {
        *bytesWritten = static_cast<uint64_t>(count);
    }
    return 0;
}

int TcpConnection::read(void * buf, uint64_t maxLen, uint64_t * bytesRead) noexcept
{
    ssize_t count = ::recv(connFd, buf, maxLen, 0);
    if (count < 0)
    {
        int r = errno;
        return r;
    }
    if (bytesRead)
    {
        *bytesRead = static_cast<uint64_t>(count);
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
    uint32_t threads = 0; // 0 = auto-detect via silk::getAvailableProcessorCount
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
    static constexpr int EPOLL_BATCH = 1024;

    enum class State : uint8_t
    {
        Reading,
        Writing,
    };

    struct Connection
    {
        EventTag tag = EventTag::Connection;
        TcpConnection conn;
        std::unique_ptr<char[]> buf;
        uint32_t offset = 0;
        State state = State::Reading;
        bool readable = true;
        bool writable = true;
    };

    struct Worker
    {
        std::thread thread;
        TcpConnection listener;
        Event stopEvent;
    };

    //
    // Worker entry point.
    //

    struct WorkerParams
    {
        Server * server;
        Worker * worker;
    };
    static void workerMain(WorkerParams params) noexcept;

    /** Drive read/write state machine. Returns false if connection should close. */
    static bool drive(Connection * conn, uint32_t msgSize) noexcept;

    //
    // State.
    //

    ServerConfig cfg;
    bool started = false;
    std::vector<Worker> workers;
};

Server::Server(const ServerConfig & cfg)
    : cfg(cfg)
    , workers(cfg.threads)
{
    for (Worker & worker : workers)
    {
        int r = TcpConnection::listen(cfg.host.c_str(), cfg.port, LISTEN_BACKLOG, &worker.listener);
        SILK_ASSERT(!r, "listen failed: {}", std::strerror(r));
    }
}

Server::~Server()
{
    if (started)
    {
        stop();
    }
}

void Server::start()
{
    SILK_ASSERT(!started);

    for (Worker & worker : workers)
    {
        int r = Event::create(&worker.stopEvent);
        SILK_ASSERT(!r, "Event::create failed: {}", std::strerror(r));
        worker.thread = std::thread(workerMain, WorkerParams{this, &worker});
    }

    started = true;
}

void Server::stop()
{
    if (!started)
    {
        return;
    }

    for (Worker & worker : workers)
    {
        worker.listener.close();
    }

    for (Worker & worker : workers)
    {
        int r = worker.stopEvent.trigger();
        SILK_UNUSED(r);
    }
    for (Worker & worker : workers)
    {
        worker.thread.join();
    }

    started = false;
}

bool Server::drive(Connection * conn, uint32_t msgSize) noexcept
{
    for (;;)
    {
        if (conn->state == State::Reading)
        {
            if (!conn->readable)
            {
                return true;
            }
            uint64_t bytesRead = 0;
            int r = conn->conn.read(conn->buf.get() + conn->offset, msgSize - conn->offset, &bytesRead);
            if (!r)
            {
                if (bytesRead == 0)
                {
                    return false;
                }
                conn->offset += static_cast<uint32_t>(bytesRead);
                if (conn->offset == msgSize)
                {
                    // First 4 bytes carry a stall budget in nanoseconds.
                    // Busy-loop here; this worker thread cannot service its
                    // other connections during the stall, demonstrating HOL
                    // blocking on epoll's per-thread reactor.
                    busyLoopForStall(conn->buf.get());

                    conn->state = State::Writing;
                    conn->offset = 0;
                }
                continue;
            }
            if (r == EAGAIN)
            {
                conn->readable = false;
                return true;
            }
            if (!isExpectedShutdown(r))
            {
                SILK_ERROR("read failed: {}", std::strerror(r));
            }
            return false;
        }

        if (!conn->writable)
        {
            return true;
        }
        uint64_t bytesWritten = 0;
        int r = conn->conn.write(conn->buf.get() + conn->offset, msgSize - conn->offset, &bytesWritten);
        if (!r)
        {
            conn->offset += static_cast<uint32_t>(bytesWritten);
            if (conn->offset == msgSize)
            {
                conn->state = State::Reading;
                conn->offset = 0;
            }
            continue;
        }
        if (r == EAGAIN)
        {
            conn->writable = false;
            return true;
        }
        if (!isExpectedShutdown(r))
        {
            SILK_ERROR("write failed: {}", std::strerror(r));
        }
        return false;
    }
}

void Server::workerMain(WorkerParams params) noexcept
{
    Server * server = params.server;
    Worker * worker = params.worker;

    Epoll epoll;
    int r = Epoll::create(&epoll);
    SILK_ASSERT(!r, "Epoll::create failed: {}", std::strerror(r));

    ListenerTag listenerTag;
    StopTag stopTag;
    r = epoll.add(worker->listener.getFd(), EPOLLIN | EPOLLET, &listenerTag);
    SILK_ASSERT(!r);
    r = epoll.add(worker->stopEvent.getFd(), EPOLLIN, &stopTag);
    SILK_ASSERT(!r);

    std::unordered_set<Connection *> liveConnections;
    epoll_event events[EPOLL_BATCH];

    bool stop = false;
    while (!stop)
    {
        int eventCount = 0;
        r = epoll.wait(events, EPOLL_BATCH, -1, &eventCount);
        if (r == EINTR)
        {
            continue;
        }
        SILK_ASSERT(!r, "epoll_wait failed: {}", std::strerror(r));

        for (int i = 0; i < eventCount; ++i)
        {
            EventTag tag = *static_cast<EventTag *>(events[i].data.ptr);

            if (tag == EventTag::Stop)
            {
                stop = true;
                continue;
            }

            if (tag == EventTag::Listener)
            {
                for (;;)
                {
                    Connection * conn = new Connection();
                    int ar = worker->listener.accept(&conn->conn);
                    if (ar)
                    {
                        delete conn;
                        if (ar == EAGAIN)
                        {
                            break;
                        }
                        if (!isExpectedShutdown(ar))
                        {
                            SILK_ERROR("accept failed: {}", std::strerror(ar));
                        }
                        break;
                    }

                    conn->buf = std::make_unique<char[]>(server->cfg.msgSize);
                    liveConnections.insert(conn);
                    ar = epoll.add(conn->conn.getFd(), EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP, conn);
                    SILK_ASSERT(!ar);
                }
                continue;
            }

            Connection * conn = static_cast<Connection *>(events[i].data.ptr);
            uint32_t mask = events[i].events;
            if (mask & EPOLLIN)
            {
                conn->readable = true;
            }
            if (mask & EPOLLOUT)
            {
                conn->writable = true;
            }

            bool keepAlive = drive(conn, server->cfg.msgSize);
            if (!keepAlive || eventClosesConnection(mask))
            {
                liveConnections.erase(conn);
                int dr = epoll.del(conn->conn.getFd());
                SILK_UNUSED(dr);
                delete conn;
            }
        }
    }

    for (Connection * conn : liveConnections)
    {
        int dr = epoll.del(conn->conn.getFd());
        SILK_UNUSED(dr);
        delete conn;
    }
}

struct ClientConfig
{
    std::string host = "127.0.0.1";
    uint16_t port = 7777;
    uint32_t numConnections = 16;
    uint32_t msgSize = 64;
    uint32_t threads = 0; // 0 = auto-detect via silk::getAvailableProcessorCount
    uint64_t durationNs = 10'000'000'000ULL;
    uint64_t warmupNs = 2'000'000'000ULL;
    double stallRateHz = 0.0;
    uint64_t stallNs = 0;
    bool printCounters = false;
};

class Client
{
public:
    explicit Client(const ClientConfig & cfg);
    ~Client();

    void start();
    void stop();

    std::vector<uint64_t> collectLatencies();

private:
    static constexpr int EPOLL_BATCH = 1024;

    enum class State : uint8_t
    {
        Reading,
        Writing,
    };

    struct Connection
    {
        EventTag tag = EventTag::Connection;
        TcpConnection conn;
        std::unique_ptr<char[]> buf;
        uint32_t offset = 0;
        State state = State::Writing;
        bool readable = true;
        bool writable = true;
        uint64_t startCycles = 0;
        StallScheduler stalls;
        std::vector<uint64_t> latencies;
    };

    struct Worker;

    //
    // Worker entry point.
    //

    struct WorkerParams
    {
        Client * client;
        Worker * worker;
    };
    static void workerMain(WorkerParams params) noexcept;

    /** Drive write/read state machine. Returns false if connection should close. */
    static bool drive(Connection * conn, const ClientConfig & cfg, uint64_t warmupEndCycles) noexcept;

    struct Worker
    {
        std::thread thread;
        Event stopEvent;
        Connection * connections = nullptr;
        uint32_t connCount = 0;
    };

    //
    // State.
    //

    ClientConfig cfg;
    bool started = false;
    std::atomic<uint64_t> warmupEndCycles;
    std::vector<Connection> connections;
    std::vector<Worker> workers;
};

Client::Client(const ClientConfig & cfg)
    : cfg(cfg)
    , warmupEndCycles(UINT64_MAX)
    , connections(cfg.numConnections)
    , workers(cfg.threads)
{
}

Client::~Client()
{
    if (started)
    {
        stop();
    }
}

void Client::start()
{
    SILK_ASSERT(!started);

    for (Connection & connection : connections)
    {
        int r = TcpConnection::connect(cfg.host.c_str(), cfg.port, &connection.conn);
        SILK_ASSERT(!r, "connect failed: {}", std::strerror(r));
        SILK_ASSERT(cfg.msgSize >= sizeof(uint32_t));
        connection.buf = std::make_unique<char[]>(cfg.msgSize);
        std::memset(connection.buf.get(), 0xAB, cfg.msgSize);
        connection.stalls = StallScheduler(cfg.stallRateHz, cfg.stallNs, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&connection)));
    }

    uint32_t base = cfg.numConnections / cfg.threads;
    uint32_t extra = cfg.numConnections % cfg.threads;
    uint32_t connIdx = 0;

    for (uint32_t i = 0; i < cfg.threads; ++i)
    {
        Worker & worker = workers[i];
        int r = Event::create(&worker.stopEvent);
        SILK_ASSERT(!r, "Event::create failed: {}", std::strerror(r));
        worker.connections = connections.data() + connIdx;
        worker.connCount = base + (i < extra ? 1 : 0);
        connIdx += worker.connCount;
        worker.thread = std::thread(workerMain, WorkerParams{this, &worker});
    }

    warmupEndCycles.store(silk::Tsc::getCycles() + silk::Tsc::nanosecondsToCycles(cfg.warmupNs), std::memory_order_relaxed);

    started = true;
}

void Client::stop()
{
    if (!started)
    {
        return;
    }

    for (Connection & connection : connections)
    {
        connection.conn.close();
    }

    for (Worker & worker : workers)
    {
        int r = worker.stopEvent.trigger();
        SILK_UNUSED(r);
    }
    for (Worker & worker : workers)
    {
        worker.thread.join();
    }

    started = false;
}

std::vector<uint64_t> Client::collectLatencies()
{
    std::vector<uint64_t> all;
    uint64_t total = 0;
    for (Connection & connection : connections)
    {
        total += connection.latencies.size();
    }
    all.reserve(total);
    for (Connection & connection : connections)
    {
        all.insert(all.end(), connection.latencies.begin(), connection.latencies.end());
    }
    return all;
}

bool Client::drive(Connection * conn, const ClientConfig & cfg, uint64_t warmupEndCycles) noexcept
{
    uint32_t msgSize = cfg.msgSize;
    for (;;)
    {
        if (conn->state == State::Writing)
        {
            // Composing a fresh message: write the per-message stall budget
            // into the first 4 bytes (Poisson process scheduled per connection).
            if (conn->offset == 0)
            {
                uint32_t stallNs = conn->stalls.next();
                std::memcpy(conn->buf.get(), &stallNs, sizeof(stallNs));
            }
            if (!conn->writable)
            {
                return true;
            }
            uint64_t bytesWritten = 0;
            int r = conn->conn.write(conn->buf.get() + conn->offset, msgSize - conn->offset, &bytesWritten);
            if (!r)
            {
                conn->offset += static_cast<uint32_t>(bytesWritten);
                if (conn->offset == msgSize)
                {
                    conn->state = State::Reading;
                    conn->offset = 0;
                }
                continue;
            }
            if (r == EAGAIN)
            {
                conn->writable = false;
                return true;
            }
            if (!isExpectedShutdown(r))
            {
                SILK_ERROR("write failed: {}", std::strerror(r));
            }
            return false;
        }

        if (!conn->readable)
        {
            return true;
        }
        uint64_t bytesRead = 0;
        int r = conn->conn.read(conn->buf.get() + conn->offset, msgSize - conn->offset, &bytesRead);
        if (!r)
        {
            if (bytesRead == 0)
            {
                return false;
            }
            conn->offset += static_cast<uint32_t>(bytesRead);
            if (conn->offset == msgSize)
            {
                if (conn->startCycles >= warmupEndCycles)
                {
                    uint64_t end = silk::Tsc::getCycles();
                    conn->latencies.push_back(silk::Tsc::cyclesToNanoseconds(end - conn->startCycles));
                }
                conn->state = State::Writing;
                conn->offset = 0;
                conn->startCycles = silk::Tsc::getCycles();
            }
            continue;
        }
        if (r == EAGAIN)
        {
            conn->readable = false;
            return true;
        }
        if (!isExpectedShutdown(r))
        {
            SILK_ERROR("read failed: {}", std::strerror(r));
        }
        return false;
    }
}

void Client::workerMain(WorkerParams params) noexcept
{
    Client * client = params.client;
    Worker * worker = params.worker;

    Epoll epoll;
    int r = Epoll::create(&epoll);
    SILK_ASSERT(!r, "Epoll::create failed: {}", std::strerror(r));

    StopTag stopTag;
    r = epoll.add(worker->stopEvent.getFd(), EPOLLIN, &stopTag);
    SILK_ASSERT(!r);

    uint64_t startCycles = silk::Tsc::getCycles();
    for (uint32_t i = 0; i < worker->connCount; ++i)
    {
        Connection * conn = &worker->connections[i];
        conn->startCycles = startCycles;
        r = epoll.add(conn->conn.getFd(), EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP, conn);
        SILK_ASSERT(!r);
    }

    epoll_event events[EPOLL_BATCH];

    bool stop = false;
    while (!stop)
    {
        int eventCount = 0;
        r = epoll.wait(events, EPOLL_BATCH, -1, &eventCount);
        if (r == EINTR)
        {
            continue;
        }
        SILK_ASSERT(!r, "epoll_wait failed: {}", std::strerror(r));

        uint64_t warmupEndCycles = client->warmupEndCycles.load(std::memory_order_relaxed);

        for (int i = 0; i < eventCount; ++i)
        {
            EventTag tag = *static_cast<EventTag *>(events[i].data.ptr);
            if (tag == EventTag::Stop)
            {
                stop = true;
                break;
            }

            Connection * conn = static_cast<Connection *>(events[i].data.ptr);
            uint32_t mask = events[i].events;
            if (mask & EPOLLIN)
            {
                conn->readable = true;
            }
            if (mask & EPOLLOUT)
            {
                conn->writable = true;
            }

            bool keepAlive = drive(conn, client->cfg, warmupEndCycles);
            if (!keepAlive || eventClosesConnection(mask))
            {
                int dr = epoll.del(conn->conn.getFd());
                SILK_UNUSED(dr);
            }
        }
    }
}

static void printJson(std::vector<uint64_t> & latNs, const ClientConfig & cfg)
{
    uint64_t total = latNs.size();
    double durationS = static_cast<double>(cfg.durationNs) / 1e9;
    double rps = static_cast<double>(total) / durationS;
    double bwBytesS = rps * cfg.msgSize;

    printf("{\n");
    printf("  \"connections\": %u,\n", cfg.numConnections);
    printf("  \"threads\": %u,\n", cfg.threads);
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

/**
 * Server entry point.
 */
static void runServer(int argc, char ** argv)
{
    ServerConfig cfg;
    bool verbose = false;

    namespace po = boost::program_options;
    po::options_description desc("net-perf-epoll server options");

    std::string delayStr = "0";

    // clang-format off
    desc.add_options()
        ("help,h", "show this help")
        ("host",     po::value(&cfg.host),    "listen host")
        ("port",     po::value(&cfg.port),    "listen port")
        ("msg-size", po::value(&cfg.msgSize), "echo message size in bytes")
        ("threads",  po::value(&cfg.threads), "worker threads (each owns SO_REUSEPORT listener)")
        ("delay",    po::value(&delayStr),    "server-side delay per message (must be 0; not supported)")
        ("verbose,v", po::bool_switch(&verbose), "enable debug logging")
        ;
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << "usage: net-perf-epoll server [options]\n" << desc << "\n";
            return;
        }
        po::notify(vm);
        if (parseDuration(delayStr) != 0)
        {
            std::cerr << "error: --delay is not supported by net-perf-epoll\n";
            std::exit(1);
        }
        if (cfg.threads == 0)
        {
            cfg.threads = silk::getAvailableProcessorCount();
        }
        if (verbose)
        {
            silk::Logger::setLevel(silk::LogLevel::DEBUG);
        }
    }
    catch (const po::error & ex)
    {
        std::cerr << "error: " << ex.what() << "\n" << desc << "\n";
        std::exit(1);
    }

    sigset_t mask = blockSignals();
    silk::initialize();

    SILK_INFO("starting server on {}:{} with {} thread(s)", cfg.host, cfg.port, cfg.threads);

    Server server(cfg);
    server.start();

    int sig = 0;
    sigwait(&mask, &sig);
    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    SILK_INFO("stopping server");
    server.stop();

    silk::destroy();
}

/**
 * Client entry point.
 */
static void runClient(int argc, char ** argv)
{
    ClientConfig cfg;
    bool verbose = false;

    namespace po = boost::program_options;
    po::options_description desc("net-perf-epoll client options");

    std::string durationStr = "10s";
    std::string warmupStr = "2s";
    std::string stallDurationStr = "0";

    // clang-format off
    desc.add_options()
        ("help,h", "show this help")
        ("host",        po::value(&cfg.host),           "server host")
        ("port",        po::value(&cfg.port),           "server port")
        ("connections", po::value(&cfg.numConnections), "parallel connections (split across threads)")
        ("threads",     po::value(&cfg.threads),        "worker threads")
        ("msg-size",    po::value(&cfg.msgSize),        "message size in bytes")
        ("duration",    po::value(&durationStr),        "measurement duration (e.g. 10s, 500ms)")
        ("warmup",      po::value(&warmupStr),          "warmup duration (e.g. 2s, 500ms)")
        ("stall-rate",     po::value(&cfg.stallRateHz),   "per-connection Poisson rate of stall messages (Hz, 0 disables)")
        ("stall-duration", po::value(&stallDurationStr),  "stall duration per stall event (e.g. 100us, 1ms)")
        ("print-counters", po::bool_switch(&cfg.printCounters), "include counters in the JSON report")
        ("verbose,v",   po::bool_switch(&verbose),      "enable debug logging")
        ;
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << "usage: net-perf-epoll client [options]\n" << desc << "\n";
            return;
        }
        po::notify(vm);
        cfg.durationNs = parseDuration(durationStr);
        cfg.warmupNs = parseDuration(warmupStr);
        cfg.stallNs = parseDuration(stallDurationStr);
        if (cfg.threads == 0)
        {
            cfg.threads = silk::getAvailableProcessorCount();
        }
        if (cfg.threads > cfg.numConnections)
        {
            cfg.threads = cfg.numConnections;
        }
        if (verbose)
        {
            silk::Logger::setLevel(silk::LogLevel::DEBUG);
        }
    }
    catch (const po::error & ex)
    {
        std::cerr << "error: " << ex.what() << "\n" << desc << "\n";
        std::exit(1);
    }

    sigset_t mask = blockSignals();
    silk::initialize();

    SILK_INFO("starting client on {}:{} with {} thread(s) and {} connection(s)", cfg.host, cfg.port, cfg.threads, cfg.numConnections);

    Client client(cfg);
    client.start();

    bool signalled = false;

    if (cfg.warmupNs > 0)
    {
        SILK_INFO("warming up for {}...", formatDuration(cfg.warmupNs));
        signalled = sigwaitFor(mask, cfg.warmupNs);
    }

    if (!signalled)
    {
        SILK_INFO("measuring for {}...", formatDuration(cfg.durationNs));
        sigwaitFor(mask, cfg.durationNs);
    }

    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    SILK_INFO("stopping client");
    client.stop();

    std::vector<uint64_t> allLat = client.collectLatencies();
    printJson(allLat, cfg);

    silk::destroy();
}

/**
 * Main entry point.
 */
int main(int argc, char ** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: net-perf-epoll <server|client> [options]\n"
                  << "       net-perf-epoll <server|client> --help\n";
        return 1;
    }

    const char * subcmd = argv[1];
    if (std::strcmp(subcmd, "server") == 0)
    {
        runServer(argc - 1, argv + 1);
    }
    else if (std::strcmp(subcmd, "client") == 0)
    {
        runClient(argc - 1, argv + 1);
    }
    else
    {
        std::cerr << "unknown subcommand: " << subcmd << "\n"
                  << "usage: net-perf-epoll <server|client> [options]\n";
        return 1;
    }
    return 0;
}
