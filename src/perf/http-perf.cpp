#include "common.h"
#include "fiber-http.h"

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/init.h>
#include <silk/util/logger.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/tsc.h>

#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/NetException.h>
#include <boost/program_options.hpp>

#include <atomic>
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

#include <poll.h>
#include <pthread.h>
#include <unistd.h>

//
// Benchmark
//

struct ClientConfig
{
    std::string host = "127.0.0.1";
    uint16_t port = 80;
    uint32_t numConnections = 16;
    uint64_t durationNs = 10'000'000'000ULL;
    uint64_t warmupNs = 2'000'000'000ULL;
    bool useThreads = false;
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
    // silk::Fiber main functions.
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
            ASSERT(!r, "cannot start fiber: {}", std::strerror(r));
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
            conn.session->abort();
        }
        catch (const Poco::Exception & e)
        {
            if (!isExpectedShutdown(e.code()))
            {
                LOG_ERROR("abort failed: {}", e.displayText());
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
            ASSERT(!r);
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
            if (!isExpectedShutdown(e.code()))
            {
                LOG_ERROR("HTTP request failed: {}", e.displayText());
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
    printCounters();
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
        silk::FiberScheduler::initialize();
    }

    LOG_INFO(
        "starting {} http client, host={}:{}, connections={}",
        cfg.useThreads ? "threaded" : "fiber",
        cfg.host,
        cfg.port,
        cfg.numConnections);

    Client client(cfg);
    client.start();

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
        std::cerr << "usage: http-perf <client> [options]\n"
                  << "       http-perf <client> --help\n";
        return 1;
    }

    const char * subcmd = argv[1];
    if (strcmp(subcmd, "client") == 0)
    {
        runClient(argc - 1, argv + 1);
    }
    else
    {
        std::cerr << "unknown subcommand: " << subcmd << "\n"
                  << "usage: http-perf <client> [options]\n";
        return 1;
    }
    return 0;
}
