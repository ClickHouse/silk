# Performance Results

Measurements on an AWS instance (32-CPU Intel Xeon Platinum 8488C, Linux 6.17, release build `-O3`). All measurements are 60 s with a 10 s warmup.

The main tables are reproducible with `./bb -b release perf --duration 60s --warmup 10s all`. The high-concurrency rows (`net-perf` 1000 conn / `http-perf` 10000 conn / `s3-perf` 100x100), the thread client vs nginx row in `http-perf`, and the latency-profiler section need separate `./bb` invocations -- see each section.

---

## file-perf -- async file I/O

`/dev/shm` (tmpfs, in-memory), bs=4k, size=1 GiB, 60 s measurement, 10 s warmup. Uses `FiberScheduler::read`/`write` (`IORING_OP_READV` / `IORING_OP_WRITEV`). `numjobs` = concurrent worker fibers; `iodepth` = per-fiber async IO queue depth (ring of `IoFuture`s).

| numjobs | iodepth | mode | IOPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | randwrite | 165k | 644 MiB/s | 6 µs | 4 µs | 13 µs | 15 µs | 23 µs |
| 1 | 16 | randwrite | 518k | 2023 MiB/s | 31 µs | 29 µs | 41 µs | 47 µs | 62 µs |
| 16 | 1 | randwrite | 928k | 3626 MiB/s | 17 µs | 18 µs | 28 µs | 36 µs | 52 µs |
| 16 | 16 | randwrite | 829k | 3238 MiB/s | 309 µs | 259 µs | 794 µs | 1304 µs | 1883 µs |
| 1 | 1 | randread | 197k | 769 MiB/s | 5 µs | 3 µs | 13 µs | 15 µs | 22 µs |
| 1 | 16 | randread | 640k | 2501 MiB/s | 25 µs | 25 µs | 32 µs | 40 µs | 55 µs |
| 16 | 1 | randread | 2369k | 9254 MiB/s | 7 µs | 4 µs | 17 µs | 36 µs | 117 µs |
| 16 | 16 | randread | 6019k | 23513 MiB/s | 43 µs | 45 µs | 65 µs | 81 µs | 112 µs |

**Best throughput** (`numjobs=16 iodepth=16 randread`): 6019k IOPS, 23.0 GiB/s.

**Best latency** (`numjobs=1 iodepth=1`): 3-4 µs p50 for both read and write.

**Note on batching**: The default `Options::ioUringFlushThreshold = 64` defers `io_uring_submit` until the SQ ring has accumulated enough work to amortize the syscall -- the right trade for network/HTTP/S3 workloads where completion latency dwarfs the few-µs batching delay (see net-perf below for the resulting p99 win). On tmpfs the kernel completes reads inline at submit time, so any deferral pushes submissions off the inline-completion fast path. `file-perf` therefore initializes the scheduler with `ioUringFlushThreshold = 1`, equivalent to per-fiber submit. Measured under the default threshold (64), `16/1 randread` lands at ~1.6M IOPS and `16/16 randread` at ~4.2M -- the override recovers full throughput without any kernel or scheduler change.

---

## fio comparison (io_uring, /dev/shm, bs=4k, size=1 GiB)

| numjobs | iodepth | mode | IOPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | randwrite | 63k | 247 MiB/s | 14 µs | 13 µs | 17 µs | 26 µs | 38 µs |
| 1 | 16 | randwrite | 753k | 2943 MiB/s | 20 µs | 19 µs | 27 µs | 31 µs | 37 µs |
| 16 | 1 | randwrite | 719k | 2808 MiB/s | 20 µs | 20 µs | 27 µs | 34 µs | 45 µs |
| 16 | 16 | randwrite | 766k | 2991 MiB/s | 332 µs | 313 µs | 395 µs | 2040 µs | 5145 µs |
| 1 | 1 | randread | 65k | 254 MiB/s | 14 µs | 13 µs | 18 µs | 26 µs | 38 µs |
| 1 | 16 | randread | 967k | 3777 MiB/s | 16 µs | 14 µs | 23 µs | 30 µs | 46 µs |
| 16 | 1 | randread | 1196k | 4670 MiB/s | 11 µs | 13 µs | 19 µs | 32 µs | 44 µs |
| 16 | 16 | randread | 10778k | 42100 MiB/s | 23 µs | 20 µs | 38 µs | 68 µs | 102 µs |

