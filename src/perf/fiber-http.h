#pragma once

#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/StreamSocketImpl.h>

#include <cstdint>
#include <string>

//
// FiberSocketImpl - fiber-aware StreamSocketImpl backed by silk::FiberScheduler.
//
// Overrides connect/poll/sendBytes/receiveBytes to suspend the calling fiber
// during I/O instead of blocking the OS thread.
//

class FiberSocketImpl final : public Poco::Net::StreamSocketImpl
{
public:
    void connect(const Poco::Net::SocketAddress & address) override;
    void connect(const Poco::Net::SocketAddress & address, const Poco::Timespan & timeout) override;
    bool poll(const Poco::Timespan & timeout, int mode) override;
    int sendBytes(const void * buffer, int length, int flags) override;
    int receiveBytes(void * buffer, int length, int flags) override;
};

//
// FiberHTTPClientSession - HTTPClientSession that uses FiberSocketImpl.
//

class FiberHTTPClientSession : public Poco::Net::HTTPClientSession
{
public:
    FiberHTTPClientSession(const std::string & host, uint16_t port)
        : HTTPClientSession(host, port)
    {
        attachSocket(new FiberSocketImpl());
    }
};
