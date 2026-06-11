#include <silk/util/assert.h>

#include <benchmark/benchmark.h>
#include <boost/context/detail/fcontext.hpp>

#include <sys/mman.h>

namespace silk
{

class BoostContextBench : public benchmark::Fixture
{
};

// Raw boost::context round-trip: one extra stack that immediately jumps back
// to wherever it was called from.  Each iteration = 2 context switches
// (main -> bounce, bounce -> main) with zero scheduler overhead.
BENCHMARK_F(BoostContextBench, ContextSwitch)(benchmark::State & state)
{
    using namespace boost::context::detail;

    static constexpr uint64_t STACK_SIZE = 65536;

    void * stack = ::mmap(nullptr, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    SILK_ASSERT(stack != MAP_FAILED);

    // Bounce context: just jump back to the caller on every activation.
    fcontext_t ctx = make_fcontext(
        static_cast<char *>(stack) + STACK_SIZE,
        STACK_SIZE,
        [](transfer_t t) noexcept
        {
            for (;;)
            {
                t = jump_fcontext(t.fctx, nullptr);
            }
        });

    for (auto _ : state)
    {
        auto t = jump_fcontext(ctx, nullptr);
        ctx = t.fctx;
    }

    ::munmap(stack, STACK_SIZE);
}

} // namespace silk
