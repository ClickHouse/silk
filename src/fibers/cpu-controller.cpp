#include "cpu-controller.h"

namespace silk
{

CpuController::Decision CpuController::evaluateWindow(
    CpuController::Window * state,
    uint16_t prefixIndex,
    uint16_t width,
    uint16_t widthTotal,
    uint64_t elapsedCycles,
    uint64_t nowCycles) noexcept
{
    Decision decision;

    // An overrun window means the loop was parked or away, not attending - the counts
    // are not a valid sample; a freshly started member would otherwise judge itself on
    // its pre-park life and shrink right back.
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

        if (approveGrow(width, widthTotal, nowCycles))
        {
            decision.action = Action::GROW;
        }
    }
    // A wait rewarded by arriving work is demand; an expired spin or an empty park
    // expiry is waste.
    else if (state->wasteCount > state->rewardCount * SHRINK_WASTE_FACTOR)
    {
        evaluateShrink(state, prefixIndex, width, nowCycles, &decision);
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

void CpuController::evaluateShrink(
    CpuController::Window * state, uint16_t prefixIndex, uint16_t width, uint64_t nowCycles, Decision * decision) noexcept
{
    // Recent growth vetoes the shrink - a freshly started member reads wasteful until the
    // steal traffic re-homes its share. Only the rightmost member shrinks, and never
    // processor zero.
    uint64_t growCycles = lastGrowCycles.load(std::memory_order_relaxed);
    bool quiet = nowCycles - growCycles >= SHRINK_HOLDOFF_WINDOWS * windowCycles;

    if (!quiet || prefixIndex == 0 || prefixIndex + 1 != width)
    {
        state->lowWindowCount = 0;
        return;
    }

    // One wasteful window is variance - only a sustained run of shrink-eligible windows
    // shrinks. A pure-idle window - not one dispatch all window - shrinks without the
    // streak: there is no load to misread, only decay to finish.
    state->lowWindowCount++;

    uint32_t shrinkWindows = state->peakDispatched ? SHRINK_WINDOW_COUNT : 1;

    if (state->lowWindowCount < shrinkWindows)
    {
        return;
    }

    decision->action = Action::SHRINK;
    decision->width = prefixIndex;
}

} // namespace silk
