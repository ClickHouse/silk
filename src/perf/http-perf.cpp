#include "common.h"
#include "fiber-http.h"

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/fibers/mutex.h>
#include <silk/util/assert.h>
#include <silk/util/init.h>
#include <silk/util/list.h>
#include <silk/util/logger.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerConnection.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/StreamSocket.h>
#include <boost/program_options.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

//
// Client
//

struct ClientConfig
{
    std::string host = "127.0.0.1";
    uint16_t port = 80;
    uint32_t numConnections = 16;
    uint64_t durationNs = 10'000'000'000ULL;
    uint64_t warmupNs = 2'000'000'000ULL;
    bool useThreads = false;
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
        std::unique_ptr<Poco::Net::HTTPClientSession> session;
        silk::FiberFuture future;
        std::thread thread;
        std::vector<uint64_t> latencies;
    };

    //
    // Helpers.
    //

    void runLoop(Connection * connection) noexcept;

    //
    // Fiber main functions.
    //

    struct FiberParams
    {
        Client * client;
        Connection * connection;
    };
    static int fiberMain(FiberParams * params) noexcept;

    //
    // State.
    //

    ClientConfig cfg;
    std::vector<Connection> connections;
    std::atomic<uint64_t> warmupEndCycles{UINT64_MAX};
    std::atomic<bool> stopping{};
};

Client::Client(const ClientConfig & cfg)
    : cfg(cfg)
    , connections(cfg.numConnections)
{
}

void Client::start()
{
    for (Connection & conn : connections)
    {
        if (cfg.useThreads)
        {
            conn.session = std::make_unique<Poco::Net::HTTPClientSession>(cfg.host, cfg.port);
        }
        else
        {
            conn.session = std::make_unique<FiberHTTPClientSession>(cfg.host, cfg.port);
        }
        conn.session->setKeepAlive(true);
    }

    for (Connection & conn : connections)
    {
        if (cfg.useThreads)
        {
            conn.thread = std::thread([this, &conn] mutable { runLoop(&conn); });
        }
        else
        {
            int r = silk::FiberScheduler::run(fiberMain, {this, &conn}, &conn.future);
            SILK_ASSERT(!r, "cannot start fiber: {}", std::strerror(r));
        }
    }

    warmupEndCycles.store(silk::Tsc::getCycles() + silk::Tsc::nanosecondsToCycles(cfg.warmupNs), std::memory_order_relaxed);
}

