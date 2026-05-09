#include "common.h"
#include "fiber-http.h"

#include <silk/fibers/fiber.h>
#include <silk/fibers/future.h>
#include <silk/util/assert.h>
#include <silk/util/init.h>
#include <silk/util/logger.h>
#include <silk/util/perf.h>
#include <silk/util/platform.h>
#include <silk/util/stack.h>
#include <silk/util/tsc.h>

#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/NetException.h>
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/s3/S3EndpointProvider.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <boost/program_options.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <unistd.h>

//
// I/O modes.
//

static constexpr uint32_t MODE_READ = 0;
static constexpr uint32_t MODE_WRITE = 1;
static constexpr uint32_t MODE_READWRITE = 2;

static const char * modeName(uint32_t mode)
{
    if (mode == MODE_READ)
    {
        return "read";
    }
    if (mode == MODE_WRITE)
    {
        return "write";
    }
    if (mode == MODE_READWRITE)
    {
        return "readwrite";
    }
    return "unknown";
}

static bool parseMode(const std::string & name, uint32_t * mode)
{
    if (name == "read")
    {
        *mode = MODE_READ;
        return true;
    }
    if (name == "write")
    {
        *mode = MODE_WRITE;
        return true;
    }
    if (name == "readwrite")
    {
        *mode = MODE_READWRITE;
        return true;
    }
    return false;
}

static std::pair<Aws::Http::Scheme, std::string> parseEndpointUrl(const std::string & url)
{
    if (url.starts_with("https://"))
    {
        return {Aws::Http::Scheme::HTTPS, url.substr(8)};
    }
    if (url.starts_with("http://"))
    {
        return {Aws::Http::Scheme::HTTP, url.substr(7)};
    }
    return {Aws::Http::Scheme::HTTP, url};
}

/**
 * CachingS3EndpointProvider - bypasses the CRT RuleEngine entirely by building
 * the AWSEndpoint directly from the configured URL. The URL must already include
 * the bucket path segment for path-style access (e.g. http://host:port/bucket),
 * because S3Client only appends the key via AddPathSegments -- it does not add
 * the bucket. InitBuiltInParameters is called on the main thread during S3Client
 * construction, before any requests, so ResolveEndpoint just returns the
 * pre-built endpoint with no synchronization.
 */
class CachingS3EndpointProvider : public Aws::S3::Endpoint::S3EndpointProvider
{
public:
    using ResolveEndpointOutcome = Aws::S3::Endpoint::S3EndpointProvider::S3ResolveEndpointOutcome;

    explicit CachingS3EndpointProvider(Aws::String url)
        : endpointUrl(std::move(url))
    {
    }

    void InitBuiltInParameters(const Aws::S3::S3ClientConfiguration & config) override
    {
        SILK_UNUSED(config);
        buildCachedOutcome();
    }

    void InitBuiltInParameters(const Aws::S3::S3ClientConfiguration & config, const Aws::String & serviceName) override
    {
        SILK_UNUSED(config);
        SILK_UNUSED(serviceName);
        buildCachedOutcome();
    }

    ResolveEndpointOutcome ResolveEndpoint(const Aws::Endpoint::EndpointParameters &) const override { return *cachedOutcome; }

private:
    void buildCachedOutcome()
    {
        Aws::Endpoint::AWSEndpoint endpoint;
        endpoint.SetURL(endpointUrl);
        cachedOutcome = ResolveEndpointOutcome(std::move(endpoint));
    }

    Aws::String endpointUrl;
    std::optional<ResolveEndpointOutcome> cachedOutcome;
};

struct ClientConfig
{
    std::string endpointUrl = "http://127.0.0.1:9000";
    std::string region = "us-east-1";
    std::string bucket = "test-bucket";
    std::string key = "test-object";
    std::string accessKeyId = "minioadmin";
    std::string secretAccessKey = "minioadmin";
    uint32_t objectSize = 4096;
    uint32_t numJobs = 1;
    uint32_t ioDepth = 16;
    uint32_t mode = MODE_READ;
    uint64_t durationNs = 10'000'000'000ULL;
    uint64_t warmupNs = 2'000'000'000ULL;
    bool useThreads = false;
    bool printCounters = false;
};

