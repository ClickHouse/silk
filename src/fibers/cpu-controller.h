#pragma once

#include <silk/util/platform.h>
#include <silk/util/spinlock.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace silk
{

/**
 * Width policy for the scheduler's active CPU set: every grow and shed decision routes
 * through one gate, one growth-probe ledger, and one failed-growth suppression loop.
 * Part of the scheduler state; each processor embeds a Window, and the scheduler
 * executes the returned decisions.
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

        /** Consecutive shed-eligible windows; a shed needs a sustained run. */
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

    /** What the caller executes: GROW starts the next prefix processor, SHED moves the width down. */
    enum class Action : uint8_t
    {
        NONE,
        GROW,
        SHED,
    };

    /** One window verdict; a SHED moves the width from fromWidth to toWidth by CAS. */
    struct Decision
    {
        /** The move the caller executes. */
        Action action = Action::NONE;

        /** The width the SHED expects - a width moved meanwhile stays, the shed is best-effort. */
        uint16_t fromWidth = 0;

        /** The width the SHED drops to. */
        uint16_t toWidth = 0;
    };

    /** Set the window length and start the probe clock; called once at scheduler initialization. */
    void initialize(uint64_t windowCycles_, uint64_t nowCycles) noexcept
    {
        windowCycles = windowCycles_;

        // Start the probe clock so the first probe's pre-grow rate spans a real window,
        // not the whole uptime.
        probeCycles.store(nowCycles, std::memory_order_relaxed);
    }

    /**
     * Evaluate a member's completed window: a due probe resolves first, then the window
     * votes. elapsedCycles is the window's age, enqueuedTotal the fleet's cumulative
     * wake count. Resets the window counters; the caller restamps its window start.
     */
    Decision evaluateWindow(
        Window * state,
        uint16_t prefixIndex,
        uint16_t width,
        uint16_t widthTotal,
        uint64_t enqueuedTotal,
        uint64_t elapsedCycles,
        uint64_t nowCycles) noexcept;

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

    /** The single grow gate: suppression, a pending probe, and full width all refuse. */
    bool approveGrow(uint16_t width, uint16_t widthTotal, uint64_t nowCycles) const noexcept
    {
        if (width == widthTotal || isSuppressed(nowCycles))
        {
            return false;
        }

        return startedNumber.load(std::memory_order_relaxed) == kInvalidProcessorNumber;
    }

    /** Record a committed growth: arm the probe on the started member, snapshot the pre-grow rate. */
    void commitGrow(uint16_t startedNumber_, uint16_t width, uint64_t enqueuedTotal, uint64_t nowCycles) noexcept;

    /** Record a committed shed: reset the streak; the probe's own start shedding is a failed verdict. */
    void commitShed(Window * state, uint16_t number, uint64_t nowCycles) noexcept;

    /** Stamp a growth from any door; shedding holds off a few windows past it. */
    void stampGrow(uint64_t nowCycles) noexcept { lastGrowCycles.store(nowCycles, std::memory_order_relaxed); }

    /** True while failed-growth suppression holds; the aged-backlog rescue respects it too. */
    bool isSuppressed(uint64_t nowCycles) const noexcept { return nowCycles < suppressUntilCycles.load(std::memory_order_relaxed); }

private:
    //
    // Constants.
    //

    /** Consecutive shed-eligible windows required before the shed fires. */
    static constexpr uint32_t SHRINK_WINDOW_COUNT = 3;

    /** Windows the shed waits out past the last growth, letting steal traffic re-home the started member's share. */
    static constexpr uint32_t SHRINK_HOLDOFF_WINDOWS = 4;

    /** Waste-to-reward wait-outcome ratio above which a window reads as shed-able; loaded widths measure 3-4x. */
    static constexpr uint64_t SHRINK_WASTE_FACTOR = 8;

    /**
     * Excess dispatches accumulated over a window before a member votes to grow. A
     * spread load dispatches one fiber per pass and never accumulates; a width-starved
     * closed wake loop accumulates per pass.
     */
    static constexpr uint64_t GROW_BACKLOG_COUNT = 16;

    /**
     * Failed-growth suppression bounds, in windows: each failure doubles the suppression
     * from the floor to the cap; a growth that survives its verdict halves it back.
     */
    static constexpr uint32_t GROW_SUPPRESS_MIN_WINDOWS = 8;
    static constexpr uint32_t GROW_SUPPRESS_MAX_WINDOWS = 512;

    /** Windows a growth probe runs before its verdict - long enough for steal traffic to show in the wake rate. */
    static constexpr uint32_t GROW_VERDICT_WINDOWS = 8;

    /**
     * The verdict's gain bar is preRate / (margin * width) - a quarter of one more
     * core's proportional share. Wide fleets accept small marginal gains, a lone core
     * demands a quarter jump - noise cannot fake that.
     */
    static constexpr uint32_t GROW_VERDICT_MARGIN = 4;

    /**
     * Wake rates are fixed-point wakes per 2^20 cycles: a 1M wakes/s fleet on a 3 GHz
     * clock reads ~350, so the gain bar keeps resolution at low rates, and the shifted
     * delta only overflows past ~10^13 wakes per span.
     */
    static constexpr uint32_t RATE_SHIFT = 20;

    //
    // Helpers.
    //

    /** Resolve a due probe by the fleet's wake rate; a failed verdict returns the width revert. */
    Decision resolveProbe(uint64_t enqueuedTotal, uint64_t nowCycles) noexcept;

    /** Evaluate the wasteful-window shed on the rightmost member, maintaining the streak. */
    void evaluateShed(Window * state, uint16_t prefixIndex, uint16_t width, uint64_t nowCycles, Decision * decision) noexcept;

    /** Double the suppression from the floor to the cap - a growth failed to pay. */
    void suppressGrow(uint64_t nowCycles) noexcept
    {
        uint32_t windows = suppressWindows.load(std::memory_order_relaxed);
        windows = windows ? std::min<uint32_t>(windows * 2, GROW_SUPPRESS_MAX_WINDOWS) : GROW_SUPPRESS_MIN_WINDOWS;
        suppressWindows.store(windows, std::memory_order_relaxed);
        suppressUntilCycles.store(nowCycles + windows * windowCycles, std::memory_order_relaxed);
    }

    //
    // State.
    //

    /** The window length in TSC cycles - the width-adaptation time constant. */
    uint64_t windowCycles = 0;

    /** Serializes the probe ledger - arming, resolution, and the shed fast-fail. */
    SpinLock probeLock;

    /** The last growth's stamp, from any door; shedding waits SHRINK_HOLDOFF_WINDOWS past it. */
    std::atomic<uint64_t> lastGrowCycles{};

    /** No grow passes the gate before this stamp; doubled per failed growth. */
    std::atomic<uint64_t> suppressUntilCycles{};

    /** Current suppression length in windows; doubles per failure, halves per surviving growth. */
    std::atomic<uint32_t> suppressWindows{};

    /** The pending probe's started member; its shed is an instant failure verdict. */
    std::atomic<uint16_t> startedNumber{kInvalidProcessorNumber};

    /** The pre-grow width a failed probe reverts to. */
    std::atomic<uint16_t> probeWidth{};

    /** The pre-probe snapshot: stamp, fleet enqueue counter, and wake rate. */
    std::atomic<uint64_t> probeCycles{};
    std::atomic<uint64_t> probeEnqueued{};
    std::atomic<uint64_t> probeRate{};
};

} // namespace silk
