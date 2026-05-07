# Synchronization Primitives

All primitives are fiber-aware: waiting suspends the calling fiber rather than
blocking the OS thread. Source lives in `src/fibers/`.

---

## Layering

```
FiberFuture         -- single-producer/single-consumer result handle
FiberFutex          -- counter-based wakeup (Linux futex pattern)
FiberMutex          -- mutual exclusion, unfair

FiberSequencer      -- monotone counter with ordered, cancellable waiters
  FiberEvent        -- manual-reset event (built on FiberSequencer)
  FairFiberMutex    -- fair (FIFO) mutex (built on FiberSequencer)
```

`FiberFuture` is the leaf; every other primitive either uses the scheduler's
waiter table directly or builds on `FiberSequencer`.

---

## FiberFuture

Single-producer, single-consumer result handle. The producer calls `set(err)`;
the consumer calls `wait()` or `isSet()`.

```cpp
FiberFuture future;
// producer
future.set(0);          // or future.set(errno)
// consumer
int err = future.wait(); // suspends until set
```

`reset()` clears the future so it can be reused for the next operation.

**State** -- packed `uint64_t`: `{waiter:61, multipleWait:1, hasCallback:1, isSet:1}`.
The `waiter` field holds a `Fiber*` (normal wait), a `MultipleWaitState*`
(multiple wait), or a `SubscribeCallback*` (subscribe). The `multipleWait`
and `hasCallback` bits select the dispatch path in `signal()`; only one is
ever set on a given future.

**suspendCallback race** -- the waiter pointer is installed inside
`suspendCallback`, which runs after the fiber is parked. If `signal()` arrives
between `wait()` reading `isSet=false` and the callback running, the callback
finds `isSet=true` and reschedules the fiber immediately rather than parking it.

### waitForMultiple

Blocks until at least one future in an array is set. Attaches a
`MultipleWaitState` (stack-allocated) to each future in order, stopping early
if one is already set. Once woken, detaches from all futures and spins on a
`completionCounter` until every in-flight `signal()` has incremented it -- this
ensures the `MultipleWaitState` on the stack is safe to destroy on return.

### waitWithTimeout

```cpp
int err = FiberFuture::waitWithTimeout(&future, nanoseconds);
// returns ETIMEDOUT if the deadline expires first
```

Implemented as `waitForMultiple` over `{future, sleepFuture}`. Whichever fires
first wins; the other is cancelled before `waitWithTimeout` returns.

---

## FiberFutex

Counter-based wakeup, mirroring the Linux futex pattern. Multiple fibers may
wait concurrently; `post()` wakes all of them.

```cpp
FiberFutex futex;
// waiter
uint64_t token = futex.get();
// check condition ...
futex.wait(token + 1);   // or futex.wait() as shorthand for wait(get() + 1)
// poster
futex.post();            // increments counter, wakes all waiters
```

**State** -- packed `uint64_t`: `{counter:63, hasWaiters:1}`. `post()` clears
`hasWaiters` in the same CAS that increments the counter, then calls
`releaseWaiters` if the old value had the bit set.

**Spin phase** -- 16 PAUSEs (~500 ns) before suspending when no waiters are
queued, identical strategy to `FiberMutex`.

**Waiter table** -- uses `FiberScheduler::enqueueWaiter` / `releaseWaiters`
keyed by `this`. `post()` uses release ordering; `get()` and `wait()` use
acquire.

---

## FiberMutex

Unfair fiber-aware mutex. Conforms to `BasicLockable` / `Lockable`; compatible
with `std::lock_guard` and `std::unique_lock`.

**State** -- packed `uint64_t`: `{owner:63, hasWaiters:1}`.

**Fast path** -- `try_lock()` does a single CAS from 0 to `{currentFiber, 0}`.

**Spin phase** -- `lock()` spins 16 PAUSEs (~500 ns) if the owner is `RUNNING`
on another CPU and no waiters are queued, before falling back to the slow path.

**Slow path** -- `lockHelper()` sets the `hasWaiters` flag, then
`suspendCallback` enqueues the fiber into the waiter table. `unlock()` CASes
state to 0 and calls `releaseWaiters` if `hasWaiters` was set.