static std::unique_ptr<Aws::S3::S3Client>
makeS3Client(const ClientConfig & cfg, std::shared_ptr<Aws::Utils::Threading::Executor> executor = nullptr)
{
    auto [scheme, endpoint] = parseEndpointUrl(cfg.endpointUrl);

    Aws::Auth::AWSCredentials creds(cfg.accessKeyId, cfg.secretAccessKey);

    Aws::S3::S3ClientConfiguration s3Config(false, "legacy");
    s3Config.region = cfg.region;
    s3Config.endpointOverride = endpoint;
    s3Config.scheme = scheme;
    s3Config.verifySSL = false;
    s3Config.useVirtualAddressing = false;
    s3Config.payloadSigningPolicy = Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never;
    s3Config.executor = std::move(executor);
    // The bench measures single-attempt latency. With retries enabled the SDK
    // would re-read slot.body on retry, which conflicts with the slot's reuse
    // across iterations and would muddy the latency distribution.
    s3Config.retryStrategy = std::make_shared<Aws::Client::DefaultRetryStrategy>(0L, 0L);

    return std::make_unique<Aws::S3::S3Client>(
        creds, std::make_shared<CachingS3EndpointProvider>(cfg.endpointUrl + "/" + cfg.bucket), s3Config);
}

static void printJson(std::vector<uint64_t> & latNs, const ClientConfig & cfg, uint32_t failedSessions)
{
    uint64_t total = latNs.size();
    double durationS = static_cast<double>(cfg.durationNs) / 1e9;
    double opsPerSec = static_cast<double>(total) / durationS;

    printf("{\n");
    printf("  \"rw\": \"%s\",\n", modeName(cfg.mode));
    printf("  \"endpoint\": \"%s\",\n", cfg.endpointUrl.c_str());
    printf("  \"bucket\": \"%s\",\n", cfg.bucket.c_str());
    printf("  \"key\": \"%s\",\n", cfg.key.c_str());
    printf("  \"object_size\": %u,\n", cfg.objectSize);
    printf("  \"numjobs\": %u,\n", cfg.numJobs);
    printf("  \"iodepth\": %u,\n", cfg.ioDepth);
    printf("  \"threads\": %s,\n", cfg.useThreads ? "true" : "false");
    printf("  \"duration_s\": %.3f,\n", durationS);
    printf("  \"total\": %lu,\n", total);
    printf("  \"rps\": %.1f,\n", opsPerSec);
    printf("  \"errors\": %u,\n", failedSessions);
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
 * PocoHttpClient
 */
class PocoHttpClient : public Aws::Http::HttpClient
{
public:
    explicit PocoHttpClient(const Aws::Client::ClientConfiguration & config) { SILK_UNUSED(config); }
    ~PocoHttpClient();

    std::shared_ptr<Aws::Http::HttpResponse> MakeRequest(
        const std::shared_ptr<Aws::Http::HttpRequest> & request,
        Aws::Utils::RateLimits::RateLimiterInterface * readLimiter,
        Aws::Utils::RateLimits::RateLimiterInterface * writeLimiter) const override;

protected:
    virtual std::unique_ptr<Poco::Net::HTTPClientSession> makeSession(const Aws::Http::URI & uri) const;

private:
    static constexpr uint64_t SESSION_BUFFER_SIZE = 8 * 1024 - 16;

    struct PooledSession
    {
        silk::StackEntry entry;
        std::unique_ptr<Poco::Net::HTTPClientSession> session;
        char buffer[SESSION_BUFFER_SIZE];
    };

    PooledSession * acquireSession(const Aws::Http::URI & uri) const;
    void releaseSession(PooledSession * node, bool sessionOk) const;

    static const char * getMethod(Aws::Http::HttpMethod method);
    static void writeHeaders(Poco::Net::HTTPRequest & pocoReq, const Aws::Http::HeaderValueCollection & headers);
    static void readHeaders(Aws::Http::HttpResponse & response, const Poco::Net::HTTPResponse & pocoResp);
    static void copyStream(std::ostream & out, std::istream & in, char * buffer);

    mutable silk::LockFreeStack<PooledSession, &PooledSession::entry> sessionPool;
};

PocoHttpClient::~PocoHttpClient()
{
    while (PooledSession * node = sessionPool.pop())
    {
        delete node;
    }
}

std::unique_ptr<Poco::Net::HTTPClientSession> PocoHttpClient::makeSession(const Aws::Http::URI & uri) const
{
    return std::make_unique<Poco::Net::HTTPClientSession>(uri.GetAuthority(), uri.GetPort());
}

PocoHttpClient::PooledSession * PocoHttpClient::acquireSession(const Aws::Http::URI & uri) const
{
    PooledSession * node = sessionPool.pop();
    if (!node)
    {
        node = new PooledSession;
        node->session = makeSession(uri);
        node->session->setKeepAlive(true);
    }
    return node;
}

void PocoHttpClient::releaseSession(PooledSession * node, bool sessionOk) const
{
    if (sessionOk)
    {
        sessionPool.push(node);
    }
    else
    {
        node->session->abort();
        delete node;
    }
}

const char * PocoHttpClient::getMethod(Aws::Http::HttpMethod method)
{
    switch (method)
    {
        case Aws::Http::HttpMethod::HTTP_GET:
            return "GET";
        case Aws::Http::HttpMethod::HTTP_POST:
            return "POST";
        case Aws::Http::HttpMethod::HTTP_PUT:
            return "PUT";
        case Aws::Http::HttpMethod::HTTP_DELETE:
            return "DELETE";
        case Aws::Http::HttpMethod::HTTP_HEAD:
            return "HEAD";
        case Aws::Http::HttpMethod::HTTP_PATCH:
            return "PATCH";
        case Aws::Http::HttpMethod::HTTP_CONNECT:
            return "CONNECT";
        case Aws::Http::HttpMethod::HTTP_OPTIONS:
            return "OPTIONS";
        case Aws::Http::HttpMethod::HTTP_TRACE:
            return "TRACE";
    }
    SILK_ASSERT(false);
}

void PocoHttpClient::writeHeaders(Poco::Net::HTTPRequest & pocoReq, const Aws::Http::HeaderValueCollection & headers)
{
    for (const auto & [name, value] : headers)
    {
        pocoReq.set(name, value);
    }
}

void PocoHttpClient::readHeaders(Aws::Http::HttpResponse & response, const Poco::Net::HTTPResponse & pocoResp)
{
    for (const auto & [name, value] : pocoResp)
    {
        response.AddHeader(name, value);
    }
}

void PocoHttpClient::copyStream(std::ostream & out, std::istream & in, char * buffer)
{
    for (;;)
    {
        in.read(buffer, SESSION_BUFFER_SIZE);
        std::streamsize n = in.gcount();
        if (n <= 0)
        {
            break;
        }
        out.write(buffer, n);
    }
}

std::shared_ptr<Aws::Http::HttpResponse> PocoHttpClient::MakeRequest(
    const std::shared_ptr<Aws::Http::HttpRequest> & request,
    Aws::Utils::RateLimits::RateLimiterInterface * readLimiter,
    Aws::Utils::RateLimits::RateLimiterInterface * writeLimiter) const
{
    SILK_UNUSED(readLimiter);
    SILK_UNUSED(writeLimiter);

    if (!IsRequestProcessingEnabled())
    {
        return nullptr;
    }

    PooledSession * node = acquireSession(request->GetUri());
    bool sessionOk = true;

    auto response = Aws::MakeShared<Aws::Http::Standard::StandardHttpResponse>("s3-perf", request);

    try
    {
        std::string pathAndQuery = request->GetUri().GetURLEncodedPath() + request->GetUri().GetQueryString();
        Poco::Net::HTTPRequest pocoReq(getMethod(request->GetMethod()), pathAndQuery, Poco::Net::HTTPMessage::HTTP_1_1);
        writeHeaders(pocoReq, request->GetHeaders());

        std::ostream & reqBodyStream = node->session->sendRequest(pocoReq);

        const auto & body = request->GetContentBody();
        if (body)
        {
            copyStream(reqBodyStream, *body, node->buffer);
        }

        Poco::Net::HTTPResponse pocoResp;
        std::istream & respBodyStream = node->session->receiveResponse(pocoResp);

        response->SetResponseCode(static_cast<Aws::Http::HttpResponseCode>(pocoResp.getStatus()));
        readHeaders(*response, pocoResp);
        copyStream(response->GetResponseBody(), respBodyStream, node->buffer);

        // Reset read position so the SDK can parse the body (errors, GetObject result).
        response->GetResponseBody().clear();
        response->GetResponseBody().seekg(0);
    }
    catch (const Poco::Exception & e)
    {
        sessionOk = false;
        response->SetClientErrorType(Aws::Client::CoreErrors::NETWORK_CONNECTION);
        response->SetClientErrorMessage(e.displayText());
    }
    catch (const std::exception & e)
    {
        sessionOk = false;
        response->SetClientErrorType(Aws::Client::CoreErrors::NETWORK_CONNECTION);
        response->SetClientErrorMessage(e.what());
    }

    releaseSession(node, sessionOk);
    return response;
}

class PocoHttpClientFactory : public Aws::Http::HttpClientFactory
{
public:
    std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(const Aws::Client::ClientConfiguration & config) const override
    {
        return Aws::MakeShared<PocoHttpClient>("s3-perf", config);
    }

    std::shared_ptr<Aws::Http::HttpRequest>
    CreateHttpRequest(const Aws::String & uri, Aws::Http::HttpMethod method, const Aws::IOStreamFactory & streamFactory) const override;

    std::shared_ptr<Aws::Http::HttpRequest>
    CreateHttpRequest(const Aws::Http::URI & uri, Aws::Http::HttpMethod method, const Aws::IOStreamFactory & streamFactory) const override;
};

std::shared_ptr<Aws::Http::HttpRequest> PocoHttpClientFactory::CreateHttpRequest(
    const Aws::String & uri, Aws::Http::HttpMethod method, const Aws::IOStreamFactory & streamFactory) const
{
    return CreateHttpRequest(Aws::Http::URI(uri), method, streamFactory);
}

std::shared_ptr<Aws::Http::HttpRequest> PocoHttpClientFactory::CreateHttpRequest(
    const Aws::Http::URI & uri, Aws::Http::HttpMethod method, const Aws::IOStreamFactory & streamFactory) const
{
    auto req = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>("s3-perf", uri, method);
    req->SetResponseStreamFactory(streamFactory);
    return req;
}

/**
 * FiberHttpClient - overrides makeSession to return a FiberHTTPClientSession
 * so I/O suspends the calling fiber instead of blocking the OS thread.
 */
class FiberHttpClient final : public PocoHttpClient
{
public:
    using PocoHttpClient::PocoHttpClient;

protected:
    std::unique_ptr<Poco::Net::HTTPClientSession> makeSession(const Aws::Http::URI & uri) const override
    {
        return std::make_unique<FiberHTTPClientSession>(uri.GetAuthority(), uri.GetPort());
    }
};

/**
 * FiberHttpClientFactory inherits CreateHttpRequest from PocoHttpClientFactory,
 * overriding only CreateHttpClient to vend FiberHttpClient instances.
 */
class FiberHttpClientFactory final : public PocoHttpClientFactory
{
public:
    std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(const Aws::Client::ClientConfiguration & config) const override
    {
        return Aws::MakeShared<FiberHttpClient>("s3-perf", config);
    }
};

/**
 * FiberExecutor - runs SDK async tasks as fibers instead of OS threads.
 *
 * Note: SubmitToThread is unbounded -- FiberScheduler::run always succeeds, so
 * unlike PooledThreadExecutor (which blocks when the pool saturates) this
 * executor offers no backpressure. The s3-perf bench is bounded externally by
 * numJobs * ioDepth, so this is harmless here. Code copying this class into
 * production should add a bound (counting semaphore, slot accounting, etc.).
 */
class FiberExecutor final : public Aws::Utils::Threading::Executor
{
protected:
    bool SubmitToThread(std::function<void()> && fn) override;

private:
    struct ExecParams
    {
        std::function<void()> fn;
    };

    static int execFiberMain(ExecParams * params) noexcept;
};

bool FiberExecutor::SubmitToThread(std::function<void()> && fn)
{
    return silk::FiberScheduler::run(execFiberMain, {std::move(fn)}, nullptr);
}

int FiberExecutor::execFiberMain(ExecParams * params) noexcept
{
    std::function<void()> fn = std::move(params->fn);
    try
    {
        fn();
    }
    catch (const std::exception & e)
    {
        // The SDK wraps tasks in std::packaged_task, which captures exceptions
        // into the promise before rethrowing. The session thread sees the error
        // via callable.get(). We catch here only to satisfy noexcept.
        SILK_ERROR("fiber executor task exception: {}", e.what());
    }
    return 0;
}

/**
 * S3Bench
 *
 * Sessions are OS threads (simulated CH queries). Each session maintains an
 * ioDepth-slot ring buffer of in-flight callables. In fiber mode the SDK
 * executor is FiberExecutor so each HTTP request runs in a fiber; in
 * thread mode it uses standard PooledThreadExecutor.
 */
class S3Bench
{
public:
    S3Bench(const ClientConfig & cfg, Aws::S3::S3Client * s3);

    void start();
    void stop();

    std::vector<uint64_t> collectLatencies();

    uint32_t failedSessions() const;

private:
    struct Slot
    {
        silk::FiberFuture future;
        std::optional<Aws::S3::S3Error> error;
        std::shared_ptr<std::stringstream> body;
        uint64_t startCycles = 0;
    };

    struct Session
    {
        std::thread thread;
        std::vector<uint64_t> latencies;
        bool failed = false;
    };

    void submit(Slot & slot, uint32_t & count);
    void runSession(Session * session);

    ClientConfig cfg;
    Aws::S3::S3Client * s3;
    std::vector<Session> sessions;
    std::atomic<uint64_t> warmupEndCycles{UINT64_MAX};
    std::atomic<bool> stopping{};
};

S3Bench::S3Bench(const ClientConfig & cfg, Aws::S3::S3Client * s3)
    : cfg(cfg)
    , s3(s3)
    , sessions(cfg.numJobs)
{
}

void S3Bench::start()
{
    for (Session & session : sessions)
    {
        session.thread = std::thread([this, &session] { runSession(&session); });
    }

    warmupEndCycles.store(silk::Tsc::getCycles() + silk::Tsc::nanosecondsToCycles(cfg.warmupNs), std::memory_order_relaxed);
}

void S3Bench::stop()
{
    stopping.store(true, std::memory_order_relaxed);

    for (Session & session : sessions)
    {
        session.thread.join();
    }
}

std::vector<uint64_t> S3Bench::collectLatencies()
{
    std::vector<uint64_t> all;
    for (Session & session : sessions)
    {
        all.insert(all.end(), session.latencies.begin(), session.latencies.end());
    }
    return all;
}

uint32_t S3Bench::failedSessions() const
{
    uint32_t count = 0;
    for (const Session & session : sessions)
    {
        if (session.failed)
        {
            count++;
        }
    }
    return count;
}

void S3Bench::submit(Slot & slot, uint32_t & count)
{
    bool doPut;
    switch (cfg.mode)
    {
        case MODE_READ:
            doPut = false;
            break;
        case MODE_WRITE:
            doPut = true;
            break;
        case MODE_READWRITE:
            doPut = (count % 2 == 0);
            break;
    }
    count++;

    slot.future.reset();
    slot.error.reset();
    slot.startCycles = silk::Tsc::getCycles();

    if (doPut)
    {
        slot.body->seekg(0);
        slot.body->clear();

        Aws::S3::Model::PutObjectRequest req;
        req.SetBucket(cfg.bucket);
        req.SetKey(cfg.key);
        req.SetBody(slot.body);
        req.SetContentLength(static_cast<long long>(cfg.objectSize));

        s3->PutObjectAsync(
            req,
            [&slot](const auto *, const auto &, const auto & outcome, const auto &)
            {
                if (!outcome.IsSuccess())
                {
                    slot.error = outcome.GetError();
                }
                slot.future.set(0);
            });
    }
    else
    {
        Aws::S3::Model::GetObjectRequest req;
        req.SetBucket(cfg.bucket);
        req.SetKey(cfg.key);

        s3->GetObjectAsync(
            req,
            [&slot](const auto *, const auto &, auto outcome, const auto &)
            {
                if (!outcome.IsSuccess())
                {
                    slot.error = outcome.GetError();
                }
                slot.future.set(0);
            });
    }
}

void S3Bench::runSession(Session * session)
{
    std::vector<Slot> slots(cfg.ioDepth);
    for (auto & slot : slots)
    {
        slot.body = std::make_shared<std::stringstream>(std::string(cfg.objectSize, 'x'));
    }

    uint32_t count = 0;
    for (auto & slot : slots)
    {
        submit(slot, count);
    }

    uint32_t head = 0;
    while (!stopping.load(std::memory_order_relaxed))
    {
        slots[head].future.wait();
        uint64_t end = silk::Tsc::getCycles();

        if (slots[head].error)
        {
            // Fail-fast: a single S3 error terminates the session. With a misconfigured
            // endpoint or a transient failure every session exits after one bad outcome,
            // and the run produces no useful latency data. failedSessions exposes the
            // count so the caller can warn.
            SILK_ERROR("S3 request failed: {}", slots[head].error->GetMessage());
            session->failed = true;
            head = (head + 1) % cfg.ioDepth;
            break;
        }

        if (slots[head].startCycles >= warmupEndCycles.load(std::memory_order_relaxed))
        {
            session->latencies.push_back(silk::Tsc::cyclesToNanoseconds(end - slots[head].startCycles));
        }

        submit(slots[head], count);
        head = (head + 1) % cfg.ioDepth;
    }

    // On normal stop: all ioDepth slots from head are in-flight.
    // On error: head was advanced past the consumed slot; ioDepth-1 remain.
    // silk::FiberFuture::wait() returns immediately if already set, so draining
    // all slots is always correct.
    for (uint32_t i = 0; i < cfg.ioDepth; i++)
    {
        slots[(head + i) % cfg.ioDepth].future.wait();
    }
}

/**
 * Main entry point.
 */
int main(int argc, char ** argv)
{
    ClientConfig cfg;
    std::string rwStr = "read";
    std::string durationStr = "10s";
    std::string warmupStr = "2s";
    bool verbose = false;

    namespace po = boost::program_options;
    po::options_description desc("s3-perf options");

    // clang-format off
    desc.add_options()
        ("help,h",       "show this help")
        ("endpoint",     po::value(&cfg.endpointUrl),       "S3 endpoint URL (e.g. http://127.0.0.1:9000)")
        ("region",       po::value(&cfg.region),            "AWS region")
        ("bucket",       po::value(&cfg.bucket),            "S3 bucket")
        ("key",          po::value(&cfg.key),               "object key")
        ("access-key",   po::value(&cfg.accessKeyId),       "AWS access key ID")
        ("secret-key",   po::value(&cfg.secretAccessKey),   "AWS secret access key")
        ("size",         po::value(&cfg.objectSize),        "object size in bytes (for write)")
        ("numjobs",      po::value(&cfg.numJobs),           "concurrent session threads")
        ("iodepth",      po::value(&cfg.ioDepth),           "parallel S3 requests per session")
        ("rw",           po::value(&rwStr),                 "I/O mode: read | write | readwrite")
        ("threads",      po::bool_switch(&cfg.useThreads),  "use SDK thread executor instead of FiberExecutor")
        ("duration",     po::value(&durationStr),            "measurement duration (e.g. 10s, 500ms)")
        ("warmup",       po::value(&warmupStr),             "warmup duration (e.g. 2s, 500ms)")
        ("print-counters", po::bool_switch(&cfg.printCounters), "enable per-CPU profiler and include counters in the JSON report")
        ("verbose,v",    po::bool_switch(&verbose),         "enable debug logging")
        ;
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << "usage: s3-perf [options]\n" << desc << "\n";
            return 0;
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
        return 1;
    }

    if (!parseMode(rwStr, &cfg.mode))
    {
        std::cerr << "error: unknown --rw mode: " << rwStr << " (expected read | write | readwrite)\n";
        return 1;
    }

    Aws::SDKOptions sdkOptions;
    sdkOptions.httpOptions.initAndCleanupCurl = false;

    std::shared_ptr<Aws::Utils::Threading::Executor> executor;

    silk::initialize();
    if (!cfg.useThreads)
    {
        silk::FiberScheduler::Options options{.enableProfiler = cfg.printCounters};
        silk::FiberScheduler::initialize(&options);
    }

    if (cfg.useThreads)
    {
        sdkOptions.httpOptions.httpClientFactory_create_fn = [] { return Aws::MakeShared<PocoHttpClientFactory>("s3-perf"); };
        executor = std::make_shared<Aws::Utils::Threading::PooledThreadExecutor>(static_cast<size_t>(cfg.numJobs) * cfg.ioDepth);
    }
    else
    {
        sdkOptions.httpOptions.httpClientFactory_create_fn = [] { return Aws::MakeShared<FiberHttpClientFactory>("s3-perf"); };
        executor = std::make_shared<FiberExecutor>();
    }

    Aws::InitAPI(sdkOptions);
    auto s3 = makeS3Client(cfg, std::move(executor));

    S3Bench bench(cfg, s3.get());

    SILK_INFO("starting benchmark");
    bench.start();

    sigset_t mask = blockSignals();

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

    SILK_INFO("stopping benchmark");
    bench.stop();

    uint32_t failedSessions = bench.failedSessions();
    if (failedSessions > 0)
    {
        SILK_WARN("{}/{} sessions exited early due to S3 errors", failedSessions, cfg.numJobs);
    }

    auto latencies = bench.collectLatencies();
    printJson(latencies, cfg, failedSessions);

    s3.reset();
    Aws::ShutdownAPI(sdkOptions);

    if (!cfg.useThreads)
    {
        silk::FiberScheduler::destroy();
    }
    silk::destroy();

    return 0;
}
