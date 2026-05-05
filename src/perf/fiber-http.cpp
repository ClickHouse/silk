#include "fiber-http.h"

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/platform.h>

#include <Poco/Net/StreamSocket.h>

#include <poll.h>
#include <unistd.h>

#include <sys/socket.h>

// Use silk::FiberScheduler::read/write (io_uring) instead of recv/send + poll.
#define USE_IO_URING_RW

void FiberSocketImpl::connect(const Poco::Net::SocketAddress & address)
{
    connect(address, Poco::Timespan(-1));
}

void FiberSocketImpl::connect(const Poco::Net::SocketAddress & address, const Poco::Timespan & timeout)
{
    init(address.af());
    setNoDelay(true);
    setBlocking(false);

    int r = ::connect(sockfd(), address.addr(), address.length());
    if (r < 0)
    {
        r = errno;
        if (r != EINPROGRESS)
        {
            error(r, address.toString());
        }

        silk::FiberScheduler::IoFuture pollFuture;
        silk::FiberScheduler::poll(sockfd(), POLLOUT, nullptr, &pollFuture);

        Poco::Timestamp::TimeDiff timeoutUs = timeout.totalMicroseconds();
        if (timeoutUs >= 0)
        {
            r = silk::FiberFuture::waitWithTimeout(&pollFuture, static_cast<uint64_t>(timeoutUs) * 1000);
            if (r == ETIMEDOUT)
            {
                pollFuture.cancel();
                pollFuture.wait();
                error(ETIMEDOUT, address.toString());
            }
        }
        else
        {
            r = pollFuture.wait();
        }

        if (r)
        {
            error(r, address.toString());
        }
    }

    r = socketError();
    if (r)
    {
        error(r, address.toString());
    }
}

bool FiberSocketImpl::poll(const Poco::Timespan & timeout, int mode)
{
    ASSERT(!getBlocking());

    uint32_t events = 0;
    if (mode & SELECT_READ)
    {
        events |= POLLIN;
    }
    if (mode & SELECT_WRITE)
    {
        events |= POLLOUT;
    }
    if (mode & SELECT_ERROR)
    {
        events |= POLLERR;
    }

    uint64_t triggered = 0;
    silk::FiberScheduler::IoFuture pollFuture;
    silk::FiberScheduler::poll(sockfd(), events, &triggered, &pollFuture);

    int r;
    Poco::Timestamp::TimeDiff timeoutUs = timeout.totalMicroseconds();
    if (timeoutUs >= 0)
    {
        r = silk::FiberFuture::waitWithTimeout(&pollFuture, static_cast<uint64_t>(timeoutUs) * 1000);
        if (r == ETIMEDOUT)
        {
            pollFuture.cancel();
            pollFuture.wait();
            return false;
        }
    }
    else
    {
        r = pollFuture.wait();
    }

    if (r)
    {
        error(r, "poll");
    }
    return (triggered & static_cast<uint64_t>(events)) != 0;
}

int FiberSocketImpl::sendBytes(const void * buffer, int length, int flags)
{
    UNUSED(flags);
    ASSERT(!getBlocking());

    int total = 0;
    const char * ptr = static_cast<const char *>(buffer);
    while (total < length)
    {
#if defined(USE_IO_URING_RW)
        uint64_t bytesWritten = 0;
        int r = silk::FiberScheduler::write(sockfd(), ptr, static_cast<uint64_t>(length - total), 0, &bytesWritten);
        if (r)
        {
            error(r, "send");
        }
        total += static_cast<int>(bytesWritten);
        ptr += bytesWritten;
#else
        ssize_t count = ::send(sockfd(), ptr, static_cast<size_t>(length - total), MSG_NOSIGNAL);
        if (count < 0)
        {
            int r = errno;
            if (r == EAGAIN)
            {
                r = silk::FiberScheduler::poll(sockfd(), POLLOUT);
                if (!r)
                {
                    continue;
                }
            }
            error(r, "send");
        }
        total += static_cast<int>(count);
        ptr += count;
#endif
    }
    return total;
}

int FiberSocketImpl::receiveBytes(void * buffer, int length, int flags)
{
    UNUSED(flags);
    ASSERT(!getBlocking());

#if defined(USE_IO_URING_RW)
    uint64_t bytesRead = 0;
    int r = silk::FiberScheduler::read(sockfd(), buffer, static_cast<uint64_t>(length), 0, &bytesRead);
    if (r)
    {
        error(r, "recv");
    }
    return static_cast<int>(bytesRead);
#else
    for (;;)
    {
        ssize_t count = ::recv(sockfd(), buffer, static_cast<size_t>(length), 0);
        if (count >= 0)
        {
            return static_cast<int>(count);
        }
        int r = errno;
        if (r == EAGAIN)
        {
            r = silk::FiberScheduler::poll(sockfd(), POLLIN);
            if (!r)
            {
                continue;
            }
        }
        error(r, "recv");
    }
#endif
}
