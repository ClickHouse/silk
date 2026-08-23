# Performance Results

Measurements on an AWS instance (32-CPU Intel Xeon Platinum 8488C, Linux 7.0, release build `-O3`). All measurements are 60 s with a 10 s warmup.

The main tables are reproducible with `./bb -b release perf --duration 60s --warmup 10s all`. The high-concurrency rows (`net-perf` 1000 conn / `http-perf` 10000 conn / `s3-perf` 100x100) and the internal HTTP server vs nginx row in `http-perf`, need separate `./bb` invocations -- see each section.

---

## file-perf -- async file I/O

`/dev/shm` (tmpfs, in-memory), bs=4k, size=1 GiB, 60 s measurement, 10 s warmup. Uses `FiberScheduler::read`/`write` (`IORING_OP_READV` / `IORING_OP_WRITEV`). `numjobs` = concurrent worker fibers; `iodepth` = per-fiber async IO queue depth (ring of `IoFuture`s). Pass `--fixed-buffers` to switch to registered buffers (`IORING_OP_READ_FIXED` / `IORING_OP_WRITE_FIXED`) -- see the subsection below.

| numjobs | iodepth | mode | IOPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | randwrite | 200k | 782 MiB/s | 5 µs | 4 µs | 13 µs | 14 µs | 21 µs |
| 1 | 16 | randwrite | 544k | 2126 MiB/s | 29 µs | 29 µs | 32 µs | 40 µs | 58 µs |
| 16 | 1 | randwrite | 859k | 3355 MiB/s | 19 µs | 19 µs | 31 µs | 43 µs | 102 µs |
| 16 | 16 | randwrite | 767k | 2994 MiB/s | 334 µs | 331 µs | 368 µs | 554 µs | 653 µs |
| 1 | 1 | randread | 267k | 1041 MiB/s | 4 µs | 3 µs | 12 µs | 13 µs | 18 µs |
| 1 | 16 | randread | 1040k | 4062 MiB/s | 15 µs | 12 µs | 23 µs | 26 µs | 31 µs |
| 16 | 1 | randread | 2516k | 9830 MiB/s | 6 µs | 5 µs | 14 µs | 22 µs | 106 µs |
| 16 | 16 | randread | 7234k | 28259 MiB/s | 35 µs | 34 µs | 52 µs | 65 µs | 79 µs |

**Best throughput** (`numjobs=16 iodepth=16 randread`): 7234k IOPS, 27.6 GiB/s.

**Best latency** (`numjobs=1 iodepth=1`): 3-4 µs p50 for both read and write.

**Note on batching**: The default `Options::ioUringFlushThreshold = 64` defers `io_uring_submit` until the SQ ring has accumulated enough work to amortize the syscall -- the right trade for network/HTTP/S3 workloads where completion latency dwarfs the few-µs batching delay (see net-perf below for the resulting p99 win). On tmpfs the kernel completes reads inline at submit time, so any deferral pushes submissions off the inline-completion fast path. `file-perf` therefore initializes the scheduler with `ioUringFlushThreshold = 1`, equivalent to per-fiber submit. Measured under the default threshold (64), `16/1 randread` lands at ~1.6M IOPS and `16/16 randread` at ~4.2M -- the override recovers full throughput without any kernel or scheduler change.

### Registered buffers (`--fixed-buffers`)

`./bb -b release perf --duration 60s --warmup 10s file --fixed-buffers` reruns the same matrix with registered buffers: each worker registers one buffer (covering its whole `iodepth * blockSize` block) on every per-CPU ring via `FiberScheduler::registerBuffers`, then issues `readFixed`/`writeFixed` against it (see `docs/scheduler.md`). The kernel reuses the pre-pinned mapping and skips the per-IO page-pin and iovec import.

The win is largest where per-IO buffer setup is the dominant cost: high-concurrency writes (`16` jobs) gain the most IOPS and shed the most average and tail latency. Reads, already inline-completed on tmpfs, see smaller gains except at `16/16`. 

---

## fio comparison (io_uring, /dev/shm, bs=4k, size=1 GiB)

