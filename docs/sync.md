# Synchronization Primitives

All primitives are fiber-aware: waiting suspends the calling fiber rather than
blocking the OS thread. Source lives in `src/fibers/`.

---

## Layering

```
FiberFuture         -- single-producer/single-consumer result handle
FiberFutex          -- counter-based wakeup (Linux futex pattern)
FiberMutex          -- mutual exclusion, unfair
FiberCondVar        -- condition variable (mirrors std::condition_variable)

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

Fiber-aware shared mutex. Conforms to `BasicLockable`, `Lockable`, and
`SharedMutex`; compatible with `std::lock_guard`, `std::unique_lock`, and
`std::shared_lock`.

```cpp
FiberMutex mutex;
mutex.lock();          /* exclusive */    mutex.unlock();
mutex.lock_shared();   /* shared */       mutex.unlock_shared();
```

**State** -- packed `uint64_t`:
`{value:61, exclusive:1, hasExclusiveWaiters:1, hasSharedWaiters:1}`.
The `value` field reinterprets as `Fiber *` when `exclusive=1` and as the
shared holder count when `exclusive=0`; `raw=0` is the canonical unlocked
state. Splitting the waiter bit by acquire mode is what enables
writer-priority: readers can check `hasExclusiveWaiters` on the fast path.

**Fast path** -- `try_lock()` does a single CAS from 0 to
`{value=currentFiber, exclusive=1}`. `try_lock_shared()` CASes
`{value, exclusive=0}` to `{value+1, exclusive=0}` only if
`hasExclusiveWaiters` is clear.

**Spin phase** -- `lock()` and `lock_shared()` spin 16 PAUSEs (~500 ns) only
when the blocker is an identifiable exclusive owner currently `RUNNING` on
another CPU and no waiter is yet queued. Shared holders have no recorded
identity, so exclusive acquirers waiting on shared traffic skip the spin
and go straight to suspend.

**Slow path** -- `lockHelper()` arms `hasExclusiveWaiters`;
`lockSharedHelper()` arms `hasSharedWaiters`. In each case `suspendCallback`
then enqueues the fiber into the waiter table. `unlock()` CASes state to 0
and calls `releaseWaiters` if either waiter bit was set. `unlock_shared()`
decrements `value`; when the count reaches 0 it clears both waiter bits
atomically and calls `releaseWaiters` if either was set.

**Writer priority (best-effort)** -- once an exclusive waiter sets
`hasExclusiveWaiters`, new readers see the bit on the fast path and queue
behind the writer rather than slipping in. The wake model is still
wake-all, so on the next release event every queued waiter races; if a
shared waiter wins, the writer re-arms the bit and re-suspends. The bit
is also briefly clear between the release CAS and the writer re-arming it,
so a reader arriving in that gap can slip ahead. In practice this strongly
favors writers without providing strict starvation-freedom.

**suspendCallback race** -- after enqueuing, the waiter re-reads its
matching bit (`hasExclusiveWaiters` for an exclusive waiter,
`hasSharedWaiters` for a shared one). If false, the previous releasing
fiber already called `releaseWaiters` but missed us (we were not yet in
the waiter table); we self-release. The woken fibers retry their helper
and either acquire or re-arm the matching bit, and the next release
delivers them properly.

---

## FiberCondVar

Fiber-aware condition variable. Mirrors `std::condition_variable`: a fiber
atomically releases a `Lockable` and suspends inside `wait()`; the lock is
re-acquired before `wait()` returns. `notify_one()` wakes one currently
waiting fiber (no-op if none is waiting); `notify_all()` wakes everyone
currently waiting and has no effect on fibers that arrive later. Spurious
wakeups are permitted -- callers must re-check the predicate.

```cpp
FiberCondVar cv;
FiberMutex mutex;
// waiter
std::unique_lock lock(mutex);
while (!predicate) { cv.wait(lock); }
// signaller
{
    std::lock_guard guard(mutex);
    /* update predicate */
}
cv.notify_one();   // or cv.notify_all()
```

`wait` accepts any type with `lock()`/`unlock()`, so it composes with
`FiberMutex`, `std::unique_lock<FiberMutex>`, etc.

**State** -- a `SpinLock` protects an intrusive `List<Future>` of waiters.
Each waiter is a `Future` (inherits `FiberFuture`) allocated on the calling
fiber's stack by `wait()` / `wait_for()` and linked into the list.

**wait_for** -- `cv.wait_for(lock, nanoseconds)` returns 0 on a notify-driven
wakeup or `ETIMEDOUT` on timeout. Implemented via
`FiberFuture::waitWithTimeout`; on timeout the future cancels itself, which
removes it from the waiter list. A notify that races at the boundary may
consume our future but `wait_for` still returns `ETIMEDOUT` -- the
predicate re-check handles the lost wakeup, per the usual cv contract.

**Cancel/notify race** -- a single `inWaiters` bool per waiter, read and
written only under `spinLock`, serializes timeout-triggered self-cancel
against `notify_one` / `notify_all`. Exactly one of those paths removes the
future from the list and calls `set()`; the other observes `inWaiters` is
false and bails. `notify_all` splices the waiter list into a local snapshot
and clears `inWaiters` on each, all under the lock; the actual `set(0)`
calls fire outside the lock.

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