**suspendCallback race** -- after enqueuing, if `hasWaiters` is false (the
holder unlocked and re-acquired between our enqueue and this check),
`releaseWaiters` is called immediately. This causes a spurious wakeup; the
woken fibers retry `lockHelper`, re-set `hasWaiters`, and the next `unlock`
will release them properly.

---

## FiberSequencer

Monotone counter with fiber-aware ordered waiters. The foundation for
`FiberEvent` and `FairFiberMutex`.

```cpp
FiberSequencer seq;
// waiter
FiberSequencer::Future future;
seq.wait(token, &future);  // registers future; set when counter >= token
future.wait();             // or seq.wait(token) for blocking shorthand
future.cancel();           // cancel a pending wait (sets future with ECANCELED)
// producer
seq.increment();           // counter++, wakes all newly satisfied futures
seq.advance(value);        // CAS counter to value if value > current
```

`Future` inherits `FiberFuture`, so it can be passed to
`FiberFuture::waitForMultiple` and `waitWithTimeout`.

**Internal structure** -- three concurrent data structures protected by a
combiner lock:

- `requestQueue` (`LockFreeStack`) -- incoming `Future*` pushed by `wait()`
- `cancelQueue` (`LockFreeStack`) -- futures pushed by `cancel()`
- `waiters` (`Tree<Future>`) -- futures inserted by the combiner, sorted by token

**Combiner pattern** -- `drain()` is called after every `increment()`,
`advance()`, and `cancel()`. Only one thread runs it at a time; others CAS the
combiner state to `PENDING` and return. The active combiner loops until it can
transition from `BUSY` back to `FREE` without seeing `PENDING`, meaning no new
work arrived during its pass.

Inside the combiner:
1. Drain `cancelQueue`: remove each future from the tree, add to cancel list.
2. Drain `requestQueue`: classify each future -- if `CANCELLED` raced ahead,
   add to cancel list; otherwise insert into the tree.
3. Walk the tree from the minimum: wake all futures whose `token <= counter`.

Wakes and cancels are deferred until after the combiner is released, so
`future.set()` can itself call `drain()` without deadlocking.

**Cancellation state** -- `Future::state` has two bits: `IN_TABLE` (set by the
combiner when inserting into the tree) and `CANCELLED` (set by `cancel()`).
The `IN_TABLE` bit ensures `cancelQueue` holds the future only while it is in
the tree, mirroring the sleep cancellation pattern in the scheduler.

---

## FiberEvent

Manual-reset event built on `FiberSequencer`. The event is either set or unset;
`set()` wakes all current waiters and the event stays set until `reset()`.

```cpp
FiberEvent event;
// waiter
event.wait();                     // suspends if unset, returns immediately if set
FiberEvent::Future future;
event.wait(&future);              // async variant
// signaller
event.set();
event.reset();
```

**Implementation** -- maps the set/unset state onto the sequencer counter's
LSB: odd = set, even = unset.

- `isSet()`: `counter & 1`
- `set()`: `advance(counter | 1)` -- no-op if already odd
- `reset()`: `advance(counter + (counter & 1))` -- increments by 1 only if odd,
  making it even
- `wait()`: waits for the next odd value (`counter + 1` if currently even)

---

## FairFiberMutex

Fair (FIFO) mutex implemented as a ticket lock over `FiberSequencer`. Conforms
to `BasicLockable`; compatible with `std::lock_guard`.

```cpp
FairFiberMutex mutex;
mutex.lock();    // takes a ticket, waits until served
mutex.unlock();  // increments the serve counter, wakes the next waiter

FairFiberMutex::Future future;
mutex.lock(&future);  // takes a ticket, returns immediately; caller waits on future
```

**Implementation** -- `nextTicket` is the issue counter; the sequencer counter
is the serve counter. `lock()` does `fetch_add(1)` on `nextTicket` to claim a
ticket, then calls `sequencer.wait(ticket)` to block until that ticket is
served. `unlock()` calls `sequencer.increment()`, which advances the serve
counter by one and wakes the next waiter in token order.

The async `lock(Future*)` variant is useful when the caller needs to signal
readiness after taking a ticket but before actually blocking -- for example, to
post a message to another fiber confirming the lock request is queued.
