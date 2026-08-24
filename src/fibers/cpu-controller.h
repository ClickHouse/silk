#pragma once

#include <silk/util/platform.h>

#include <atomic>
#include <cstdint>

namespace silk
{

/**
 * Width policy for the scheduler's active CPU set: every grow and shrink decision routes
 * through one gate. Part of the scheduler state; each processor embeds a Window, and
 * the scheduler executes the returned decisions.
 */
class CpuController
{
public:
    /** Per-processor controller state - embedded in the scheduler's per-processor state, owner-written. */
    struct Window
    {
        /** Waits rewarded by arriving work since the window start - demand. */
        uint64_t rewardCount = 0;

        /** Expired-empty waits since the window start - waste. */
        uint64_t wasteCount = 0;

        /** Excess dispatches - fibers that ran behind two others - accumulated over the window. */
        uint64_t backlogCount = 0;

        /** Deepest single dispatch pass of the window. */
        uint32_t peakDispatched = 0;

        /** Consecutive shrink-eligible windows; a shrink needs a sustained run. */
        uint32_t lowWindowCount = 0;

        /** Account a wait outcome: rewarded by arriving work is demand, expired empty is waste. */
        void countWait(bool rewarded) noexcept
        {
            if (rewarded)
            {
                ++rewardCount;
            }
            else
            {
                ++wasteCount;
            }
        }

        /** Account a dispatch pass of the given depth to the window. */
        void countDispatched(uint32_t dispatched) noexcept
        {
            // Dispatches past the second ran fibers that waited behind two others -
            // parallel slack width can serve; a sequential chain runs one or two deep
            // and never accumulates.
            if (dispatched > 2)
            {
                backlogCount += dispatched - 2;
            }

            if (dispatched > peakDispatched)
            {
                peakDispatched = dispatched;
            }
        }
    };

    /** What the caller executes: GROW starts the next prefix processor, SHRINK moves the width down. */
    enum class Action : uint8_t
    {
        NONE,
        GROW,
        SHRINK,
    };

    /** One window verdict; a SHRINK moves the width down to width by CAS. */
    struct Decision
    {
        /** The move the caller executes. */
        Action action = Action::NONE;

        /** The width the SHRINK drops to. */
        uint16_t width = 0;
    };

    /** Set the window length; called once at scheduler initialization. */
    void initialize(uint64_t windowCycles_) noexcept { windowCycles = windowCycles_; }

    /**
     * Evaluate a member's completed window: a backlogged window votes to grow, a
     * sustained wasteful run on the rightmost member shrinks. elapsedCycles is the
     * window's age. Resets the window counters; the caller restamps its window start.
     */
    Decision evaluateWindow(
        Window * state, uint16_t prefixIndex, uint16_t width, uint16_t widthTotal, uint64_t elapsedCycles, uint64_t nowCycles) noexcept;

    /** Arm-or-age a ready queue's backlog stamp; true when the backlog aged a full window. */
    bool observeBacklog(std::atomic<uint64_t> * backlogSinceCycles, uint64_t nowCycles) noexcept
    {
        // The first observation arms the stamp; only backlog older than a full window is
        // reported aged.
        uint64_t sinceCycles = backlogSinceCycles->load(std::memory_order_relaxed);

        if (sinceCycles == 0)
        {
            backlogSinceCycles->store(nowCycles, std::memory_order_relaxed);
            return false;
        }

        return nowCycles - sinceCycles >= windowCycles;
    }

    /** The advisory grow gate: full width and the growth pace refuse; claimGrow enforces. */
    bool approveGrow(uint16_t width, uint16_t widthTotal, uint64_t nowCycles) const noexcept
    {
        // One growth per window fleet-wide - steal traffic needs a window to re-home
        // the last member's share before the next one can prove demand.
        if (nowCycles - lastGrowCycles.load(std::memory_order_relaxed) < windowCycles)
        {
            return false;
        }

        return width != widthTotal;
    }

    /** Claim the one growth per window - the pace ticket; a fresh growth or a lost race refuses. */
    bool claimGrow(uint64_t nowCycles) noexcept
    {
        uint64_t growCycles = lastGrowCycles.load(std::memory_order_relaxed);

        if (nowCycles - growCycles < windowCycles)
        {
            return false;
        }

        return lastGrowCycles.compare_exchange_weak(growCycles, nowCycles, std::memory_order_relaxed);
    }

    /** Record a committed shrink - resets the shrink streak. */
    void commitShrink(Window * state) noexcept { state->lowWindowCount = 0; }

private:
    //
    // Constants.
    //

    /** Consecutive shrink-eligible windows required before the shrink fires. */
    static constexpr uint32_t SHRINK_WINDOW_COUNT = 3;

    /** Windows the shrink waits out past the last growth, letting steal traffic re-home the started member's share. */
    static constexpr uint32_t SHRINK_HOLDOFF_WINDOWS = 4;

    /** Waste-to-reward wait-outcome ratio above which a window reads as shrink-able; loaded widths measure 3-4x. */
    static constexpr uint64_t SHRINK_WASTE_FACTOR = 8;

    /**
     * Excess dispatches accumulated over a window before a member votes to grow. A
     * spread load dispatches one fiber per pass and never accumulates; a width-starved
     * closed wake loop accumulates per pass.
     */
    static constexpr uint64_t GROW_BACKLOG_COUNT = 16;

    //
    // Helpers.
    //

    /** Evaluate the wasteful-window shrink on the rightmost member, maintaining the streak. */
    void evaluateShrink(Window * state, uint16_t prefixIndex, uint16_t width, uint64_t nowCycles, Decision * decision) noexcept;

    //
    // State.
    //

    /** The window length in TSC cycles - the width-adaptation time constant. */
    uint64_t windowCycles = 0;

    /** The last growth's stamp, from any door - the growth pace and the shrink holdoff. */
    std::atomic<uint64_t> lastGrowCycles{};
};

} // namespace silk
