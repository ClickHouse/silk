#include "signals.h"

#include <silk/util/assert.h>
#include <silk/util/platform.h>

#include <cerrno>

#include <pthread.h>
#include <signal.h>

sigset_t blockSignals() noexcept
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    return mask;
}

bool sigwaitFor(const sigset_t & mask, uint64_t ns) noexcept
{
    uint64_t endNs = silk::getTimeNanoseconds() + ns;

    for (;;)
    {
        uint64_t nowNs = silk::getTimeNanoseconds();
        if (nowNs >= endNs)
        {
            return false;
        }

        uint64_t remainingNs = endNs - nowNs;
        struct timespec timeout = {
            .tv_sec = static_cast<time_t>(remainingNs / 1'000'000'000ULL),
            .tv_nsec = static_cast<long>(remainingNs % 1'000'000'000ULL),
        };

        int r = sigtimedwait(&mask, nullptr, &timeout);
        if (r > 0)
        {
            return true;
        }

        r = errno;
        if (r == EAGAIN)
        {
            return false;
        }
        SILK_ASSERT(r == EINTR);
    }
}
