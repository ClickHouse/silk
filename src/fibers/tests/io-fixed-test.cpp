#include <silk/fibers/fiber.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace silk
{

// writeFixed then readFixed against a registered buffer. It must test
// round-trip all the three apis, including reads into a non-base offset
// within the registered region.
TEST(IoFixed, writeReadRoundTrip)
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

// TODO(kavi): this test runs a single fiber on one CPU. The whole point of
// registering buffers on every ring is that a fiber can move to another CPU and
// still use the same bufIndex. We don't test that here because we can't reliably
// force a fiber to move to a specific CPU, so the test would be flaky. If we need
// to be sure this works, add a second test that forces the move and checks the
// fixed IO still works.

} // namespace silk
