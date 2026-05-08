# Performance Results

Measurements on an AWS instance (32-CPU Intel Xeon Platinum 8488C, Linux 6.17, release build `-O3`).
Results are reproducible with `./bb -b release perf --file --fio --net --net-asio --net-epoll --http --http-threads --http-nginx --s3 --s3-threads`.

---

## file-perf -- async file I/O

`/dev/shm` (tmpfs, in-memory), bs=4k, size=1 GiB, 10 s measurement, 2 s warmup. Uses `FiberScheduler::read`/`write` (`IORING_OP_READV` / `IORING_OP_WRITEV`). `numjobs` = concurrent worker fibers; `iodepth` = per-fiber async IO queue depth (ring of `IoFuture`s).

| numjobs | iodepth | mode | IOPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | randwrite | 163k | 635 MiB/s | 6 µs | 4 µs | 13 µs | 16 µs | 25 µs |
| 1 | 16 | randwrite | 522k | 2041 MiB/s | 31 µs | 29 µs | 41 µs | 48 µs | 67 µs |
| 16 | 1 | randwrite | 837k | 3270 MiB/s | 19 µs | 19 µs | 30 µs | 46 µs | 290 µs |
| 16 | 16 | randwrite | 755k | 2951 MiB/s | 339 µs | 270 µs | 945 µs | 1560 µs | 2297 µs |
| 1 | 1 | randread | 195k | 761 MiB/s | 5 µs | 3 µs | 13 µs | 15 µs | 24 µs |
| 1 | 16 | randread | 638k | 2493 MiB/s | 25 µs | 26 µs | 32 µs | 42 µs | 59 µs |
| 16 | 1 | randread | 2334k | 9118 MiB/s | 7 µs | 4 µs | 16 µs | 38 µs | 158 µs |
| 16 | 16 | randread | 5919k | 23120 MiB/s | 43 µs | 46 µs | 66 µs | 83 µs | 115 µs |

**Best throughput** (`numjobs=16 iodepth=16 randread`): 5919k IOPS, 22.6 GiB/s.

**Best latency** (`numjobs=1 iodepth=1`): 3-4 µs p50 for both read and write.

`numjobs=16 iodepth=16 randwrite` shows p99 blowout from ring contention; the read equivalent stays tight because tmpfs read paths have less internal locking. At `iodepth=16`, SQEs are now batched per fiber suspension (one `io_uring_submit` per fiber run instead of one per SQE), which improves randread throughput by ~9-22% and closes the gap with fio from 0.53x to 0.68x at 1 job.

---

## fio comparison (io_uring, /dev/shm, bs=4k, size=1 GiB)

| numjobs | iodepth | mode | IOPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | randwrite | 65k | 254 MiB/s | 13 µs | 13 µs | 19 µs | 28 µs | 39 µs |
| 1 | 16 | randwrite | 803k | 3137 MiB/s | 19 µs | 18 µs | 23 µs | 25 µs | 31 µs |
| 16 | 1 | randwrite | 718k | 2803 MiB/s | 20 µs | 20 µs | 27 µs | 35 µs | 47 µs |
| 16 | 16 | randwrite | 771k | 3013 MiB/s | 329 µs | 309 µs | 449 µs | 1974 µs | 4112 µs |
| 1 | 1 | randread | 68k | 266 MiB/s | 13 µs | 12 µs | 18 µs | 29 µs | 39 µs |
| 1 | 16 | randread | 977k | 3815 MiB/s | 15 µs | 15 µs | 19 µs | 29 µs | 50 µs |
| 16 | 1 | randread | 1179k | 4607 MiB/s | 11 µs | 12 µs | 21 µs | 33 µs | 45 µs |
| 16 | 16 | randread | 11439k | 44682 MiB/s | 21 µs | 20 µs | 29 µs | 42 µs | 90 µs |

At `iodepth=1`, the fiber scheduler outperforms fio (2-3x): fio uses one OS thread per job, so each IO incurs a full OS scheduler round-trip. At `iodepth=16`, fio wins but the gap narrowed significantly with SQE batching: the fiber scheduler now batches all iodepth SQEs into one `io_uring_submit` per fiber suspension, the same principle fio uses.

