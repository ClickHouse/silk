# Performance Results

Measurements on an AWS instance (32-CPU Intel Xeon Platinum 8488C, Linux 6.17, release build `-O3`). All measurements are 60 s with a 10 s warmup.

The main tables are reproducible with `./bb -b release perf --duration 60s --warmup 10s all`. The high-concurrency rows (`net-perf` 1000 conn / `http-perf` 10000 conn / `s3-perf` 100x100) and the internal HTTP server vs nginx row in `http-perf`, need separate `./bb` invocations -- see each section.

---

## file-perf -- async file I/O

`/dev/shm` (tmpfs, in-memory), bs=4k, size=1 GiB, 60 s measurement, 10 s warmup. Uses `FiberScheduler::read`/`write` (`IORING_OP_READV` / `IORING_OP_WRITEV`). `numjobs` = concurrent worker fibers; `iodepth` = per-fiber async IO queue depth (ring of `IoFuture`s).

| numjobs | iodepth | mode | IOPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | randwrite | 162k | 634 MiB/s | 6 µs | 4 µs | 13 µs | 15 µs | 23 µs |
| 1 | 16 | randwrite | 511k | 1995 MiB/s | 31 µs | 29 µs | 41 µs | 48 µs | 63 µs |
| 16 | 1 | randwrite | 895k | 3495 MiB/s | 18 µs | 19 µs | 27 µs | 36 µs | 53 µs |
| 16 | 16 | randwrite | 811k | 3168 MiB/s | 316 µs | 256 µs | 862 µs | 1441 µs | 2101 µs |
| 1 | 1 | randread | 195k | 760 MiB/s | 5 µs | 3 µs | 13 µs | 15 µs | 22 µs |
| 1 | 16 | randread | 641k | 2504 MiB/s | 25 µs | 25 µs | 32 µs | 40 µs | 55 µs |
| 16 | 1 | randread | 2289k | 8943 MiB/s | 7 µs | 5 µs | 17 µs | 37 µs | 110 µs |
| 16 | 16 | randread | 5576k | 21782 MiB/s | 46 µs | 47 µs | 71 µs | 86 µs | 115 µs |

**Best throughput** (`numjobs=16 iodepth=16 randread`): 5576k IOPS, 21.3 GiB/s.

**Best latency** (`numjobs=1 iodepth=1`): 3-4 µs p50 for both read and write.

**Note on batching**: The default `Options::ioUringFlushThreshold = 64` defers `io_uring_submit` until the SQ ring has accumulated enough work to amortize the syscall -- the right trade for network/HTTP/S3 workloads where completion latency dwarfs the few-µs batching delay (see net-perf below for the resulting p99 win). On tmpfs the kernel completes reads inline at submit time, so any deferral pushes submissions off the inline-completion fast path. `file-perf` therefore initializes the scheduler with `ioUringFlushThreshold = 1`, equivalent to per-fiber submit. Measured under the default threshold (64), `16/1 randread` lands at ~1.6M IOPS and `16/16 randread` at ~4.2M -- the override recovers full throughput without any kernel or scheduler change.

---

## fio comparison (io_uring, /dev/shm, bs=4k, size=1 GiB)

| numjobs | iodepth | mode | IOPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | randwrite | 62k | 242 MiB/s | 14 µs | 14 µs | 18 µs | 27 µs | 40 µs |
| 1 | 16 | randwrite | 790k | 3084 MiB/s | 19 µs | 19 µs | 23 µs | 26 µs | 32 µs |
| 16 | 1 | randwrite | 697k | 2721 MiB/s | 21 µs | 21 µs | 29 µs | 37 µs | 50 µs |
| 16 | 16 | randwrite | 756k | 2955 MiB/s | 336 µs | 317 µs | 375 µs | 2146 µs | 5407 µs |
| 1 | 1 | randread | 65k | 254 MiB/s | 14 µs | 13 µs | 19 µs | 28 µs | 40 µs |
| 1 | 16 | randread | 903k | 3526 MiB/s | 17 µs | 16 µs | 24 µs | 32 µs | 50 µs |
| 16 | 1 | randread | 1177k | 4597 MiB/s | 12 µs | 12 µs | 22 µs | 35 µs | 47 µs |
| 16 | 16 | randread | 10242k | 40009 MiB/s | 24 µs | 20 µs | 40 µs | 82 µs | 111 µs |

At `iodepth=1`, the fiber scheduler outperforms fio (2-3x): fio uses one OS thread per job, so each IO incurs a full OS scheduler round-trip. At `iodepth=16`, fio batches all SQEs the worker enqueued into one submit per round-trip and wins; the fiber scheduler with batching disabled (file-perf opts out -- see file-perf note above) does the same per fiber but pays additional dispatch overhead for the IoFuture ring.

