#pragma once

#include <silk/fibers/sequencer.h>

namespace silk
{

/**
 * Fiber-aware manual-reset event with Future support.
 *
 * The event is either set or unset. set() wakes all current waiters and
 * keeps the event set until reset() is called. wait() returns immediately
 * if the event is already set.
 */
class FiberEvent
{
public:
    using Future = FiberSequencer::Future;

    /** Return true if the event is currently set. */
    bool isSet() const noexcept { return sequencer.get() & 1; }

    /** Signal the event and wake all current waiters. No-op if already set. */
    void set() noexcept { (void)sequencer.advance(sequencer.get() | 1); }

    /** Clear the event. No-op if already unset. */
    void reset() noexcept
    {
        uint64_t current = sequencer.get();
        (void)sequencer.advance(current + (current & 1));
    }

    /** Wait until the event is set, blocking the calling fiber. */
    void wait() noexcept
    {
        uint64_t current = sequencer.get();
        if (current & 1)
        {
            return;
        }
        sequencer.wait(current + 1);
    }

    /**
     * Register @p future to be set when the event is signalled.
     * Sets the future immediately if the event is already set.
     */
    void wait(Future * future) noexcept
    {
        uint64_t current = sequencer.get();
        if (current & 1)
        {
            future->set(0);
            return;
        }
        sequencer.wait(current + 1, future);
    }

private:
    FiberSequencer sequencer;
};

} // namespace silk
