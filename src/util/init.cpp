#include <silk/util/init.h>

#include <silk/util/assert.h>
#include <silk/util/perf.h>
#include <silk/util/queue.h>
#include <silk/util/tsc.h>

// Suppress warnings emitted by librseq headers: volatile assignment in rseq_cs
// and unused parameters in the asm stubs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-volatile"
#pragma clang diagnostic ignored "-Wunused-parameter"
#include <rseq/rseq.h>
#pragma clang diagnostic pop

namespace silk
{

void initialize() noexcept
{
    int r = rseq_init();
    SILK_ASSERT(r == RSEQ_INIT_OK);

    Tsc::initialize();
    Perf::initialize();
    QueueBase::initialize();
}

void destroy() noexcept
{
    QueueBase::destroy();
    Perf::destroy();
}

} // namespace silk