| config | fiber IOPS | fio IOPS | ratio |
|---|---|---|---|
| 1 job, iodepth=1, randread | 195k | 65k | 3.0x |
| 16 jobs, iodepth=1, randread | 2289k | 1177k | 1.94x |
| 1 job, iodepth=16, randread | 641k | 903k | 0.71x |
| 16 jobs, iodepth=16, randread | 5576k | 10242k | 0.54x |

---

## net-perf -- TCP echo

Loopback TCP, 64 B messages, 60 s measurement, 10 s warmup. Socket I/O uses `FiberScheduler::read`/`write` (io_uring `IORING_OP_READV`/`IORING_OP_WRITEV`); the fiber suspends inside the call until the CQE arrives. Latency is measured end-to-end: client send -> server echo -> client receive.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 38k | 2 MiB/s | 26 µs | 27 µs | 33 µs | 38 µs | 48 µs |
| 256 | 1961k | 120 MiB/s | 131 µs | 111 µs | 331 µs | 355 µs | 377 µs |
| 512 | 2140k | 131 MiB/s | 239 µs | 220 µs | 435 µs | 468 µs | 637 µs |
| 1024 | 2210k | 135 MiB/s | 463 µs | 296 µs | 2079 µs | 2146 µs | 2214 µs |

Throughput scales from ~2M req/s at 256 conns to ~2.2M at 1024 conns. The distribution flattens at 1024 conns where the SQ batch saturates: p50 drops to 296 µs while p95-p99.9 cluster tightly around 2.1 ms. Submission is bounded-batched at the dispatch boundary (see Latency profiler below); without that bound, this workload's p50 is lower but p95/p99/p99.9 inflate by 5-10x.

---

## net-perf-asio -- TCP echo (Boost.Asio C++20 coroutines)

