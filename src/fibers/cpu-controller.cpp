#include "cpu-controller.h"

#include <mutex>

namespace silk
{

CpuController::Decision CpuController::evaluateWindow(
    CpuController::Window * state,
    uint16_t prefixIndex,
    uint16_t width,
    uint16_t widthTotal,
    uint64_t enqueuedTotal,
    uint64_t elapsedCycles,
    uint64_t nowCycles) noexcept
{
    // A due probe resolves at any member's window boundary, not only inside a vote - a
    // probe that outlives its load must not dangle and freeze growth behind the gate.
    Decision decision = resolveProbe(enqueuedTotal, nowCycles);

    // An overrun window means the loop was parked or away, not attending - the counts
    // are not a valid sample; a freshly started member would otherwise judge itself on
    // its pre-park life and shed right back.
    if (elapsedCycles >= 2 * windowCycles)
    {
        state->lowWindowCount = 0;
    }
    // A backlogged window is a width shortage the backlog stamp cannot see - a closed
    // loop of dependent wakes touches empty between bursts, so its queueing shows as
    // wait time, never as backlog age.
    else if (state->backlogCount >= GROW_BACKLOG_COUNT)
    {
        state->lowWindowCount = 0;

        if (decision.action == Action::NONE && approveGrow(width, widthTotal, nowCycles))
        {
            decision.action = Action::GROW;
        }
    }
    // A wait rewarded by arriving work is demand; an expired spin or an empty park
    // expiry is waste.
    else if (state->wasteCount > state->rewardCount * SHRINK_WASTE_FACTOR)
    {
        if (decision.action == Action::NONE)
        {
            evaluateShed(state, prefixIndex, width, nowCycles, &decision);
        }
    }
    else
    {
        state->lowWindowCount = 0;
    }

    state->wasteCount = 0;
    state->rewardCount = 0;
    state->backlogCount = 0;
    state->peakDispatched = 0;
    return decision;
}

CpuController::Decision CpuController::resolveProbe(uint64_t enqueuedTotal, uint64_t nowCycles) noexcept
{
    Decision decision;

    if (startedNumber.load(std::memory_order_relaxed) == kInvalidProcessorNumber)
    {
        return decision;
    }

    // One evaluator resolves a probe: the ledger is re-checked under the lock, so a
    // racing boundary neither double-suppresses nor reverts twice.
    std::lock_guard guard(probeLock);

    if (startedNumber.load(std::memory_order_relaxed) == kInvalidProcessorNumber)
    {
        return decision;
    }

    uint64_t probeCyclesValue = probeCycles.load(std::memory_order_relaxed);

    if (nowCycles - probeCyclesValue < GROW_VERDICT_WINDOWS * windowCycles)
    {
        return decision;
    }

    // The verdict is the fleet's wake rate: a gain over the pre-grow rate halves the
    // suppression, anything less is a failed spread and doubles it. The started member's
    // own shed already resolved fast failures.
    uint64_t rate = ((enqueuedTotal - probeEnqueued.load(std::memory_order_relaxed)) << RATE_SHIFT) / (nowCycles - probeCyclesValue);
    uint64_t preRate = probeRate.load(std::memory_order_relaxed);

    startedNumber.store(kInvalidProcessorNumber, std::memory_order_relaxed);
    probeCycles.store(nowCycles, std::memory_order_relaxed);
    probeEnqueued.store(enqueuedTotal, std::memory_order_relaxed);

    uint16_t width = probeWidth.load(std::memory_order_relaxed);

    if (rate >= preRate + preRate / (GROW_VERDICT_MARGIN * width))
    {
        // Halve rather than reset: one lucky pass cannot undo an escalation, while a
        // real climb's consecutive passes still reach the floor fast.
        uint32_t windows = suppressWindows.load(std::memory_order_relaxed);
        suppressWindows.store(std::max<uint32_t>(windows / 2, GROW_SUPPRESS_MIN_WINDOWS), std::memory_order_relaxed);
        return decision;
    }

    // The spread bought nothing - revert the probe's width and back off. The revert is
    // best-effort: a width moved meanwhile by a concurrent grow stays, only the
    // suppression holds.
    suppressGrow(nowCycles);

    decision.action = Action::SHED;
    decision.fromWidth = width + 1;
    decision.toWidth = width;
    return decision;
}

void CpuController::evaluateShed(
    CpuController::Window * state, uint16_t prefixIndex, uint16_t width, uint64_t nowCycles, Decision * decision) noexcept
{
    // Recent growth vetoes the shed - a freshly started member reads wasteful until the
    // steal traffic re-homes its share. Only the rightmost member sheds, and never
    // processor zero.
    uint64_t growCycles = lastGrowCycles.load(std::memory_order_relaxed);
    bool quiet = nowCycles - growCycles >= SHRINK_HOLDOFF_WINDOWS * windowCycles;

    if (!quiet || prefixIndex == 0 || prefixIndex + 1 != width)
    {
        state->lowWindowCount = 0;
        return;
    }

    // One wasteful window is variance - only a sustained run of shed-eligible windows
    // sheds. A pure-idle window - not one dispatch all window - sheds without the
    // streak: there is no load to misread, only decay to finish.
    state->lowWindowCount++;

    uint32_t shrinkWindows = state->peakDispatched ? SHRINK_WINDOW_COUNT : 1;

    if (state->lowWindowCount < shrinkWindows)
    {
        return;
    }

    decision->action = Action::SHED;
    decision->fromWidth = width;
    decision->toWidth = prefixIndex;
}

void CpuController::commitGrow(uint16_t startedNumber_, uint16_t width, uint64_t enqueuedTotal, uint64_t nowCycles) noexcept
{
    std::lock_guard guard(probeLock);

    // The ledger is a single slot: a racing second grow - two doors approved on
    // different width reads - rides the pending probe's verdict instead of
    // clobbering it.
    if (startedNumber.load(std::memory_order_relaxed) != kInvalidProcessorNumber)
    {
        return;
    }

    // The pre-grow rate spans from the last snapshot to now.
    uint64_t snapCycles = probeCycles.load(std::memory_order_relaxed);

    if (nowCycles > snapCycles)
    {
        uint64_t preRate = ((enqueuedTotal - probeEnqueued.load(std::memory_order_relaxed)) << RATE_SHIFT) / (nowCycles - snapCycles);
        probeRate.store(preRate, std::memory_order_relaxed);
    }

    probeCycles.store(nowCycles, std::memory_order_relaxed);
    probeEnqueued.store(enqueuedTotal, std::memory_order_relaxed);
    probeWidth.store(width, std::memory_order_relaxed);
    startedNumber.store(startedNumber_, std::memory_order_relaxed);
}

/** Record a committed shed: reset the streak; the probe's own start shedding is a failed verdict. */
void CpuController::commitShed(Window * state, uint16_t number, uint64_t nowCycles) noexcept
{
    state->lowWindowCount = 0;

    // The shedding member being the probe's own start resolves it as failed - double
    // the suppression; ambient sheds are not verdicts.
    if (number == startedNumber.load(std::memory_order_relaxed))
    {
        std::lock_guard guard(probeLock);

        if (number == startedNumber.load(std::memory_order_relaxed))
        {
            startedNumber.store(kInvalidProcessorNumber, std::memory_order_relaxed);
            suppressGrow(nowCycles);
        }
    }
}

} // namespace silk