| config | fiber IOPS | fio IOPS | ratio |
|---|---|---|---|
| 1 job, iodepth=1, randread | 195k | 63k | 3.1x |
| 16 jobs, iodepth=1, randread | 2334k | 1146k | 2.0x |
| 1 job, iodepth=16, randread | 638k | 939k | 0.68x |
| 16 jobs, iodepth=16, randread | 5919k | 10468k | 0.57x |

---

## net-perf -- TCP echo

Loopback TCP, 64 B messages, 10 s measurement, 2 s warmup. All socket I/O is non-blocking; fibers suspend via `FiberScheduler::poll`. Latency is measured end-to-end: client send -> server echo -> client receive.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 43k | 3 MiB/s | 23 µs | 27 µs | 32 µs | 38 µs | 49 µs |
| 256 | 1642k | 100 MiB/s | 156 µs | 144 µs | 306 µs | 327 µs | 346 µs |
| 512 | 1600k | 98 MiB/s | 320 µs | 155 µs | 2207 µs | 2285 µs | 2343 µs |
| 1024 | 1581k | 97 MiB/s | 648 µs | 177 µs | 5040 µs | 5174 µs | 5291 µs |

Throughput reaches a ceiling of ~1600k req/s by 256 connections and stays flat thereafter -- the server is fully saturated. At 256 connections p95 improved from 737 µs to 306 µs with SQE batching: when many fibers are ready simultaneously their poll SQEs land in fewer `io_uring_submit` calls, reducing kernel entry overhead. The large gap between p50 and avg reflects a bimodal distribution: most requests are served promptly but a tail stalls behind kernel scheduling.

---

## net-perf-asio -- TCP echo (Boost.Asio C++20 coroutines)

