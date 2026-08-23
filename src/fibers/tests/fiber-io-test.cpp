#include <silk/fibers/fiber.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace silk
{

// Async IO from a non-fiber thread (proxy fiber): enqueueIo must call
// submitIo immediately since there is no runFiber to flush the SQE.
TEST(FiberIo, asyncIoFromThread)
{
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    const char msg[] = "proxy";
    iovec wiov{const_cast<char *>(msg), sizeof(msg)};
    uint64_t bytesWritten = 0;
    FiberScheduler::IoFuture wf;
    FiberScheduler::write(fds[1], &wiov, 1, 0, &bytesWritten, &wf);
    int r = wf.wait();
    EXPECT_EQ(r, 0);
    EXPECT_EQ(bytesWritten, sizeof(msg));

    // Deliberately uninitialized to verify MSan unpoisoning works correctly.
    char buf[sizeof(msg)];
    iovec riov{buf, sizeof(buf)};
    uint64_t bytesRead = 0;
    FiberScheduler::IoFuture rf;
    FiberScheduler::read(fds[0], &riov, 1, 0, &bytesRead, &rf);
    r = rf.wait();
    EXPECT_EQ(r, 0);
    EXPECT_EQ(bytesRead, sizeof(msg));
    EXPECT_STREQ(buf, msg);

    ::close(fds[0]);
    ::close(fds[1]);
}

// Basic blocking read/write through a pipe.
TEST(FiberIo, readWrite)
{
    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            const char message[] = "hello";
            uint64_t bytesWritten = 0;
            int w = FiberScheduler::write(p->writeFd, message, sizeof(message), 0, &bytesWritten);
            EXPECT_EQ(w, 0);
            EXPECT_EQ(bytesWritten, sizeof(message));

            char buf[sizeof(message)] = {};
            uint64_t bytesRead = 0;
            int r = FiberScheduler::read(p->readFd, buf, sizeof(buf), 0, &bytesRead);
            EXPECT_EQ(r, 0);
            EXPECT_EQ(bytesRead, sizeof(message));
            EXPECT_STREQ(buf, message);

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {fds[0], fds[1]});
    ASSERT_EQ(r, 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

// Async scatter/gather IO: submit writev and readv concurrently, wait on each future.
TEST(FiberIo, asyncReadWrite)
{
    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            const char part1[] = "foo";
            const char part2[] = "bar";

            iovec wiov[2] = {
                {const_cast<char *>(part1), sizeof(part1) - 1},
                {const_cast<char *>(part2), sizeof(part2) - 1},
            };

            uint64_t bytesWritten = 0;
            FiberScheduler::IoFuture wf;
            FiberScheduler::write(p->writeFd, wiov, 2, 0, &bytesWritten, &wf);

            char buf[6] = {};
            iovec riov[2] = {
                {buf, 3},
                {buf + 3, 3},
            };

            uint64_t bytesRead = 0;
            FiberScheduler::IoFuture rf;
            FiberScheduler::read(p->readFd, riov, 2, 0, &bytesRead, &rf);

            int w = wf.wait();
            EXPECT_EQ(w, 0);
            EXPECT_EQ(bytesWritten, 6u);

            int r = rf.wait();
            EXPECT_EQ(r, 0);
            EXPECT_EQ(bytesRead, 6u);
            EXPECT_EQ((std::string_view{buf, 6}), "foobar");

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {fds[0], fds[1]});
    ASSERT_EQ(r, 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

// poll: wait for readability on the read end before reading.
TEST(FiberIo, pollReadable)
{
    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            const char message[] = "poll";
            uint64_t bytesWritten = 0;
            int w = FiberScheduler::write(p->writeFd, message, sizeof(message), 0, &bytesWritten);
            EXPECT_EQ(w, 0);

            uint64_t triggeredEvents = 0;
            int ep = FiberScheduler::poll(p->readFd, POLLIN, &triggeredEvents);
            EXPECT_EQ(ep, 0);
            EXPECT_TRUE(triggeredEvents & POLLIN);

            char buf[sizeof(message)] = {};
            uint64_t bytesRead = 0;
            int r = FiberScheduler::read(p->readFd, buf, sizeof(buf), 0, &bytesRead);
            EXPECT_EQ(r, 0);
            EXPECT_STREQ(buf, message);

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {fds[0], fds[1]});
    ASSERT_EQ(r, 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

// cancel: cancel a pending read; future must complete with -ECANCELED (or
// with the read result if the kernel beat the cancellation).
TEST(FiberIo, cancelRead)
{
    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            char buf[16] = {};
            FiberScheduler::IoFuture rf;
            iovec riov{buf, sizeof(buf)};
            FiberScheduler::read(p->readFd, &riov, 1, 0, nullptr, &rf);

            rf.cancel();

            int r = rf.wait();
            EXPECT_TRUE(r == ECANCELED || r == 0);

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {fds[0], fds[1]});
    ASSERT_EQ(r, 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

// connect + accept: client connect and listener accepts on loopback.
TEST(FiberIo, connectAccept)
{
    int listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(listenFd, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int r = ::bind(listenFd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    ASSERT_EQ(r, 0);
    r = ::listen(listenFd, 1);
    ASSERT_EQ(r, 0);

    socklen_t len = sizeof(addr);
    r = ::getsockname(listenFd, reinterpret_cast<sockaddr *>(&addr), &len);
    ASSERT_EQ(r, 0);

    int clientFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(clientFd, 0);

    struct Params
    {
        int listenFd;
        int clientFd;
        sockaddr_in addr;

        static int fiberMain(Params * p) noexcept
        {
            FiberScheduler::IoFuture connectFuture;
            FiberScheduler::connect(p->clientFd, reinterpret_cast<const sockaddr *>(&p->addr), sizeof(p->addr), &connectFuture);

            uint64_t acceptedFd = 0;
            int a = FiberScheduler::accept(p->listenFd, nullptr, nullptr, SOCK_CLOEXEC, &acceptedFd);
            EXPECT_EQ(a, 0);
            EXPECT_GE(static_cast<int>(acceptedFd), 0);

            EXPECT_EQ(connectFuture.wait(), 0);

            ::close(static_cast<int>(acceptedFd));

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {listenFd, clientFd, addr});
    ASSERT_EQ(r, 0);

    ::close(clientFd);
    ::close(listenFd);
}

// Stress: many fibers each doing a write+read through their own pipe.
TEST(FiberIo, concurrentReadWrite)
{
    static constexpr int N = 100;

    struct Params
    {
        int readFd;
        int writeFd;
        int index;

        static int fiberMain(Params * p) noexcept
        {
            uint64_t bytesWritten = 0;
            int w = FiberScheduler::write(p->writeFd, &p->index, sizeof(p->index), 0, &bytesWritten);
            EXPECT_EQ(w, 0);

            int val = 0;
            uint64_t bytesRead = 0;
            int r = FiberScheduler::read(p->readFd, &val, sizeof(val), 0, &bytesRead);
            EXPECT_EQ(r, 0);
            EXPECT_EQ(val, p->index);

            return val;
        }
    };

    int fds[N][2];
    FiberFuture futures[N];

    for (int i = 0; i < N; ++i)
    {
        int r = ::pipe(fds[i]);
        ASSERT_EQ(r, 0);

        r = FiberScheduler::run(Params::fiberMain, {fds[i][0], fds[i][1], i}, &futures[i]);
        ASSERT_FALSE(r);
    }

    for (int i = 0; i < N; ++i)
    {
        int r = futures[i].wait();
        ASSERT_EQ(r, i);

        ::close(fds[i][0]);
        ::close(fds[i][1]);
    }
}

// Cross-fiber cancel: one fiber submits a poll and blocks; a second fiber
// (which the scheduler may run on a different CPU via work-stealing) cancels
// it. Without the logic in cancelIo() that routes the cancel SQE to the same
// ring as the original POLL_ADD, io_uring returns -ENOENT on the cancel and
// the poller's wait() hangs forever.
//
// We repeat N times to increase the probability that work-stealing migrates
// the canceller to a different CPU than the poller on at least some iterations.
TEST(FiberIo, cancelPollFromAnotherFiber)
{
    static constexpr int N = 200;

    struct Ctx
    {
        int readFd;
        FiberFuture pollRegistered; // poller -> canceller: poll is in the ring
        FiberScheduler::IoFuture pollFuture;
    };

    struct Params
    {
        Ctx * ctx;

        static int pollerMain(Params * p) noexcept
        {
            auto * ctx = p->ctx;

            // Register an async poll (does not block yet).
            FiberScheduler::poll(ctx->readFd, POLLIN, nullptr, &ctx->pollFuture);
            // Signal the canceller that the SQE is now committed to a ring.
            ctx->pollRegistered.set(0);
            // Block until the cancel (or a spurious write) resolves us.
            return ctx->pollFuture.wait();
        }

        static int cancellerMain(Params * p) noexcept
        {
            auto * ctx = p->ctx;

            // Don't cancel until the poll SQE is definitely in a ring;
            // otherwise the cancel might race and fail with -EALREADY.
            ctx->pollRegistered.wait();
            ctx->pollFuture.cancel();
            return 0;
        }
    };

    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    for (int i = 0; i < N; ++i)
    {
        Ctx ctx;
        ctx.readFd = fds[0];

        FiberFuture f1, f2;
        int r = FiberScheduler::run(Params::pollerMain, {&ctx}, &f1);
        ASSERT_FALSE(r);
        r = FiberScheduler::run(Params::cancellerMain, {&ctx}, &f2);
        ASSERT_FALSE(r);

        r = FiberFuture::waitWithTimeout(&f1, 1'000'000'000);
        if (r)
        {
            ASSERT_EQ(r, ECANCELED);
        }
        r = f2.wait();
        ASSERT_FALSE(r);
    }

    ::close(fds[0]);
    ::close(fds[1]);
}

// Regression test: cancelIo must explicitly set CQE_TAG_CANCEL on the cancel
// SQE. io_uring_initialize_sqe does not clear user_data; SQ ring slots rotate
// after enough submissions, so a cancel SQE that omits set_data inherits a
// stale IoFuture* from a previously-completed op. handleCompletionQueue would
// then dispatch the cancel CQE as a real IO completion -- writing through
// future->result and signalling a future that has already returned to the
// caller, possibly overwriting unrelated stack memory.
//
// The pattern: drain >128 IOs to rotate slots, then cancel an IO and verify
// previously-completed futures are not re-touched. Without the fix, cancel's
// success CQE (res=0) writes 0 through a stale result pointer, clobbering the
// sentinel below.
TEST(FiberIo, cancelDoesNotResignalCompletedFutures)
{
    int fds[2];
    int r = ::pipe(fds);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            // Submit and complete enough polls to wrap the 128-entry SQ ring
            // at least once, leaving stale IoFuture* leftovers in user_data.
            static constexpr uint32_t COUNT = 256;
            FiberScheduler::IoFuture futures[COUNT];
            uint64_t triggered[COUNT] = {};

            for (uint32_t i = 0; i < COUNT; ++i)
            {
                FiberScheduler::poll(p->readFd, POLLIN, &triggered[i], &futures[i]);
            }

            char byte = 1;
            ssize_t written = ::write(p->writeFd, &byte, 1);
            EXPECT_EQ(written, 1);

            for (uint32_t i = 0; i < COUNT; ++i)
            {
                futures[i].wait();
            }

            // Drain the byte so the next poll blocks.
            char drainBuf;
            ssize_t bytesRead = ::read(p->readFd, &drainBuf, 1);
            EXPECT_EQ(bytesRead, 1);

            // Reset every result-pointer slot to a recognizable sentinel.
            // If a stale-user_data cancel CQE is dispatched as a real IO
            // completion, *future->result is overwritten with cqe->res (0 on
            // cancel-success), erasing the sentinel.
            static constexpr uint64_t SENTINEL = 0xCAFEBABE;
            for (uint32_t i = 0; i < COUNT; ++i)
            {
                triggered[i] = SENTINEL;
            }

            // Submit a fresh poll into a slot whose user_data is now a stale
            // pointer to one of the completed futures above; cancel it.
            uint64_t cancelTriggered = 0;
            FiberScheduler::IoFuture cancelFuture;
            FiberScheduler::poll(p->readFd, POLLIN, &cancelTriggered, &cancelFuture);
            cancelFuture.cancel();
            int cancelResult = cancelFuture.wait();
            EXPECT_TRUE(cancelResult == ECANCELED || cancelResult == 0);

            // Sentinels must be intact: no stale-user_data CQE was dispatched
            // as a real IO completion.
            for (uint32_t i = 0; i < COUNT; ++i)
            {
                EXPECT_EQ(triggered[i], SENTINEL) << "future " << i << " was re-touched";
            }

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, {fds[0], fds[1]});
    ASSERT_EQ(r, 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

// A single fiber posting more async polls than the SQE ring capacity (128)
// without waiting exposes exhaustion: with SQPOLL the kernel thread is pinned
// to the same CPU and cannot consume SQEs while the fiber runs.
TEST(FiberIo, sqeRingExhaustion)
{
    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * params) noexcept
        {
            static constexpr uint32_t COUNT = 256;
            FiberScheduler::IoFuture futures[COUNT];

            for (uint32_t i = 0; i < COUNT; ++i)
            {
                FiberScheduler::poll(params->readFd, POLLIN, nullptr, &futures[i]);
            }

            char byte = 1;
            ssize_t r = ::write(params->writeFd, &byte, 1);
            EXPECT_EQ(r, 1);

            for (uint32_t i = 0; i < COUNT; ++i)
            {
                futures[i].wait();
            }

            return 0;
        }
    };

    int pipeFds[2];
    ASSERT_EQ(::pipe(pipeFds), 0);

    FiberScheduler::run(Params::fiberMain, Params{pipeFds[0], pipeFds[1]});

    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
}

// Repeatedly read then immediately cancel.
// Make sure no data is lost.
TEST(FiberIo, cancelMustNotDropDeliveredBytes)
{
    static constexpr uint64_t TOTAL = 4096;

    struct Params
    {
        int readFd;
        int writeFd;

        static int fiberMain(Params * p) noexcept
        {
            std::string expected(TOTAL, '\0');
            for (uint64_t i = 0; i < TOTAL; ++i)
            {
                expected[i] = static_cast<char>(i & 0xFF);
            }

            EXPECT_EQ(::write(p->writeFd, expected.data(), TOTAL), static_cast<ssize_t>(TOTAL));
            ::close(p->writeFd);

            std::string got;
            for (;;)
            {
                char buf[64] = {};
                uint64_t bytes_read = 0;
                FiberScheduler::IoFuture future;
                iovec iov{buf, sizeof(buf)};
                FiberScheduler::read(p->readFd, &iov, 1, 0, &bytes_read, &future);
                future.cancel();
                if (future.wait() == 0)
                {
                    // Read won (likely).
                    if (bytes_read == 0)
                    {
                        break;
                    }
                    got.append(buf, bytes_read);
                }
                else
                {
                    // Cancel won.
                    // It's unlikely to happen consistently,
                    // but to keep the test independent of kernel internals, read to make progress.
                    if (!FiberScheduler::read(p->readFd, buf, sizeof(buf), 0, &bytes_read))
                    {
                        if (bytes_read == 0)
                        {
                            break;
                        }
                        got.append(buf, bytes_read);
                    }
                }
            }

            EXPECT_EQ(got, expected);
            return 0;
        }
    };

    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    EXPECT_EQ(FiberScheduler::run(Params::fiberMain, Params{fds[0], fds[1]}), 0);

    ::close(fds[0]);
}

// writeFixed then readFixed against a registered buffer. It must test
// round-trip all the three apis, including reads into a non-base offset
// within the registered region.
//
// TODO(kavi): this test runs a single fiber on one CPU. The whole point of
// registering buffers on every ring is that a fiber can move to another CPU and
// still use the same bufIndex. We don't test that here because we can't reliably
// force a fiber to move to a specific CPU, so the test would be flaky. If we need
// to be sure this works, add a second test that forces the move and checks the
// fixed IO still works.
TEST(FiberIo, fixedWriteReadRoundTrip)
{
    static constexpr uint64_t BLOCK = 4096;
    static constexpr uint64_t NBLOCKS = 2;
    static constexpr uint64_t SIZE = BLOCK * NBLOCKS;

    char tmpl[] = "/tmp/silk-io-fixed-XXXXXX";
    int fd = ::mkstemp(tmpl);
    ASSERT_GE(fd, 0) << std::strerror(errno);
    ::unlink(tmpl);
    ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(SIZE)), 0) << std::strerror(errno);

    // Single contiguous registration covering the whole buffer; bufIndex 0.
    char * buf = static_cast<char *>(std::malloc(SIZE));
    ASSERT_NE(buf, nullptr);
    iovec reg{buf, SIZE};
    FiberScheduler::registerBuffers(&reg, 1);

    struct Params
    {
        int fd;
        char * buf;

        static int fiberMain(Params * p) noexcept
        {
            // Fill block 0 with a known pattern and write it out via WRITE_FIXED.
            for (uint64_t i = 0; i < BLOCK; ++i)
            {
                p->buf[i] = static_cast<char>((i * 7 + 1) & 0xFF);
            }

            uint64_t bytesWritten = 0;
            FiberScheduler::IoFuture wf;
            FiberScheduler::writeFixed(p->fd, p->buf, BLOCK, 0, 0, &bytesWritten, &wf);
            EXPECT_EQ(wf.wait(), 0);
            EXPECT_EQ(bytesWritten, BLOCK);

            // Read back into the SECOND block: a non-base offset still inside the
            // registered region (exercises the "buf within registered buffer"
            // contract) that is deliberately left untouched by userspace. Under
            // MSan it is poisoned, so the only thing that can mark it initialized
            // is the kernel fill + readFixed's MSAN_UNPOISON. If readFixed forgot
            // to unpoison, the comparison below endup as a use-of-uninitialized.
            char * dst = p->buf + BLOCK;
            uint64_t bytesRead = 0;
            FiberScheduler::IoFuture rf;
            FiberScheduler::readFixed(p->fd, dst, BLOCK, 0, 0, &bytesRead, &rf);
            EXPECT_EQ(rf.wait(), 0);
            EXPECT_EQ(bytesRead, BLOCK);

            // The kernel-filled bytes must match what we wrote (and must be
            // readable without tripping MSan).
            for (uint64_t i = 0; i < BLOCK; ++i)
            {
                EXPECT_EQ(dst[i], static_cast<char>((i * 7 + 1) & 0xFF)) << "mismatch at byte " << i;
            }

            return 0;
        }
    };

    EXPECT_EQ(FiberScheduler::run(Params::fiberMain, Params{fd, buf}), 0);

    std::free(buf);
    ::close(fd);
}

// splice: relay a stream from one socket to another through a pipe, the pattern
// a proxy uses to forward traffic without copying it through user space. The
// first leg goes through the blocking overload and the second through the async
// one; a last splice on a source whose peer is gone reports end of input as
// zero bytes moved.
TEST(FiberIo, spliceThroughPipe)
{
    static constexpr char MESSAGE[] = "splice";

    int source[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, source), 0);

    int destination[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, destination), 0);

    int pipeFds[2];
    ASSERT_EQ(::pipe(pipeFds), 0);

    ssize_t written = ::write(source[1], MESSAGE, sizeof(MESSAGE));
    ASSERT_EQ(written, static_cast<ssize_t>(sizeof(MESSAGE)));

    // Shut the write side so the source reports end of input once the payload is consumed.
    int r = ::shutdown(source[1], SHUT_WR);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int sourceFd;
        int pipeReadFd;
        int pipeWriteFd;
        int destinationFd;

        static int fiberMain(Params * params) noexcept
        {
            uint64_t bytesIntoPipe = 0;
            int r = FiberScheduler::splice(params->sourceFd, -1, params->pipeWriteFd, -1, sizeof(MESSAGE), SPLICE_F_MOVE, &bytesIntoPipe);
            EXPECT_EQ(r, 0);
            EXPECT_EQ(bytesIntoPipe, sizeof(MESSAGE));

            uint64_t bytesOutOfPipe = 0;
            FiberScheduler::IoFuture future;
            FiberScheduler::splice(
                params->pipeReadFd, -1, params->destinationFd, -1, bytesIntoPipe, SPLICE_F_MOVE, &bytesOutOfPipe, &future);
            EXPECT_EQ(future.wait(), 0);
            EXPECT_EQ(bytesOutOfPipe, sizeof(MESSAGE));

            uint64_t bytesAfterEndOfInput = 0;
            r = FiberScheduler::splice(
                params->sourceFd, -1, params->pipeWriteFd, -1, sizeof(MESSAGE), SPLICE_F_MOVE, &bytesAfterEndOfInput);
            EXPECT_EQ(r, 0);
            EXPECT_EQ(bytesAfterEndOfInput, 0u);

            return 0;
        }
    };

    r = FiberScheduler::run(Params::fiberMain, Params{source[0], pipeFds[0], pipeFds[1], destination[1]});
    ASSERT_EQ(r, 0);

    char buf[sizeof(MESSAGE)] = {};
    ssize_t bytesRead = ::read(destination[0], buf, sizeof(buf));
    ASSERT_EQ(bytesRead, static_cast<ssize_t>(sizeof(MESSAGE)));
    ASSERT_STREQ(buf, MESSAGE);

    ::close(source[0]);
    ::close(source[1]);
    ::close(destination[0]);
    ::close(destination[1]);
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
}

TEST(FiberIo, spliceLargeLength)
{
    static constexpr char MESSAGE[] = "splice";

    int source[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, source);
    ASSERT_EQ(r, 0);

    int pipeFds[2];
    r = ::pipe(pipeFds);
    ASSERT_EQ(r, 0);

    ssize_t written = ::write(source[1], MESSAGE, sizeof(MESSAGE));
    ASSERT_EQ(written, static_cast<ssize_t>(sizeof(MESSAGE)));

    r = ::shutdown(source[1], SHUT_WR);
    ASSERT_EQ(r, 0);

    struct Params
    {
        int sourceFd;
        int pipeWriteFd;
        uint64_t * bytesSpliced;

        static int fiberMain(Params * params) noexcept
        {
            return FiberScheduler::splice(
                params->sourceFd, -1, params->pipeWriteFd, -1, uint64_t{1} << 32, SPLICE_F_MOVE, params->bytesSpliced);
        }
    };

    uint64_t bytesSpliced = 0;
    r = FiberScheduler::run(Params::fiberMain, Params{source[0], pipeFds[1], &bytesSpliced});
    ASSERT_EQ(r, 0);
    ASSERT_EQ(bytesSpliced, sizeof(MESSAGE));

    ::close(source[0]);
    ::close(source[1]);
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
}

} // namespace silk