At `iodepth=1`, the fiber scheduler outperforms fio (2-3x): fio uses one OS thread per job, so each IO incurs a full OS scheduler round-trip. At `iodepth=16`, fio batches all SQEs the worker enqueued into one submit per round-trip and wins; the fiber scheduler with batching disabled (file-perf opts out -- see file-perf note above) does the same per fiber but pays additional dispatch overhead for the IoFuture ring.

| config | fiber IOPS | fio IOPS | ratio |
|---|---|---|---|
| 1 job, iodepth=1, randread | 197k | 65k | 3.0x |
| 16 jobs, iodepth=1, randread | 2369k | 1196k | 1.98x |
| 1 job, iodepth=16, randread | 640k | 967k | 0.66x |
| 16 jobs, iodepth=16, randread | 6019k | 10778k | 0.56x |

---

## net-perf -- TCP echo

Loopback TCP, 64 B messages, 60 s measurement, 10 s warmup. Socket I/O uses `FiberScheduler::read`/`write` (io_uring `IORING_OP_READV`/`IORING_OP_WRITEV`); the fiber suspends inside the call until the CQE arrives. Latency is measured end-to-end: client send -> server echo -> client receive.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 39k | 2 MiB/s | 26 µs | 27 µs | 32 µs | 37 µs | 44 µs |
| 256 | 1951k | 119 MiB/s | 131 µs | 111 µs | 296 µs | 367 µs | 405 µs |
| 512 | 2162k | 132 MiB/s | 237 µs | 226 µs | 412 µs | 457 µs | 856 µs |
| 1024 | 2223k | 136 MiB/s | 461 µs | 447 µs | 671 µs | 861 µs | 1787 µs |

Throughput scales from ~1.95M req/s at 256 conns to ~2.22M at 1024 conns. p50 and avg track closely at all concurrency levels (447 µs vs 461 µs at 1024 conns) — the distribution is roughly unimodal; tail spikes (p99.9 1.79 ms at 1024 conns) reflect occasional stalls but stay within ~4x of the median. Submission is bounded-batched at the dispatch boundary (see Latency profiler below); without that bound, this workload's p50 is lower but p95/p99/p99.9 inflate by 5-10x.

---

## net-perf-asio -- TCP echo (Boost.Asio C++20 coroutines)