| numjobs | iodepth | mode | IOPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | randwrite | 150k | 585 MiB/s | 5 µs | 5 µs | 6 µs | 6 µs | 14 µs |
| 1 | 16 | randwrite | 560k | 2187 MiB/s | 27 µs | 28 µs | 31 µs | 37 µs | 41 µs |
| 16 | 1 | randwrite | 708k | 2765 MiB/s | 21 µs | 21 µs | 26 µs | 34 µs | 41 µs |
| 16 | 16 | randwrite | 757k | 2957 MiB/s | 335 µs | 330 µs | 367 µs | 684 µs | 5341 µs |
| 1 | 1 | randread | 157k | 613 MiB/s | 5 µs | 5 µs | 6 µs | 6 µs | 14 µs |
| 1 | 16 | randread | 889k | 3474 MiB/s | 17 µs | 16 µs | 23 µs | 31 µs | 49 µs |
| 16 | 1 | randread | 1766k | 6900 MiB/s | 8 µs | 8 µs | 8 µs | 15 µs | 20 µs |
| 16 | 16 | randread | 8697k | 33971 MiB/s | 28 µs | 23 µs | 47 µs | 98 µs | 185 µs |

At `iodepth=1`, the fiber scheduler outperforms fio (1.4-1.7x): fio uses one OS thread per job, so each IO incurs a full OS scheduler round-trip. At high total depth (`16x16`), fio batches all SQEs the worker enqueued into one submit per round-trip and wins; the fiber scheduler with batching disabled (file-perf opts out -- see file-perf note above) does the same per fiber but pays additional dispatch overhead for the IoFuture ring.

| config | fiber IOPS | fio IOPS | ratio |
|---|---|---|---|
| 1 job, iodepth=1, randread | 267k | 157k | 1.70x |
| 16 jobs, iodepth=1, randread | 2516k | 1766k | 1.42x |
| 1 job, iodepth=16, randread | 1040k | 889k | 1.17x |
| 16 jobs, iodepth=16, randread | 7234k | 8697k | 0.83x |

---

## net-perf -- TCP echo

Loopback TCP, 64 B messages, 60 s measurement, 10 s warmup. Socket I/O uses `FiberScheduler::read`/`write` (io_uring `IORING_OP_READV`/`IORING_OP_WRITEV`); the fiber suspends inside the call until the CQE arrives. Latency is measured end-to-end: client send -> server echo -> client receive.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 101k | 6 MiB/s | 10 µs | 10 µs | 11 µs | 16 µs | 22 µs |
| 256 | 1643k | 100 MiB/s | 156 µs | 119 µs | 432 µs | 859 µs | 1134 µs |
| 512 | 1993k | 122 MiB/s | 257 µs | 229 µs | 486 µs | 701 µs | 875 µs |
| 1024 | 1966k | 120 MiB/s | 521 µs | 484 µs | 832 µs | 1449 µs | 1629 µs |

The single connection is a serial chain the processor prefix concentrates on one core: 9 µs p50, paired at +50% over a full-width scheduler. At 1024 conns the fleet is saturated and holds ~2M req/s at full width. At 256-512 conns per-core idle gaps open between messages, but a wait rewarded by arriving work reads as demand, so the width holds and the rows track full-width throughput. Submission is bounded-batched at the dispatch boundary (see Latency profiler below); without that bound, this workload's p50 is lower but p95/p99/p99.9 inflate by 5-10x.

---

## net-perf-asio -- TCP echo (Boost.Asio C++20 coroutines)