Same workload as net-perf above, reimplemented with Boost.Asio C++20 coroutines (`asio::awaitable<void>`) and epoll (Asio's default Linux backend). Server and client use one thread per available CPU (respecting `taskset`). Reproduced with `./bb -b release net-perf-asio`.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 3k | 0 MiB/s | 313 µs | 348 µs | 522 µs | 625 µs | 759 µs |
| 256 | 390k | 24 MiB/s | 657 µs | 660 µs | 730 µs | 754 µs | 854 µs |
| 512 | 414k | 25 MiB/s | 1236 µs | 1245 µs | 1312 µs | 1339 µs | 1376 µs |
| 1024 | 409k | 25 MiB/s | 2503 µs | 2651 µs | 2799 µs | 2843 µs | 3529 µs |

**Comparison with net-perf (fibers + io_uring):**

| connections | net-perf RPS | net-perf-asio RPS | ratio |
|---|---|---|---|
| 1 | 44k | 3k | **~15x** |
| 256 | 1522k | 390k | **~4x** |
| 512 | 1615k | 414k | **~4x** |
| 1024 | 1566k | 409k | **~4x** |

The gap has two structural causes. First, net-perf uses io_uring for all socket I/O while Asio uses epoll; io_uring avoids the per-operation `epoll_ctl` + `epoll_wait` + `recv`/`send` syscall chain. Second, the fiber scheduler's work-stealing threads spin and pick up completions in nanoseconds, while Asio's reactor threads sleep in `epoll_wait` and require a wakeup write + pthread wake per completion.

The gap is largest at 1 connection (~15x) where per-operation scheduling overhead dominates with no parallelism to hide it, and narrows to ~4x at high connection counts where I/O bandwidth is the bottleneck. Asio's io_uring backend (`BOOST_ASIO_HAS_IO_URING`) was also tested and performed ~2.5x worse than epoll, ruling out the I/O backend as a factor. Linking with jemalloc had no effect. The bottleneck is entirely in Asio's handler dispatch path.

---

## net-perf-epoll -- TCP echo (raw epoll, multi-threaded)

Same workload as net-perf above, reimplemented as the simplest efficient epoll loop: edge-triggered `recv`/`send` per connection, one worker thread per available CPU (auto-detected via `silk::getAvailableProcessorCount`), `SO_REUSEPORT` listener per worker on the server, no fibers, no io_uring. Each worker owns its epoll instance and round-robins its connections through a per-fd state machine. Reproduced with `./bb -b release net-perf-epoll`.

| connections | RPS | BW | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | 39k | 2 MiB/s | 25 µs | 25 µs | 30 µs | 35 µs | 45 µs |
| 256 | 2254k | 138 MiB/s | 114 µs | 101 µs | 168 µs | 190 µs | 3091 µs |
| 512 | 2260k | 138 MiB/s | 227 µs | 210 µs | 292 µs | 327 µs | 3257 µs |
| 1024 | 2203k | 134 MiB/s | 465 µs | 446 µs | 579 µs | 749 µs | 3528 µs |

**Comparison with net-perf (fibers + io_uring), same-run measurements:**

| connections | net-perf RPS | net-perf-epoll RPS | RPS ratio | net-perf p99 | net-perf-epoll p99 | p99 ratio |
|---|---|---|---|---|---|---|
| 1 | 41k | 39k | 0.95x | 39 µs | 35 µs | 0.90x |
| 256 | 1729k | 2254k | **1.30x** | 485 µs | 190 µs | **0.39x** |
| 512 | 1759k | 2260k | **1.28x** | 1512 µs | 327 µs | **0.22x** |
| 1024 | 1714k | 2203k | **1.29x** | 2404 µs | 749 µs | **0.31x** |

At 1 connection both are equivalent — the host has spare CPU and engine overhead is invisible. Past saturation (~32-64 connections) raw epoll wins ~30% on throughput and 3-5x on p99 tail latency. Per-cpu rate at saturation: fibers ≈ 110k req/cpu (9.1 µs CPU/req), epoll ≈ 145k req/cpu (6.9 µs CPU/req); the 2.2 µs/req gap is the cost of the fiber abstraction in this workload — fiber suspend/resume + io_uring SQE/CQE submission + ready-queue bookkeeping per round-trip. The tail-latency difference is a separate phenomenon: silk's scheduler is selectively unfair (some fibers run hot while others starve for thousands of requests at a time), which keeps p50 low (130 µs at 1024 conns) but inflates p99 dramatically. The epoll loop services its connections in round-robin within each worker, so per-connection treatment is uniform — p99 stays close to p50 (749 µs vs 446 µs at 1024 conns).

The gap holds across message sizes (1.3-1.6x at 64 B – 16 KiB at 256 connections), so it scales with the abstraction cost rather than amortizing over per-byte work. Disabling `USE_IO_URING_RW` (falling back to `recv`/`send` + `FiberScheduler::poll`) does not close it — io_uring on loopback is roughly a wash compared to direct syscalls in this workload.

What raw epoll gives up: composability. The state machine can't naturally accommodate sleeps (no `--delay` support), multi-step protocols, or branching control flow without growing into a small interpreter. net-perf-epoll is the throughput floor; net-perf is the structure you'd actually program against.

---

## sockperf comparison -- TCP echo

Loopback TCP, 64 B messages, 10 s measurement. sockperf uses a single-threaded epoll server; the 1-connection row is the apples-to-apples baseline. Multi-connection rows reflect sockperf's server bottleneck, not TCP cost.

| connections | IOPS | avg | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| 1 | 42k | 23.56 µs | 23.08 µs | 32.98 µs | 42.81 µs |
| 4 | 43k | 23.27 µs | 22.82 µs | 32.87 µs | 43.05 µs |
| 16 | 95k | 21.2 µs | 22.8 µs | 32.63 µs | 41.73 µs |
| 64 | 85k | 23.39 µs | 22.94 µs | 32.88 µs | 42.74 µs |

At 1 connection, net-perf and sockperf are identical (43k vs 42k IOPS, ~23-26 µs p50). The fiber scheduler adds zero overhead over raw TCP.

---

## http-perf -- HTTP/1.1 GET

nginx `return 200` (Content-Length: 0), loopback, 10 s measurement, 2 s warmup. Client and server pinned to separate CPU halves (16 CPUs each). Fiber client uses `FiberSocketImpl` backed by `FiberScheduler::read`/`write` (io_uring `IORING_OP_READV`/`IORING_OP_WRITEV`); thread client uses one blocking OS thread per connection.

| connections | mode | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | fiber | 38k | 27 µs | 24 µs | 34 µs | 40 µs | 101 µs |
| 256 | fiber | 1158k | 221 µs | 66 µs | 2033 µs | 2453 µs | 2558 µs |
| 512 | fiber | 1215k | 421 µs | 83 µs | 4773 µs | 5890 µs | 6192 µs |
| 1024 | fiber | 1224k | 836 µs | 84 µs | 10858 µs | 12690 µs | 13243 µs |
| 1 | threads | 36k | 28 µs | 27 µs | 33 µs | 39 µs | 274 µs |
| 256 | threads | 1206k | 212 µs | 207 µs | 333 µs | 459 µs | 969 µs |
| 512 | threads | 1179k | 434 µs | 426 µs | 626 µs | 855 µs | 1668 µs |
| 1024 | threads | 1149k | 891 µs | 878 µs | 1225 µs | 1784 µs | 3428 µs |

At 1 connection both modes are identical (~36-38k RPS, ~24-27 µs p50): baseline is Poco's HTTP parsing overhead. At higher concurrency both clients saturate nginx at ~1M RPS, so throughput is similar. The difference is latency: fiber p50 stays nearly flat across all concurrency levels (24-84 µs) while thread p50 grows linearly with thread count, reaching 10x worse at 1024 connections (878 µs vs 84 µs). The fiber scheduler multiplexes all connections across 16 scheduler threads with zero per-fiber context-switch cost; each additional thread adds OS scheduling overhead proportional to the total thread count.

### Server: internal (silk fibers) vs nginx

**Not a production HTTP server.** `http-perf server` is benchmark scaffolding: each accepted connection runs Poco's stock `HTTPServerConnection::run` on a fiber over `FiberSocketImpl`. Poco's HTTP server is allocation-heavy — `std::stringstream`-driven request/response parsing, per-request buffer churn even after our `MemoryPool` patches, virtual dispatch on every byte. Nobody should ship this; we use it because reusing Poco's parser on both ends gives an apples-to-apples comparison: the only thing varying between the two rows of the table below is the server's I/O loop (silk's accept fiber + per-conn fibers + io_uring read/write vs nginx's tuned C event loop). Everything else — request parsing, response building, the client — is held constant.

| connections | server | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 1 | internal | 27k | 37 µs | 36 µs | 43 µs | 49 µs | 64 µs |
| 256 | internal | 1023k | 250 µs | 152 µs | 1478 µs | 1960 µs | 2248 µs |
| 512 | internal | 1011k | 506 µs | 88 µs | 5023 µs | 5146 µs | 5277 µs |
| 1024 | internal | 964k | 1020 µs | 94 µs | 12445 µs | 16772 µs | 19661 µs |
| 1 | nginx | 36k | 28 µs | 25 µs | 35 µs | 42 µs | 97 µs |
| 256 | nginx | 1290k | 198 µs | 72 µs | 1700 µs | 2050 µs | 2137 µs |
| 512 | nginx | 1248k | 410 µs | 63 µs | 4557 µs | 5738 µs | 5904 µs |
| 1024 | nginx | 1254k | 816 µs | 58 µs | 9979 µs | 12599 µs | 13163 µs |

The internal server lands at ~80% of nginx RPS at high concurrency (964–1023k vs 1248–1290k). The gap is Poco overhead, not silk overhead: nginx's `return 200` handler skips most of HTTP/1.1 parsing, while Poco constructs `HTTPServerRequestImpl`/`HTTPServerResponseImpl` plus heap-allocated stream buffers per request. p50 latencies sit within a few µs of each other at high concurrency; tail latencies are dominated by client-side queuing in both cases. The takeaway is that silk's accept-fiber + per-connection-fiber I/O loop has negligible overhead on top of whatever HTTP machinery you put on it — to beat nginx you'd swap Poco for a hand-rolled state machine that allocates nothing per request, which is a different project.

### High-concurrency throughput (connections=10000, delay=10ms, duration=60s)

| connections | mode | RPS | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| 10000 | fibers | 814k | 12292 µs | 12232 µs | 13408 µs | 14419 µs | 25152 µs |
| 10000 | threads | 717k | 13942 µs | 13648 µs | 15790 µs | 24089 µs | 28534 µs |

At 10000 connections (10 ms synthetic delay, all 32 cores at 100%) fibers sustain 814k RPS vs 717k for threads (+13.5%). The delay keeps all 10k connections alive simultaneously, so nginx is the bottleneck rather than client throughput. Latency is consistently better across all percentiles: fiber p99.9 (25 ms) vs threads (29 ms), and fiber p99 (14 ms) vs threads (24 ms).

---

## s3-perf -- S3 object storage

MinIO loopback (`http://127.0.0.1:9000`), object size=4096 B, 10 s measurement, 2 s warmup. Both modes use `numjobs` OS session threads, each maintaining an `iodepth`-slot ring of in-flight async S3 requests and waiting on a `FiberFuture` per slot. The difference is the AWS SDK executor and HTTP client: fiber mode runs each SDK async task as a fiber with io_uring socket I/O (`FiberExecutor` + `FiberHttpClient`); thread mode runs each task on a `PooledThreadExecutor` (sized `numjobs x iodepth`) with blocking socket I/O.

| numjobs | iodepth | mode | executor | OPS/s | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | read | fibers | 1456 | 687 µs | 699 µs | 851 µs | 943 µs | 1123 µs |
| 1 | 64 | read | fibers | 38097 | 1679 µs | 1647 µs | 2691 µs | 3527 µs | 4338 µs |
| 16 | 1 | read | fibers | 27962 | 572 µs | 559 µs | 698 µs | 976 µs | 1534 µs |
| 16 | 64 | read | fibers | 47577 | 21464 µs | 20609 µs | 37777 µs | 44589 µs | 65603 µs |
| 1 | 1 | write | fibers | 1427 | 701 µs | 690 µs | 852 µs | 989 µs | 1124 µs |
| 1 | 64 | write | fibers | 598 | 105466 µs | 104739 µs | 153297 µs | 179286 µs | 196299 µs |
| 16 | 1 | write | fibers | 1459 | 10954 µs | 776 µs | 65304 µs | 115223 µs | 195529 µs |
| 16 | 64 | write | fibers | 2277 | 427224 µs | 416051 µs | 679043 µs | 902817 µs | 1278862 µs |
| 1 | 1 | read | threads | 1043 | 958 µs | 933 µs | 1270 µs | 1373 µs | 1494 µs |
| 1 | 64 | read | threads | 39572 | 1617 µs | 1583 µs | 2605 µs | 3447 µs | 4292 µs |
| 16 | 1 | read | threads | 29829 | 536 µs | 525 µs | 648 µs | 942 µs | 1529 µs |
| 16 | 64 | read | threads | 47858 | 21347 µs | 20676 µs | 36630 µs | 43711 µs | 55342 µs |
| 1 | 1 | write | threads | 1122 | 891 µs | 839 µs | 1281 µs | 1454 µs | 1608 µs |
| 1 | 64 | write | threads | 602 | 105235 µs | 105012 µs | 161543 µs | 175461 µs | 202878 µs |
| 16 | 1 | write | threads | 1330 | 12026 µs | 994 µs | 67975 µs | 114874 µs | 198232 µs |
| 16 | 64 | write | threads | 2278 | 429072 µs | 419749 µs | 679185 µs | 880016 µs | 984188 µs |

At `numjobs=1 iodepth=1` read, fibers deliver 1456 OPS vs 1043 for threads (+40%): with one outstanding request at a time, the thread executor pays a full OS wake-up round-trip per response, while a fiber resumes inline on the scheduler thread. At higher iodepth or numjobs, MinIO becomes the bottleneck and throughput converges. Write latency blows out at high iodepth (`iodepth=64` p50 >100 ms, `16x64` p50 >400 ms) symmetrically across both executors, confirming MinIO internal serialization is the cause.

The write `16x1` p50 (776 µs fibers / 994 µs threads) is much lower than the avg (11 ms / 12 ms) because a small fraction of requests stall behind MinIO lock contention, pulling the mean up while the median stays fast.

### High-concurrency tail latency (numjobs=100, iodepth=100, duration=60s)

| numjobs | iodepth | mode | executor | OPS/s | avg | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| 100 | 100 | read | fibers | 45079 | 220727 µs | 215328 µs | 282965 µs | 425493 µs | 579894 µs |
| 100 | 100 | read | threads | 44046 | 223812 µs | 217035 µs | 290433 µs | 440547 µs | 957205 µs |

At 10,000 concurrent requests (100 jobs x iodepth 100) throughput is identical (~45k OPS) -- MinIO is fully saturated. The difference is tail latency: fiber p99.9 is 580 ms vs 957 ms for threads (1.65x). p95 and p99 are close (283 ms vs 290 ms and 425 ms vs 441 ms); the gap widens at p99.9 where OS scheduler jitter under 10,000 OS threads causes occasional long stalls that the fiber scheduler avoids.
