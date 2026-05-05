#include <silk/util/assert.h>

// Suppress warnings emitted by librseq headers: volatile assignment in rseq_cs
// and unused parameters in the asm stubs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-volatile"
#pragma clang diagnostic ignored "-Wunused-parameter"
#include <rseq/rseq.h>
#pragma clang diagnostic pop

namespace silk
{

void initRseq() noexcept
{
    int ret = rseq_init();
    ASSERT(ret == RSEQ_INIT_OK);
}

} // namespace silk
