#pragma once

#include <cerrno>

static inline bool isExpectedShutdown(int r)
{
    return r == ECONNRESET || r == ECANCELED || r == EBADF || r == EPIPE || r == EINVAL;
}