void Client::stop()
{
    stopping.store(true, std::memory_order_relaxed);

    for (Connection & conn : connections)
    {
        try
        {
            conn.session->socket().shutdown();
        }
        catch (const Poco::Exception & e)
        {
            if (!isExpectedShutdown(e.code()))
            {
                SILK_ERROR("shutdown failed: {}", e.displayText());
            }
        }
    }

    for (Connection & conn : connections)
    {
        if (cfg.useThreads)
        {
            conn.thread.join();
        }
        else
        {
            int r = conn.future.wait();
            SILK_ASSERT(!r);
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

void Client::runLoop(Connection * conn) noexcept
{
    while (!stopping.load(std::memory_order_relaxed))
    {
        uint64_t start = silk::Tsc::getCycles();

        try
        {
            Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, "/", Poco::Net::HTTPMessage::HTTP_1_1);
            conn->session->sendRequest(request);

            Poco::Net::HTTPResponse response;
            std::istream & body = conn->session->receiveResponse(response);
            body.ignore(std::numeric_limits<std::streamsize>::max());
        }
        catch (const Poco::Exception & e)
        {
            if (!stopping.load(std::memory_order_relaxed) && !isExpectedShutdown(e.code()))
            {
                SILK_ERROR("HTTP request failed: {}", e.displayText());
            }
            break;
        }

        if (start >= warmupEndCycles.load(std::memory_order_relaxed))
        {
            uint64_t end = silk::Tsc::getCycles();
            conn->latencies.push_back(silk::Tsc::cyclesToNanoseconds(end - start));
        }
    }
}

int Client::fiberMain(FiberParams * params) noexcept
{
    params->client->runLoop(params->connection);
    return 0;
}

static void printJson(std::vector<uint64_t> & latNs, const ClientConfig & cfg)
{
    uint64_t total = latNs.size();
    double durationS = static_cast<double>(cfg.durationNs) / 1e9;
    double rps = static_cast<double>(total) / durationS;

    printf("{\n");
    printf("  \"connections\": %u,\n", cfg.numConnections);
    printf("  \"host\": \"%s\",\n", cfg.host.c_str());
    printf("  \"port\": %u,\n", cfg.port);
    printf("  \"duration_s\": %.3f,\n", durationS);
    printf("  \"total\": %lu,\n", total);
    printf("  \"rps\": %.1f,\n", rps);
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
 * Client entry point.
 */
static void runClient(int argc, char ** argv)
{
    ClientConfig cfg;
    std::string durationStr = "10s";
    std::string warmupStr = "2s";
    bool verbose = false;

    namespace po = boost::program_options;
    po::options_description desc("http-perf client options");

    // clang-format off
    desc.add_options()
        ("help,h",      "show this help")
        ("host",        po::value(&cfg.host),             "server host")
        ("port",        po::value(&cfg.port),             "server port")
        ("connections", po::value(&cfg.numConnections),   "parallel connections or threads")
        ("threads",     po::bool_switch(&cfg.useThreads), "use OS threads instead of fibers")
        ("duration",    po::value(&durationStr),          "measurement duration (e.g. 10s, 500ms)")
        ("warmup",      po::value(&warmupStr),            "warmup duration (e.g. 2s, 500ms)")
        ("print-counters", po::bool_switch(&cfg.printCounters), "enable per-CPU profiler and include counters in the JSON report")
        ("verbose,v",   po::bool_switch(&verbose),        "enable debug logging")
        ;
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << "usage: http-perf client [options]\n" << desc << "\n";
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
    bool signalled = false;

    silk::initialize();
    if (!cfg.useThreads)
    {
        silk::FiberScheduler::Options options{.enableProfiler = cfg.printCounters};
        silk::FiberScheduler::initialize(&options);
    }

    SILK_INFO(
        "starting {} http client, host={}:{}, connections={}",
        cfg.useThreads ? "threaded" : "fiber",
        cfg.host,
        cfg.port,
        cfg.numConnections);

    Client client(cfg);
    client.start();

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

    if (!cfg.useThreads)
    {
        silk::FiberScheduler::destroy();
    }
    silk::destroy();
}

//
// Server
//

struct ServerConfig
{
    uint16_t port = 8080;
    uint32_t maxQueued = 0;
    uint64_t delayNs = 0;
    bool useThreads = false;
    bool printCounters = false;
};

struct EchoHandlerConfig
{
    uint64_t delayNs = 0;
    bool fiberSleep = false;
};

class EchoHandler final : public Poco::Net::HTTPRequestHandler
{
public:
    explicit EchoHandler(const EchoHandlerConfig & cfg)
        : cfg(cfg)
    {
    }

    void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
    {
        SILK_UNUSED(request);

        if (cfg.delayNs)
        {
            if (cfg.fiberSleep)
            {
                silk::FiberScheduler::sleep(cfg.delayNs);
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::nanoseconds(cfg.delayNs));
            }
        }

        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        response.setContentLength(0);
        response.send();
    }

private:
    const EchoHandlerConfig & cfg;
};

class EchoHandlerFactory final : public Poco::Net::HTTPRequestHandlerFactory
{
public:
    explicit EchoHandlerFactory(const EchoHandlerConfig & cfg)
        : cfg(cfg)
    {
    }

    Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest & request) override
    {
        SILK_UNUSED(request);
        return new EchoHandler(cfg);
    }

private:
    EchoHandlerConfig cfg;
};

/**
 * One accept fiber, one fiber per connection.
 *
 * Per-connection logic is delegated to Poco::Net::HTTPServerConnection over a
 * FiberSocketImpl-backed StreamSocket so all read/write I/O suspends fibers
 * instead of blocking threads.
 */
class FiberHTTPServer
{
public:
    FiberHTTPServer(Poco::Net::HTTPRequestHandlerFactory::Ptr factory, FiberServerSocket socket, Poco::Net::HTTPServerParams::Ptr params)
        : factory(std::move(factory))
        , params(std::move(params))
        , socket(std::move(socket))
    {
    }

    void start();
    void stop();

private:
    struct Conn
    {
        silk::ListEntry listEntry;
        Poco::Net::StreamSocket socket;
        silk::FiberFuture future;
    };

    //
    // Fiber main functions.
    //

    struct AcceptFiberParams
    {
        FiberHTTPServer * server;
    };
    static int acceptFiberMain(AcceptFiberParams * params) noexcept
    {
        params->server->acceptLoop();
        return 0;
    }

    struct ConnFiberParams
    {
        FiberHTTPServer * server;
        Poco::Net::StreamSocket socket;
    };
    static int connFiberMain(ConnFiberParams * params) noexcept
    {
        params->server->connectionLoop(params->socket);
        return 0;
    }

    //
    // Helpers.
    //

    void acceptLoop() noexcept;
    void connectionLoop(Poco::Net::StreamSocket socket) noexcept;

    //
    // State.
    //

    Poco::Net::HTTPRequestHandlerFactory::Ptr factory;
    Poco::Net::HTTPServerParams::Ptr params;

    FiberServerSocket socket;
    std::atomic<bool> stopping{false};
    silk::FiberFuture acceptFuture;

    silk::FiberMutex connsMutex;
    silk::List<Conn, &Conn::listEntry> conns;
};

void FiberHTTPServer::start()
{
    int r = silk::FiberScheduler::run(acceptFiberMain, AcceptFiberParams{this}, &acceptFuture);
    SILK_ASSERT(r == 0, "spawn accept fiber: {}", std::strerror(r));
}

void FiberHTTPServer::stop()
{
    stopping.store(true, std::memory_order_relaxed);

    // shutdown wakes a pending io_uring poll on the listen socket via POLLHUP;
    // close() alone is not guaranteed to deliver a CQE to the accept fiber.
    try
    {
        socket.shutdown();
    }
    catch (const Poco::Exception & e)
    {
        if (!isExpectedShutdown(e.code()))
        {
            SILK_ERROR("shutdown failed: {}", e.displayText());
        }
    }

    int r = acceptFuture.wait();
    SILK_ASSERT(r == 0, "accept fiber: {}", std::strerror(r));

    socket.close();

    {
        std::lock_guard lock(connsMutex);
        for (Conn * c = conns.front(); c; c = conns.next(c))
        {
            try
            {
                c->socket.shutdown();
            }
            catch (const Poco::Exception & e)
            {
                if (!isExpectedShutdown(e.code()))
                {
                    SILK_ERROR("shutdown failed: {}", e.displayText());
                }
            }
        }
    }

    while (Conn * c = conns.pop_front())
    {
        c->future.wait();
        delete c;
    }
}

void FiberHTTPServer::acceptLoop() noexcept
{
    while (!stopping.load(std::memory_order_relaxed))
    {
        Poco::Net::StreamSocket clientSocket;
        try
        {
            clientSocket = socket.acceptConnection();
        }
        catch (const Poco::Exception & e)
        {
            if (!stopping.load(std::memory_order_relaxed) && !isExpectedShutdown(e.code()))
            {
                SILK_ERROR("accept failed: {}", e.displayText());
            }
            return;
        }

        Conn * conn = new Conn();
        conn->socket = clientSocket;
        {
            std::lock_guard lock(connsMutex);
            conns.push_back(conn);
        }

        int r = silk::FiberScheduler::run(connFiberMain, ConnFiberParams{this, clientSocket}, &conn->future);
        if (r != 0)
        {
            SILK_ERROR("spawn conn fiber: {}", std::strerror(r));
            {
                std::lock_guard lock(connsMutex);
                conns.remove(conn);
            }
            delete conn;
            return;
        }
    }
}

void FiberHTTPServer::connectionLoop(Poco::Net::StreamSocket socket) noexcept
{
    try
    {
        Poco::Net::HTTPServerConnection conn(socket, params, factory);
        conn.run();
    }
    catch (const Poco::Exception & e)
    {
        if (!stopping.load(std::memory_order_relaxed) && !isExpectedShutdown(e.code()))
        {
            SILK_ERROR("connection error: {}", e.displayText());
        }
    }
}

/**
 * Server entry point.
 */
static void runServer(int argc, char ** argv)
{
    ServerConfig cfg;
    std::string delayStr = "0";
    bool verbose = false;

    namespace po = boost::program_options;
    po::options_description desc("http-perf server options");

    // clang-format off
    desc.add_options()
        ("help,h",    "show this help")
        ("port",      po::value(&cfg.port),       "listen port")
        ("queued",    po::value(&cfg.maxQueued),  "max queued connections (default: 4 * available CPUs)")
        ("delay",     po::value(&delayStr),       "per-request response delay (e.g. 5ms, 100us)")
        ("threads",   po::bool_switch(&cfg.useThreads), "use OS threads instead of fibers")
        ("print-counters", po::bool_switch(&cfg.printCounters), "enable per-CPU profiler and include counters in the JSON report")
        ("verbose,v", po::bool_switch(&verbose),  "enable debug logging")
        ;
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << "usage: http-perf server [options]\n" << desc << "\n";
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

    uint32_t numProcessors = silk::getAvailableProcessorCount();
    if (cfg.maxQueued == 0)
    {
        cfg.maxQueued = numProcessors * 4;
    }

    sigset_t mask = blockSignals();

    silk::initialize();
    if (!cfg.useThreads)
    {
        silk::FiberScheduler::Options options{.enableProfiler = cfg.printCounters};
        silk::FiberScheduler::initialize(&options);
    }

    Poco::Net::HTTPServerParams::Ptr params = new Poco::Net::HTTPServerParams();
    params->setMaxThreads(static_cast<int>(numProcessors));
    params->setMaxQueued(static_cast<int>(cfg.maxQueued));
    params->setKeepAlive(true);

    EchoHandlerConfig handlerCfg{.delayNs = cfg.delayNs, .fiberSleep = !cfg.useThreads};
    Poco::Net::HTTPRequestHandlerFactory::Ptr factory = new EchoHandlerFactory(handlerCfg);

    SILK_INFO(
        "starting {} http server on port {}, queued={}, delay={}",
        cfg.useThreads ? "threaded" : "fiber",
        cfg.port,
        cfg.maxQueued,
        formatDuration(cfg.delayNs));

    if (cfg.useThreads)
    {
        Poco::Net::ServerSocket socket(cfg.port);
        Poco::Net::HTTPServer server(factory, socket, params);
        server.start();

        int sig = 0;
        sigwait(&mask, &sig);

        SILK_INFO("stopping http server");
        server.stopAll();
    }
    else
    {
        FiberServerSocket socket(cfg.port);
        FiberHTTPServer server(factory, socket, params);
        server.start();

        int sig = 0;
        sigwait(&mask, &sig);

        SILK_INFO("stopping http server");
        server.stop();
    }

    if (cfg.printCounters)
    {
        printf("{\n");
        if (!cfg.useThreads)
        {
            printSchedulerLatency();
            printf(",");
        }
        printCounters();
        printf("}\n");
    }

    if (!cfg.useThreads)
    {
        silk::FiberScheduler::destroy();
    }
    silk::destroy();
}

/**
 * Main entry point.
 */
int main(int argc, char ** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: http-perf <client|server> [options]\n"
                  << "       http-perf <client|server> --help\n";
        return 1;
    }

    const char * subcmd = argv[1];
    if (strcmp(subcmd, "client") == 0)
    {
        runClient(argc - 1, argv + 1);
    }
    else if (strcmp(subcmd, "server") == 0)
    {
        runServer(argc - 1, argv + 1);
    }
    else
    {
        std::cerr << "unknown subcommand: " << subcmd << "\n"
                  << "usage: http-perf <client|server> [options]\n";
        return 1;
    }
    return 0;
}
