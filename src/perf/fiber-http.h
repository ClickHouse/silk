#pragma once

#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/ServerSocketImpl.h>
#include <Poco/Net/StreamSocketImpl.h>

#include <atomic>
#include <cstdint>
#include <string>

/**
 * Fiber-aware StreamSocketImpl backed by silk::FiberScheduler.
 *
 * Overrides connect/poll/sendBytes/receiveBytes to suspend the calling fiber
 * during I/O instead of blocking the OS thread.
 */
class FiberSocketImpl final : public Poco::Net::StreamSocketImpl
{
public:
    FiberSocketImpl() = default;

    /** Wrap an already-accepted fd (must be non-blocking). */
    explicit FiberSocketImpl(int sockfd);

    void connect(const Poco::Net::SocketAddress & address) override;
    void connect(const Poco::Net::SocketAddress & address, const Poco::Timespan & timeout) override;
    bool poll(const Poco::Timespan & timeout, int mode) override;
    int sendBytes(const void * buffer, int length, int flags) override;
    int receiveBytes(void * buffer, int length, int flags) override;

    /**
     * Race-free shutdown for cross-thread teardown: SocketImpl::_sockfd is
     * written by Poco's close() inside the fiber's I/O error path, so reading
     * it from another thread (as the base shutdown() does) races. We mirror
     * the fd into an atomic on init and ::shutdown that copy directly.
     */
    void shutdown() override;

private:
    std::atomic<int> atomicFd{-1};
};

/**
 * HTTPClientSession that uses FiberSocketImpl.
 */
class FiberHTTPClientSession : public Poco::Net::HTTPClientSession
{
public:
    FiberHTTPClientSession(const std::string & host, uint16_t port)
        : HTTPClientSession(host, port)
    {
        attachSocket(new FiberSocketImpl());
    }
};

/**
 * Fiber-aware ServerSocketImpl. acceptConnection waits for a pending connection
 * via silk::FiberScheduler::poll, then accepts it as a FiberSocketImpl-backed
 * StreamSocket.
 */
class FiberServerSocketImpl final : public Poco::Net::ServerSocketImpl
{
public:
    Poco::Net::SocketImpl * acceptConnection(Poco::Net::SocketAddress & clientAddr) override;
};

/**
 * ServerSocket that uses FiberServerSocketImpl.
 */
class FiberServerSocket : public Poco::Net::ServerSocket
{
public:
    FiberServerSocket()
        : Poco::Net::ServerSocket(new FiberServerSocketImpl(), true)
    {
    }

    explicit FiberServerSocket(uint16_t port, int backlog = 64)
        : Poco::Net::ServerSocket(new FiberServerSocketImpl(), true)
    {
        bind(Poco::Net::SocketAddress(Poco::Net::IPAddress(), port), true);
        listen(backlog);
        // FiberServerSocketImpl::acceptConnection requires non-blocking mode
        // so accept4 returns immediately after silk's poll fires.
        setBlocking(false);
    }

    void shutdown() { impl()->shutdown(); }
};