Same workload as net-perf above, reimplemented with Boost.Asio C++20 coroutines (`asio::awaitable<void>`) and epoll (Asio's default Linux backend). Server and client use one thread per available CPU (respecting `taskset`). Reproduced with `./bb -b release net-perf-asio --duration 60s --warmup 10s`.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 3k | 0 MiB/s | 314 µs | 349 µs | 515 µs | 611 µs | 739 µs |
| 256 | 339k | 21 MiB/s | 755 µs | 771 µs | 837 µs | 862 µs | 895 µs |
| 512 | 358k | 22 MiB/s | 1429 µs | 1437 µs | 1506 µs | 1535 µs | 1572 µs |
| 1024 | 427k | 26 MiB/s | 2396 µs | 2401 µs | 2492 µs | 2529 µs | 2583 µs |

**Comparison with net-perf (fibers + io_uring), measured in the same suite:**

| connections | net-perf RPS | net-perf-asio RPS | ratio |
|---|---|---|---|
| 1 | 38k | 3k | **~13x** |
| 256 | 1961k | 339k | **~5.8x** |
| 512 | 2140k | 358k | **~6.0x** |
| 1024 | 2210k | 427k | **~5.2x** |

Two structural differences explain most of the gap. First, net-perf uses io_uring for all socket I/O while Asio uses epoll; io_uring avoids the per-operation `epoll_ctl` + `epoll_wait` + `recv`/`send` syscall chain. Second, the fiber scheduler's per-CPU pinned scheduler threads pick up completions via `io_uring_enter`, while Asio's reactor threads block in `epoll_wait` and resume via a pthread wakeup.

The gap is largest at 1 connection (~13x) where per-operation scheduling overhead dominates with no parallelism to hide it, and stays around 5-6x at high connection counts where the server CPU half is the bottleneck.

---

## net-perf-epoll -- TCP echo (raw epoll, multi-threaded)

Same workload as net-perf above, reimplemented as the simplest efficient epoll loop: edge-triggered `recv`/`send` per connection, one worker thread per available CPU (auto-detected via `silk::getAvailableProcessorCount`), `SO_REUSEPORT` listener per worker on the server, no fibers, no io_uring. Each worker owns its epoll instance and round-robins its connections through a per-fd state machine. Reproduced with `./bb -b release net-perf-epoll --duration 60s --warmup 10s`.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 44k | 3 MiB/s | 23 µs | 25 µs | 29 µs | 35 µs | 42 µs |
| 256 | 2478k | 151 MiB/s | 103 µs | 102 µs | 138 µs | 152 µs | 169 µs |
| 512 | 2506k | 153 MiB/s | 204 µs | 205 µs | 273 µs | 303 µs | 338 µs |
| 1024 | 2485k | 152 MiB/s | 412 µs | 398 µs | 515 µs | 546 µs | 714 µs |

**Comparison with net-perf (fibers + io_uring), same-run measurements:**

| connections | net-perf RPS | net-perf-epoll RPS | RPS ratio | net-perf p99 | net-perf-epoll p99 | p99 ratio |
|---|---|---|---|---|---|---|
| 1 | 38k | 44k | 1.16x | 38 µs | 35 µs | 0.92x |
| 256 | 1961k | 2478k | **1.26x** | 355 µs | 152 µs | **0.43x** |
| 512 | 2140k | 2506k | **1.17x** | 468 µs | 303 µs | **0.65x** |
| 1024 | 2210k | 2485k | **1.12x** | 2146 µs | 546 µs | **0.25x** |

At 1 connection both are equivalent -- the host has spare CPU and engine overhead is invisible. Past saturation raw epoll wins 13-30% on throughput and 1.5-2.2x on p99 tail latency. Per-CPU rate at saturation (256 conns, 16 server CPUs): fibers ≈ 122k req/cpu (8.2 µs CPU/req), epoll ≈ 158k req/cpu (6.3 µs CPU/req); the 1.9 µs/req gap is the cost of the fiber abstraction in this workload -- fiber suspend/resume + io_uring SQE/CQE submission + ready-queue bookkeeping per round-trip. The epoll loop services its connections in round-robin within each worker, so per-connection treatment is uniform -- p99 stays close to p50 (548 µs vs 401 µs at 1024 conns); net-perf with bounded-batched submission also keeps a tight ratio (861 µs p99 vs 447 µs p50, ~1.9x).

What raw epoll gives up: composability. The state machine can't naturally accommodate sleeps (no `--delay` support), multi-step protocols, or branching control flow without growing into a small interpreter. net-perf-epoll is the throughput floor; net-perf is the structure you'd actually program against.

---

## http-perf -- HTTP/1.1 GET

nginx `return 200` (empty body), loopback, 60 s measurement, 10 s warmup. Client and server pinned to separate CPU halves (16 CPUs each). Fiber client uses `FiberSocketImpl` backed by `FiberScheduler::read`/`write` (io_uring `IORING_OP_READV`/`IORING_OP_WRITEV`).

| connections | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| 1 | 37k | 27 µs | 25 µs | 33 µs | 41 µs | 71 µs |
| 256 | 1447k | 177 µs | 113 µs | 822 µs | 976 µs | 1078 µs |
| 512 | 1448k | 354 µs | 103 µs | 3712 µs | 4340 µs | 4524 µs |
| 1024 | 1426k | 718 µs | 101 µs | 8943 µs | 10382 µs | 10805 µs |

At 1 connection p50 ~25 µs reflects Poco's HTTP parsing overhead. At higher concurrency the fiber client saturates nginx at ~1.4-1.47M RPS. Tail behavior is dominated by nginx itself once `connections >= 256`.

### Server: internal (silk fibers) vs nginx

**Not a production HTTP server.** `http-perf server` is benchmark scaffolding: each accepted connection runs Poco's stock `HTTPServerConnection::run` on a fiber over `FiberSocketImpl`. Poco's HTTP server is allocation-heavy — `std::stringstream`-driven request/response parsing, per-request buffer churn even after our `MemoryPool` patches, virtual dispatch on every byte. Nobody should ship this; we use it because reusing Poco's parser on both ends gives an apples-to-apples comparison: the only thing varying between the two rows of the table below is the server's I/O loop (silk's accept fiber + per-conn fibers + io_uring read/write vs nginx's tuned C event loop). Everything else — request parsing, response building, the client — is held constant.

| connections | server | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | internal | 28k | 36 µs | 35 µs | 42 µs | 47 µs | 61 µs |
| 256 | internal | 1168k | 219 µs | 209 µs | 479 µs | 599 µs | 685 µs |
| 512 | internal | 1144k | 447 µs | 140 µs | 3846 µs | 5015 µs | 5819 µs |
| 1024 | internal | 1118k | 916 µs | 125 µs | 10630 µs | 14166 µs | 16739 µs |
| 1 | nginx | 37k | 27 µs | 25 µs | 33 µs | 41 µs | 71 µs |
| 256 | nginx | 1447k | 177 µs | 113 µs | 822 µs | 976 µs | 1078 µs |
| 512 | nginx | 1448k | 354 µs | 103 µs | 3712 µs | 4340 µs | 4524 µs |
| 1024 | nginx | 1426k | 718 µs | 101 µs | 8943 µs | 10382 µs | 10805 µs |

The internal server lands at ~80% of nginx RPS at high concurrency (1118-1168k vs 1426-1448k). The gap is Poco overhead, not silk overhead: nginx's `return 200` handler skips most of HTTP/1.1 parsing, while Poco constructs `HTTPServerRequestImpl`/`HTTPServerResponseImpl` plus heap-allocated stream buffers per request. The takeaway is that silk's accept-fiber + per-connection-fiber I/O loop has small overhead on top of whatever HTTP machinery you put on it -- to beat nginx you'd swap Poco for a hand-rolled state machine that allocates nothing per request, which is a different project.

### High-concurrency throughput (connections=10000, delay=10ms, duration=60s, warmup=10s)

Run against the internal silk-fiber HTTP server with a 10 ms server-side sleep per request, so all 10k connections stay alive simultaneously and the server CPU half is fully loaded. Reproduced with `./bb -b release http-perf [--threads] --connections 10000 --delay 10ms --duration 60s --warmup 10s`.

| connections | mode | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 10000 | fibers | 551k | 10253 µs | 10208 µs | 10535 µs | 10762 µs | 12094 µs |
| 10000 | threads | 589k | 16200 µs | 15503 µs | 23554 µs | 26398 µs | 30314 µs |

Throughput is in the same band (551k fibers vs 589k threads); the workload is server-bound. The big difference is latency tightness: fiber percentiles cluster within a 1.9 ms window (p50 10.2 ms -> p99.9 12.1 ms), while threads spread over 15 ms (p50 15.5 ms -> p99.9 30.3 ms). At 10k OS threads the kernel scheduler injects multi-millisecond stalls into the tail; the fiber scheduler keeps the tail close to the median.

---

## s3-perf -- S3 object storage

MinIO loopback (`http://127.0.0.1:9000`), object size=4096 B, 60 s measurement, 10 s warmup. Both modes use `numjobs` OS session threads, each maintaining an `iodepth`-slot ring of in-flight async S3 requests and waiting on a `FiberFuture` per slot. The difference is the AWS SDK executor and HTTP client: fiber mode runs each SDK async task as a fiber with io_uring socket I/O (`FiberExecutor` + `FiberHttpClient`); thread mode runs each task on a `PooledThreadExecutor` (sized `numjobs x iodepth`) with blocking socket I/O.

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

### Per-IO breakdown (net-perf, 1000 connections, 60 s, 10 s warmup, 2038k RPS)

Reproduced with `./bb -b release net-perf --connections 1000 --duration 60s --warmup 10s --print-counters`.

| event | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| `suspend_wait` | 159 µs | 479 µs | 982 µs | 1.0 ms |
| `io_wait` | 160 µs | 480 µs | 982 µs | 1.0 ms |
| `sq_wait` | 13 µs | 357 µs | 918 µs | 1.0 ms |
| `submit_io` | 17 µs | 125 µs | 249 µs | 262 µs |
| `cq_wait` | 27 µs | 162 µs | 540 µs | 998 µs |
| `ready_wait` | 12 µs | 132 µs | 675 µs | 1.0 ms |
| `fiber_run` | 194 ns | 403 ns | 2.4 µs | 6.7 µs |

`fiber_run` p50 = 194 ns confirms the dispatch loop itself is essentially free; this workload is IO-bound. `io_wait` is the full silk-side wait; `sq_wait` is the share before the SQE reaches the kernel and `cq_wait` bounds the CQE-side share, so the kernel's own time is the residual. Submission amortizes over batched SQEs: 23.4 M syscalls observed during the run averaging ~12 SQEs each.

Submission is bounded by **both** a count and a time threshold. `runFiber` calls `submitIo(false)` after each fiber, which fires the syscall when either (a) the SQ ring holds at least `ioUringFlushThreshold = 64` SQEs, or (b) `ioUringFlushTimeout = 100` µs has elapsed since the last submit. `handleReadyQueue`, `enqueueWakeup`, the proxy-fiber `enqueueIo` path, and worker threads call `submitIo(true)` to force-flush. The count threshold caps per-syscall cost and SQ-ring overflow; the time threshold caps the SQ_WAIT tail when a single fiber holds the scheduler thread long enough that pending SQEs from earlier fibers would otherwise wait for end-of-batch flush. SQPOLL is not enabled because the per-CPU pinned scheduler design would put the kernel poller in contention with the user-space scheduler thread.

### Profiler overhead (net-perf, 1000 connections, 60 s, 10 s warmup)

| metric | off | on | Δ |
|---|---|---|---|
| RPS | 2056k | 2038k | -0.9% |
| p50 | 178 µs | 375 µs | +111% |
| p99 | 4595 µs | 1594 µs | **-65%** |
| p99.9 | 4766 µs | 1723 µs | **-64%** |

Profiler costs ~1% RPS. Tail percentiles (p99, p99.9) are markedly lower with the profiler on -- the per-suspend TSC reads + ring writes serialize the dispatch loop in a way that suppresses the rare large-burst stalls visible in the no-profiler run. The cost shows up on the median: p50 roughly doubles (178 µs -> 375 µs) because every dispatch pays a few hundred extra cycles. The no-profiler run's p50/p99 ratio of 25x (178 µs / 4595 µs) is wider than typical steady-state; the profiler-on run's 4x ratio (375 µs / 1594 µs) is closer to the shape seen at this load in the work-stealing canonical runs.
