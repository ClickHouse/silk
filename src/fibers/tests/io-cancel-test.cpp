#include <silk/fibers/fiber.h>

#include <gtest/gtest.h>

#include <string>

#include <unistd.h>

namespace silk
{

// Repeatedly read then immediately cancel.
// Make sure no data is lost.
TEST(IoCancel, cancelMustNotDropDeliveredBytes)
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

} // namespace silk