Same workload as net-perf above, reimplemented with Boost.Asio C++20 coroutines (`asio::awaitable<void>`) and epoll (Asio's default Linux backend). Server and client use one thread per available CPU (respecting `taskset`). Reproduced with `./bb -b release net-perf-asio --duration 60s --warmup 10s`.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 3k | 0 MiB/s | 300 µs | 343 µs | 491 µs | 585 µs | 711 µs |
| 256 | 377k | 23 MiB/s | 678 µs | 683 µs | 740 µs | 768 µs | 814 µs |
| 512 | 383k | 23 MiB/s | 1337 µs | 1350 µs | 1466 µs | 1496 µs | 1534 µs |
| 1024 | 380k | 23 MiB/s | 2696 µs | 2700 µs | 2782 µs | 2818 µs | 2867 µs |

**Comparison with net-perf (fibers + io_uring), measured in the same suite:**

| connections | net-perf RPS | net-perf-asio RPS | ratio |
|---|---|---|---|
| 1 | 39k | 3k | **~13x** |
| 256 | 1951k | 377k | **~5.2x** |
| 512 | 2162k | 383k | **~5.6x** |
| 1024 | 2223k | 380k | **~5.9x** |

Two structural differences explain most of the gap. First, net-perf uses io_uring for all socket I/O while Asio uses epoll; io_uring avoids the per-operation `epoll_ctl` + `epoll_wait` + `recv`/`send` syscall chain. Second, the fiber scheduler's per-CPU pinned scheduler threads pick up completions via `io_uring_enter`, while Asio's reactor threads block in `epoll_wait` and resume via a pthread wakeup.

The gap is largest at 1 connection (~13x) where per-operation scheduling overhead dominates with no parallelism to hide it, and stays around 5-6x at high connection counts where the server CPU half is the bottleneck.

---

## net-perf-epoll -- TCP echo (raw epoll, multi-threaded)

Same workload as net-perf above, reimplemented as the simplest efficient epoll loop: edge-triggered `recv`/`send` per connection, one worker thread per available CPU (auto-detected via `silk::getAvailableProcessorCount`), `SO_REUSEPORT` listener per worker on the server, no fibers, no io_uring. Each worker owns its epoll instance and round-robins its connections through a per-fd state machine. Reproduced with `./bb -b release net-perf-epoll --duration 60s --warmup 10s`.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 42k | 3 MiB/s | 24 µs | 25 µs | 29 µs | 35 µs | 41 µs |
| 256 | 2530k | 154 MiB/s | 101 µs | 98 µs | 150 µs | 169 µs | 285 µs |
| 512 | 2532k | 155 MiB/s | 202 µs | 196 µs | 265 µs | 288 µs | 914 µs |
| 1024 | 2507k | 153 MiB/s | 408 µs | 401 µs | 510 µs | 548 µs | 1972 µs |

**Comparison with net-perf (fibers + io_uring), same-run measurements:**

| connections | net-perf RPS | net-perf-epoll RPS | RPS ratio | net-perf p99 | net-perf-epoll p99 | p99 ratio |
|---|---|---|---|---|---|---|
| 1 | 39k | 42k | 1.08x | 37 µs | 35 µs | 0.95x |
| 256 | 1951k | 2530k | **1.30x** | 367 µs | 169 µs | **0.46x** |
| 512 | 2162k | 2532k | **1.17x** | 457 µs | 288 µs | **0.63x** |
| 1024 | 2223k | 2507k | **1.13x** | 861 µs | 548 µs | **0.64x** |

At 1 connection both are equivalent -- the host has spare CPU and engine overhead is invisible. Past saturation raw epoll wins 13-30% on throughput and 1.5-2.2x on p99 tail latency. Per-CPU rate at saturation (256 conns, 16 server CPUs): fibers ≈ 122k req/cpu (8.2 µs CPU/req), epoll ≈ 158k req/cpu (6.3 µs CPU/req); the 1.9 µs/req gap is the cost of the fiber abstraction in this workload -- fiber suspend/resume + io_uring SQE/CQE submission + ready-queue bookkeeping per round-trip. The epoll loop services its connections in round-robin within each worker, so per-connection treatment is uniform -- p99 stays close to p50 (548 µs vs 401 µs at 1024 conns); net-perf with bounded-batched submission also keeps a tight ratio (861 µs p99 vs 447 µs p50, ~1.9x).

What raw epoll gives up: composability. The state machine can't naturally accommodate sleeps (no `--delay` support), multi-step protocols, or branching control flow without growing into a small interpreter. net-perf-epoll is the throughput floor; net-perf is the structure you'd actually program against.

---

## http-perf -- HTTP/1.1 GET

nginx `return 200` (empty body), loopback, 60 s measurement, 10 s warmup. Client and server pinned to separate CPU halves (16 CPUs each). Fiber client uses `FiberSocketImpl` backed by `FiberScheduler::read`/`write` (io_uring `IORING_OP_READV`/`IORING_OP_WRITEV`); thread client uses one blocking OS thread per connection. The thread+nginx row is collected separately with `./bb -b release http-perf --nginx --threads --connections 1 256 512 1024 --duration 60s --warmup 10s`.

| connections | mode | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | fiber | 39k | 25 µs | 24 µs | 32 µs | 40 µs | 85 µs |
| 256 | fiber | 1340k | 191 µs | 90 µs | 1359 µs | 1867 µs | 2127 µs |
| 512 | fiber | 1341k | 382 µs | 68 µs | 4384 µs | 5225 µs | 5435 µs |
| 1024 | fiber | 1351k | 758 µs | 71 µs | 9568 µs | 11419 µs | 11718 µs |
| 1 | threads | 37k | 27 µs | 27 µs | 32 µs | 39 µs | 223 µs |
| 256 | threads | 1266k | 202 µs | 192 µs | 341 µs | 485 µs | 839 µs |
| 512 | threads | 1262k | 406 µs | 367 µs | 733 µs | 1125 µs | 1819 µs |
| 1024 | threads | 1193k | 858 µs | 842 µs | 1169 µs | 1767 µs | 3311 µs |

At 1 connection both modes are identical (~37-39k RPS, ~24-27 µs p50): baseline is Poco's HTTP parsing overhead. At higher concurrency both clients saturate nginx at ~1.2-1.35M RPS, so throughput is similar. The difference is latency: fiber p50 stays nearly flat across all concurrency levels (24-90 µs) while thread p50 grows roughly linearly with thread count, reaching ~12x worse at 1024 connections (842 µs vs 71 µs). The fiber scheduler multiplexes all connections across 16 scheduler threads with sub-microsecond context-switch cost (see `fiber_run` below); each additional OS thread adds scheduling overhead proportional to the total thread count.

### Server: internal (silk fibers) vs nginx

**Not a production HTTP server.** `http-perf server` is benchmark scaffolding: each accepted connection runs Poco's stock `HTTPServerConnection::run` on a fiber over `FiberSocketImpl`. Poco's HTTP server is allocation-heavy — `std::stringstream`-driven request/response parsing, per-request buffer churn even after our `MemoryPool` patches, virtual dispatch on every byte. Nobody should ship this; we use it because reusing Poco's parser on both ends gives an apples-to-apples comparison: the only thing varying between the two rows of the table below is the server's I/O loop (silk's accept fiber + per-conn fibers + io_uring read/write vs nginx's tuned C event loop). Everything else — request parsing, response building, the client — is held constant.

| connections | server | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | internal | 28k | 35 µs | 35 µs | 41 µs | 46 µs | 60 µs |
| 256 | internal | 1104k | 232 µs | 162 µs | 1112 µs | 1470 µs | 1815 µs |
| 512 | internal | 1093k | 468 µs | 148 µs | 4631 µs | 6136 µs | 7349 µs |
| 1024 | internal | 1044k | 981 µs | 86 µs | 11919 µs | 16406 µs | 19449 µs |
| 1 | nginx | 39k | 25 µs | 24 µs | 32 µs | 40 µs | 85 µs |
| 256 | nginx | 1340k | 191 µs | 90 µs | 1359 µs | 1867 µs | 2127 µs |
| 512 | nginx | 1341k | 382 µs | 68 µs | 4384 µs | 5225 µs | 5435 µs |
| 1024 | nginx | 1351k | 758 µs | 71 µs | 9568 µs | 11419 µs | 11718 µs |

The internal server lands at ~80% of nginx RPS at high concurrency (1044-1104k vs 1340-1351k). The gap is Poco overhead, not silk overhead: nginx's `return 200` handler skips most of HTTP/1.1 parsing, while Poco constructs `HTTPServerRequestImpl`/`HTTPServerResponseImpl` plus heap-allocated stream buffers per request. The takeaway is that silk's accept-fiber + per-connection-fiber I/O loop has small overhead on top of whatever HTTP machinery you put on it -- to beat nginx you'd swap Poco for a hand-rolled state machine that allocates nothing per request, which is a different project.

### High-concurrency throughput (connections=10000, delay=10ms, duration=60s, warmup=10s)

Run against the internal silk-fiber HTTP server with a 10 ms server-side sleep per request, so all 10k connections stay alive simultaneously and the server CPU half is fully loaded. Reproduced with `./bb -b release http-perf [--threads] --connections 10000 --delay 10ms --duration 60s --warmup 10s`.

| connections | mode | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 10000 | fibers | 575k | 10262 µs | 10225 µs | 10539 µs | 10844 µs | 12597 µs |
| 10000 | threads | 636k | 15503 µs | 12053 µs | 28940 µs | 34035 µs | 39164 µs |

Throughput is in the same band (575k fibers vs 636k threads); the workload is server-bound. The big difference is latency tightness: fiber percentiles cluster within a 2.4 ms window (p50 10.2 ms -> p99.9 12.6 ms), while threads spread over 27 ms (p50 12.1 ms -> p99.9 39.2 ms). At 10k OS threads the kernel scheduler injects multi-millisecond stalls into the tail; the fiber scheduler keeps the tail close to the median.

---

## s3-perf -- S3 object storage

MinIO loopback (`http://127.0.0.1:9000`), object size=4096 B, 60 s measurement, 10 s warmup. Both modes use `numjobs` OS session threads, each maintaining an `iodepth`-slot ring of in-flight async S3 requests and waiting on a `FiberFuture` per slot. The difference is the AWS SDK executor and HTTP client: fiber mode runs each SDK async task as a fiber with io_uring socket I/O (`FiberExecutor` + `FiberHttpClient`); thread mode runs each task on a `PooledThreadExecutor` (sized `numjobs x iodepth`) with blocking socket I/O.

| numjobs | iodepth | mode | executor | OPS/s | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | read | fibers | 1678 | 596 µs | 606 µs | 716 µs | 790 µs | 962 µs |
| 1 | 64 | read | fibers | 39731 | 1611 µs | 1578 µs | 2551 µs | 3404 µs | 4071 µs |
| 16 | 1 | read | fibers | 29230 | 547 µs | 535 µs | 669 µs | 940 µs | 1544 µs |
| 16 | 64 | read | fibers | 50060 | 20448 µs | 19910 µs | 35103 µs | 41662 µs | 49624 µs |
| 1 | 1 | write | fibers | 1559 | 641 µs | 617 µs | 777 µs | 875 µs | 1010 µs |
| 1 | 64 | write | fibers | 611 | 104642 µs | 103667 µs | 151958 µs | 177046 µs | 198348 µs |
| 16 | 1 | write | fibers | 1579 | 10132 µs | 697 µs | 61724 µs | 114861 µs | 183053 µs |
| 16 | 64 | write | fibers | 2366 | 429349 µs | 412892 µs | 710441 µs | 896133 µs | 1146916 µs |
| 1 | 1 | read | threads | 959 | 1042 µs | 1085 µs | 1290 µs | 1373 µs | 1496 µs |
| 1 | 64 | read | threads | 40324 | 1587 µs | 1549 µs | 2596 µs | 3382 µs | 4204 µs |
| 16 | 1 | read | threads | 30716 | 521 µs | 510 µs | 632 µs | 900 µs | 1491 µs |
| 16 | 64 | read | threads | 50068 | 20445 µs | 19737 µs | 35075 µs | 41884 µs | 51820 µs |
| 1 | 1 | write | threads | 1160 | 862 µs | 829 µs | 1189 µs | 1355 µs | 1516 µs |
| 1 | 64 | write | threads | 623 | 102639 µs | 101939 µs | 147443 µs | 170832 µs | 190216 µs |
| 16 | 1 | write | threads | 1318 | 12140 µs | 1012 µs | 68045 µs | 118428 µs | 188976 µs |
| 16 | 64 | write | threads | 2381 | 426606 µs | 410346 µs | 709980 µs | 869217 µs | 1067136 µs |

At `numjobs=1 iodepth=1` read, fibers deliver 1678 OPS vs 959 for threads (+75%): with one outstanding request at a time, the thread executor pays a full OS wake-up round-trip per response, while a fiber resumes inline on the scheduler thread. At higher iodepth or numjobs, MinIO becomes the bottleneck and throughput converges. Write latency blows out at high iodepth (`iodepth=64` p50 >100 ms, `16x64` p50 >400 ms) symmetrically across both executors, confirming MinIO internal serialization is the cause.

The write `16x1` p50 (697 µs fibers / 1012 µs threads) is much lower than the avg (10 ms / 12 ms) because a small fraction of requests stall behind MinIO lock contention, pulling the mean up while the median stays fast.

### High-concurrency tail latency (numjobs=100, iodepth=100, duration=60s, warmup=10s)

Reproduced with `./bb -b release s3-perf [--threads] --numjobs 100 --iodepth 100 --duration 60s --warmup 10s`.

| numjobs | iodepth | mode | executor | OPS/s | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 100 | 100 | read | fibers | 46998 | 212075 µs | 206508 µs | 261370 µs | 306935 µs | 384190 µs |
| 100 | 100 | read | threads | 45948 | 216746 µs | 210145 µs | 273958 µs | 391332 µs | 573618 µs |

At 10,000 concurrent requests (100 jobs x iodepth 100) throughput is close (~46-47k OPS) -- MinIO is fully saturated. Fibers retain a tail-latency edge: p99 is 307 ms vs 391 ms for threads (1.27x), p99.9 is 384 ms vs 574 ms (1.49x). The gap widens at higher percentiles where 10,000 OS threads stall behind kernel scheduling jitter that the fiber scheduler avoids.

---

## Latency profiler

Per-CPU profiler (opted in via `--print-counters`) emits log2 histograms for five intervals in the fiber/IO lifecycle, listed below in lifetime order. Producer is the per-CPU scheduler thread (sole producer of its SPSC ring); consumer is the same CPU's service loop, drained on every iteration.

| event | interval |
|---|---|
| `suspend_wait` | suspended -> next `enqueueReady` (blocked-on-condition latency) |
| `io_submit` | `io_uring_submit` syscall (one per dispatch batch) |
| `io_wait` | `enqueueIo` -> CQE handled (kernel IO latency) |
| `ready_wait` | `enqueueReady` -> dispatch (ready-queue dwell) |
| `fiber_run` | `switchToFiberContext` -> return (on-CPU time per slice) |

### Per-IO breakdown (net-perf, 1000 connections, 60 s, 10 s warmup, 2164k RPS)

Reproduced with `./bb -b release net-perf --connections 1000 --duration 60s --warmup 10s --print-counters`.

| event | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| `suspend_wait` | 189 µs | 396 µs | 817 µs | 1.0 ms |
| `io_submit` | 124 µs | 234 µs | 260 µs | 446 µs |
| `io_wait` | 189 µs | 396 µs | 817 µs | 1.0 ms |
| `ready_wait` | 12.2 µs | 32.4 µs | 450 µs | 797 µs |
| `fiber_run` | 193 ns | 376 ns | 1.2 µs | 5.5 µs |

`fiber_run` p50 = 193 ns confirms the dispatch loop itself remains essentially free; this workload is IO-bound. SchedulerSystemTime totals 1052 CPU-s (55% of 32 cores x 60 s); the dominant cost is `io_uring_submit`, but it now amortizes over batched SQEs: 7.7 M syscalls averaging ~42 SQEs each (vs. ~258 M per-fiber syscalls before batching). User-mode fiber work consumes 67 s (3.5%); idle time is 194 s (10%).

Submission is bounded-batched at the dispatch boundary: `runFiber` calls `submitIo(false)` after each fiber, which threshold-gates the syscall (fires when SQ ring holds at least IO_URING_FLUSH_THRESHOLD = 64 SQEs); `handleReadyQueue` and `runStealLoop` end-of-drain calls `submitIo(true)` to force-flush partial batches. The threshold caps both per-syscall cost (linear in SQE count) and SQ ring overflow. SQPOLL is not enabled because the per-CPU pinned scheduler design would put the kernel poller in contention with the user-space scheduler thread. The remaining residual cost is the kernel's per-SQE work, which is already amortized as well as user-space batching can manage.

### Profiler overhead (net-perf, 1000 connections, 60 s, 10 s warmup)

| metric | off | on | Δ |
|---|---|---|---|
| RPS | 1922k | 1856k | -3.4% |
| p50 | 210 µs | 173 µs | -18% |
| p99 | 2431 µs | 3277 µs | +35% |
| p99.9 | 2491 µs | 3474 µs | +39% |

Profiler costs ~3% RPS. p50 actually improves under the profiler (the per-suspend TSC reads + ring writes change the dispatch-loop cadence in ways that benefit the median); the cost shows up in the tail (+35-39% at p99/p99.9).