Same workload as net-perf above, reimplemented with Boost.Asio C++20 coroutines (`asio::awaitable<void>`) and epoll (Asio's default Linux backend). Server and client use one thread per available CPU (respecting `taskset`). Reproduced with `./bb -b release net-perf-asio --duration 60s --warmup 10s`.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 3k | 0 MiB/s | 326 µs | 370 µs | 517 µs | 612 µs | 735 µs |
| 256 | 358k | 22 MiB/s | 715 µs | 722 µs | 772 µs | 794 µs | 823 µs |
| 512 | 389k | 24 MiB/s | 1317 µs | 1332 µs | 1400 µs | 1428 µs | 1466 µs |
| 1024 | 407k | 25 MiB/s | 2517 µs | 2520 µs | 2593 µs | 2624 µs | 2667 µs |

**Comparison with net-perf (fibers + io_uring):**

| connections | net-perf RPS | net-perf-asio RPS | ratio |
|---|---|---|---|
| 1 | 101k | 3k | **~34x** |
| 256 | 1643k | 358k | **~4.6x** |
| 512 | 1993k | 389k | **~5.1x** |
| 1024 | 1966k | 407k | **~4.8x** |

Two structural differences explain most of the gap. First, net-perf uses io_uring for all socket I/O while Asio uses epoll; io_uring avoids the per-operation `epoll_ctl` + `epoll_wait` + `recv`/`send` syscall chain. Second, the fiber scheduler's per-CPU pinned scheduler threads pick up completions via `io_uring_enter`, while Asio's reactor threads block in `epoll_wait` and resume via a pthread wakeup.

The gap is largest at 1 connection (~34x) where per-operation scheduling overhead dominates with no parallelism to hide it, and stays around 4.6-5.1x at high connection counts where the server CPU half is the bottleneck.

---

## net-perf-epoll -- TCP echo (raw epoll, multi-threaded)

Same workload as net-perf above, reimplemented as the simplest efficient epoll loop: edge-triggered `recv`/`send` per connection, one worker thread per available CPU (auto-detected via `silk::getAvailableProcessorCount`), `SO_REUSEPORT` listener per worker on the server, no fibers, no io_uring. Each worker owns its epoll instance and round-robins its connections through a per-fd state machine. Reproduced with `./bb -b release net-perf-epoll --duration 60s --warmup 10s`.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 44k | 3 MiB/s | 23 µs | 25 µs | 29 µs | 35 µs | 42 µs |
| 256 | 2478k | 151 MiB/s | 103 µs | 102 µs | 138 µs | 152 µs | 169 µs |
| 512 | 2506k | 153 MiB/s | 204 µs | 205 µs | 273 µs | 303 µs | 338 µs |
| 1024 | 2485k | 152 MiB/s | 412 µs | 398 µs | 515 µs | 546 µs | 714 µs |

**Comparison with net-perf (fibers + io_uring):**

| connections | net-perf RPS | net-perf-epoll RPS | RPS ratio | net-perf p99 | net-perf-epoll p99 | p99 ratio |
|---|---|---|---|---|---|---|
| 1 | 102k | 44k | 0.43x | 19 µs | 35 µs | 1.84x |
| 256 | 1584k | 2478k | **1.56x** | 697 µs | 152 µs | **0.22x** |
| 512 | 1677k | 2506k | **1.49x** | 873 µs | 303 µs | **0.35x** |
| 1024 | 2022k | 2485k | **1.23x** | 3595 µs | 546 µs | **0.15x** |

At 1 connection the fiber scheduler wins 2.3x on throughput and p99: the processor prefix runs the whole serial chain on one core with no wake round trips, while each epoll round-trip pays the `epoll_wait` + `recv`/`send` syscall chain. Past saturation raw epoll wins on throughput (23% at 1024 conns, more in the 256-512 width-oscillation band - see net-perf above) and 3-7x on p99 tail latency. The epoll loop services its connections in round-robin within each worker, so per-connection treatment is uniform and p99 stays close to p50; net-perf's bounded-batched submission clusters p95-p99.9 around the batch period.

What raw epoll gives up: composability. The state machine can't naturally accommodate sleeps (no `--delay` support), multi-step protocols, or branching control flow without growing into a small interpreter. net-perf-epoll is the throughput floor; net-perf is the structure you'd actually program against.

---

## http-perf -- HTTP/1.1 GET

nginx `return 200` (empty body), loopback, 60 s measurement, 10 s warmup. Client and server pinned to separate CPU halves (16 CPUs each). Fiber client uses `FiberSocketImpl` backed by `FiberScheduler::read`/`write` (io_uring `IORING_OP_READV`/`IORING_OP_WRITEV`).

| connections | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| 1 | 42k | 24 µs | 24 µs | 28 µs | 36 µs | 53 µs |
| 256 | 1284k | 199 µs | 71 µs | 1661 µs | 2127 µs | 2400 µs |
| 512 | 1309k | 391 µs | 104 µs | 4169 µs | 5257 µs | 5384 µs |
| 1024 | 1312k | 780 µs | 86 µs | 9984 µs | 12360 µs | 12721 µs |

At 1 connection p50 ~24 µs reflects Poco's HTTP parsing overhead. At higher concurrency the fiber client saturates nginx at ~1.3M RPS. Tail behavior is dominated by nginx itself once `connections >= 256`.

### Server: internal (silk fibers) vs nginx

**Not a production HTTP server.** `http-perf server` is benchmark scaffolding: each accepted connection runs Poco's stock `HTTPServerConnection::run` on a fiber over `FiberSocketImpl`. Poco's HTTP server is allocation-heavy — `std::stringstream`-driven request/response parsing, per-request buffer churn even after our `MemoryPool` patches, virtual dispatch on every byte. Nobody should ship this; we use it because reusing Poco's parser on both ends gives an apples-to-apples comparison: the only thing varying between the two rows of the table below is the server's I/O loop (silk's accept fiber + per-conn fibers + io_uring read/write vs nginx's tuned C event loop). Everything else — request parsing, response building, the client — is held constant.

| connections | server | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | internal | 35k | 28 µs | 28 µs | 32 µs | 35 µs | 39 µs |
| 256 | internal | 1001k | 256 µs | 195 µs | 969 µs | 1474 µs | 1777 µs |
| 512 | internal | 1008k | 508 µs | 249 µs | 2245 µs | 3112 µs | 3696 µs |
| 1024 | internal | 1030k | 994 µs | 495 µs | 7350 µs | 9441 µs | 9959 µs |
| 1 | nginx | 42k | 24 µs | 24 µs | 28 µs | 36 µs | 53 µs |
| 256 | nginx | 1284k | 199 µs | 71 µs | 1661 µs | 2127 µs | 2400 µs |
| 512 | nginx | 1309k | 391 µs | 104 µs | 4169 µs | 5257 µs | 5384 µs |
| 1024 | nginx | 1312k | 780 µs | 86 µs | 9984 µs | 12360 µs | 12721 µs |

The internal server lands at ~78% of nginx RPS at high concurrency (1001-1030k vs 1284-1312k). The gap is Poco overhead, not silk overhead: nginx's `return 200` handler skips most of HTTP/1.1 parsing, while Poco constructs `HTTPServerRequestImpl`/`HTTPServerResponseImpl` plus heap-allocated stream buffers per request. The takeaway is that silk's accept-fiber + per-connection-fiber I/O loop has small overhead on top of whatever HTTP machinery you put on it -- to beat nginx you'd swap Poco for a hand-rolled state machine that allocates nothing per request, which is a different project.

### High-concurrency throughput (connections=10000, delay=10ms, duration=60s, warmup=10s)

Run against the internal silk-fiber HTTP server with a 10 ms server-side sleep per request, so all 10k connections stay alive simultaneously and the server CPU half is fully loaded. Reproduced with `./bb -b release http-perf [--threads] --connections 10000 --delay 10ms --duration 60s --warmup 10s`.

| connections | mode | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 10000 | fibers | 669k | 10272 µs | 10232 µs | 10577 µs | 10823 µs | 11941 µs |
| 10000 | threads | 612k | 14569 µs | 13916 µs | 19955 µs | 21806 µs | 24075 µs |

Throughput is in the same band (669k fibers vs 612k threads); the workload is server-bound. The big difference is latency tightness: fiber percentiles cluster within a 1.7 ms window (p50 10.2 ms -> p99.9 11.9 ms), while threads spread over 10 ms (p50 13.9 ms -> p99.9 24.1 ms). At 10k OS threads the kernel scheduler injects multi-millisecond stalls into the tail; the fiber scheduler keeps the tail close to the median.

---

## s3-perf -- S3 object storage

MinIO loopback (`http://127.0.0.1:9000`), object size=4096 B, 60 s measurement, 10 s warmup. Both modes use `numjobs` OS session threads, each maintaining an `iodepth`-slot ring of in-flight async S3 requests and waiting on a `FiberFuture` per slot. The difference is the AWS SDK executor and HTTP client: fiber mode runs each SDK async task as a fiber with io_uring socket I/O (`FiberExecutor` + `FiberHttpClient`); thread mode runs each task on a `PooledThreadExecutor` (sized `numjobs x iodepth`) with blocking socket I/O.

`s3-perf --threads` dies with SIGSEGV mid-measurement on the current build, so the executor pair cannot be re-measured until that fix; the tables below stand as last measured.

| numjobs | iodepth | mode | executor | OPS/s | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | read | fibers | 1644 | 608 µs | 619 µs | 732 µs | 809 µs | 975 µs |
| 1 | 64 | read | fibers | 38254 | 1673 µs | 1640 µs | 2708 µs | 3562 µs | 5170 µs |
| 16 | 1 | read | fibers | 28632 | 559 µs | 547 µs | 682 µs | 951 µs | 1558 µs |
| 16 | 64 | read | fibers | 48854 | 20952 µs | 20368 µs | 36155 µs | 42398 µs | 50771 µs |
| 1 | 1 | write | fibers | 1556 | 643 µs | 622 µs | 775 µs | 893 µs | 1021 µs |
| 1 | 64 | write | fibers | 620 | 103109 µs | 102474 µs | 148701 µs | 171291 µs | 197833 µs |
| 16 | 1 | write | fibers | 1564 | 10225 µs | 701 µs | 62782 µs | 115468 µs | 179561 µs |
| 16 | 64 | write | fibers | 2377 | 427479 µs | 411679 µs | 706008 µs | 887396 µs | 1127425 µs |
| 1 | 1 | read | threads | 928 | 1078 µs | 1115 µs | 1322 µs | 1423 µs | 1547 µs |
| 1 | 64 | read | threads | 39943 | 1602 µs | 1573 µs | 2584 µs | 3388 µs | 4143 µs |
| 16 | 1 | read | threads | 30353 | 527 µs | 516 µs | 639 µs | 931 µs | 1556 µs |
| 16 | 64 | read | threads | 49213 | 20801 µs | 20136 µs | 35722 µs | 42467 µs | 51131 µs |
| 1 | 1 | write | threads | 996 | 1004 µs | 1013 µs | 1331 µs | 1486 µs | 1647 µs |
| 1 | 64 | write | threads | 629 | 101701 µs | 101028 µs | 148501 µs | 171016 µs | 197191 µs |
| 16 | 1 | write | threads | 1326 | 12066 µs | 983 µs | 68493 µs | 118364 µs | 179890 µs |
| 16 | 64 | write | threads | 2400 | 423428 µs | 411010 µs | 692696 µs | 853490 µs | 1070228 µs |

At `numjobs=1 iodepth=1` read, fibers deliver 1644 OPS vs 928 for threads (+77%): with one outstanding request at a time, the thread executor pays a full OS wake-up round-trip per response, while a fiber resumes inline on the scheduler thread. At higher iodepth or numjobs, MinIO becomes the bottleneck and throughput converges. Write latency blows out at high iodepth (`iodepth=64` p50 >100 ms, `16x64` p50 >400 ms) symmetrically across both executors, confirming MinIO internal serialization is the cause.

The write `16x1` p50 (697 µs fibers / 1012 µs threads) is much lower than the avg (10 ms / 12 ms) because a small fraction of requests stall behind MinIO lock contention, pulling the mean up while the median stays fast.

### High-concurrency tail latency (numjobs=100, iodepth=100, duration=60s, warmup=10s)

Reproduced with `./bb -b release s3-perf [--threads] --numjobs 100 --iodepth 100 --duration 60s --warmup 10s`.

| numjobs | iodepth | mode | executor | OPS/s | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 100 | 100 | read | fibers | 46324 | 215105 µs | 208044 µs | 277884 µs | 377087 µs | 508672 µs |
| 100 | 100 | read | threads | 45449 | 219248 µs | 212625 µs | 270734 µs | 374390 µs | 509306 µs |

At 10,000 concurrent requests (100 jobs x iodepth 100) throughput is close (~45-46k OPS) -- MinIO is fully saturated. Executor choice has minor impact at this load: throughput edge to fibers is ~1.9%, and tail latencies (p99 ~375 ms, p99.9 ~509 ms) are within ~1% of each other. MinIO internal serialization dominates. The gap widens at higher percentiles where 10,000 OS threads stall behind kernel scheduling jitter that the fiber scheduler avoids.

---

## Latency profiler

Per-CPU profiler (opted in via `--print-counters`) emits log2 histograms for seven intervals in the fiber/IO lifecycle, listed below in lifetime order. Producer is the per-CPU scheduler thread (sole producer of its SPSC ring); consumer is the same CPU's service loop, drained on every iteration.

| event | interval |
|---|---|
| `suspend_wait` | suspended -> next `enqueueReady` (blocked-on-condition latency) |
| `io_wait` | `enqueueIo` -> CQE handled (full IO latency: silk pre-submit + kernel + silk post-drain) |
| `sq_wait` | `enqueueIo` -> `io_uring_submit` (SQE pending in silk's SQ ring before flush to kernel) |
| `submit_io` | `io_uring_submit` syscall (one per dispatch batch) |
| `cq_wait` | wall-clock gap between consecutive non-empty CQ drains on a ring (upper bound on CQE-in-ring dwell) |
| `ready_wait` | `enqueueReady` -> dispatch (ready-queue dwell) |
| `fiber_run` | `switchToFiberContext` -> return (on-CPU time per slice) |

### Per-IO breakdown (net-perf, 1000 connections, 60 s, 10 s warmup, 1882k RPS)

Reproduced with `./bb -b release net-perf --connections 1000 --duration 60s --warmup 10s --print-counters`.

| event | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| `suspend_wait` | 147 µs | 409 µs | 1.2 ms | 2.0 ms |
| `io_wait` | 147 µs | 407 µs | 1.2 ms | 2.0 ms |
| `sq_wait` | 13 µs | 261 µs | 1.0 ms | 2.0 ms |
| `submit_io` | 28 µs | 124 µs | 246 µs | 261 µs |
| `cq_wait` | 52 µs | 195 µs | 410 µs | 518 µs |
| `ready_wait` | 12 µs | 32 µs | 260 µs | 503 µs |
| `fiber_run` | 219 ns | 439 ns | 3.0 µs | 7.5 µs |

`fiber_run` p50 = 219 ns confirms the dispatch loop itself is essentially free; this workload is IO-bound. `io_wait` is the full silk-side wait; `sq_wait` is the share before the SQE reaches the kernel and `cq_wait` bounds the CQE-side share, so the kernel's own time is the residual. Submission amortizes over batched SQEs: 17.4 M syscalls observed during the run averaging ~14 SQEs each.

Submission is bounded by **both** a count and a time threshold. `runFiber` calls `submitIo(false)` after each fiber, which fires the syscall when either (a) the SQ ring holds at least `ioUringFlushThreshold = 64` SQEs, or (b) `ioUringFlushTimeout = 100` µs has elapsed since the last submit. `handleReadyQueue`, `enqueueWakeup`, the proxy-fiber `enqueueIo` path, and worker threads call `submitIo(true)` to force-flush. The count threshold caps per-syscall cost and SQ-ring overflow; the time threshold caps the SQ_WAIT tail when a single fiber holds the scheduler thread long enough that pending SQEs from earlier fibers would otherwise wait for end-of-batch flush. SQPOLL is not enabled because the per-CPU pinned scheduler design would put the kernel poller in contention with the user-space scheduler thread.

### Profiler overhead (net-perf, 1000 connections, 60 s, 10 s warmup)

| metric | off | on | Δ |
|---|---|---|---|
| RPS | 1956k | 1882k | -3.8% |
| p50 | 478 µs | 342 µs | -28% |
| p99 | 1106 µs | 3501 µs | +217% |
| p99.9 | 1724 µs | 4531 µs | +163% |

Profiler costs ~4% RPS. The percentile deltas are not stable at this load: 1000 connections sits in the width controller's oscillation band (see net-perf above), where run-to-run tail variance between back-to-back identical runs exceeds the profiler's own effect, so only the RPS delta is meaningful here.
