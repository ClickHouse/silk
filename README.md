# Silk

A cooperative fiber scheduler for Linux with per-CPU scheduler threads, io_uring integration, and topology-aware work-stealing. Fibers are lightweight stackful coroutines that suspend rather than block their OS thread, enabling high concurrency with low overhead.

## Documentation

- [`docs/scheduler.md`](docs/scheduler.md) — scheduler loop, context switching, suspension pattern, async IO, sleep cancellation, work-stealing design, and performance benchmarks
- [`docs/sync.md`](docs/sync.md) — synchronization primitives: `FiberFuture`, `FiberFutex`, `FiberMutex`, `FiberSequencer`, `FiberEvent`, `FairFiberMutex`
- [`docs/util.md`](docs/util.md) — utility library: lock-free data structures, TSC timing, memory pool, CPU topology, logging, assertions
- [`docs/perf.md`](docs/perf.md) — `net-perf` and `file-perf` benchmark results and fio comparison
- [`docs/coroutines.md`](docs/coroutines.md) — stackless coroutines vs stackful fibers: design differences and performance data
- [`src/fibers/tests/`](src/fibers/tests/) — usage examples: fiber lifecycle, futures, synchronization primitives, async IO
- [`src/gdb/fiber.py`](src/gdb/fiber.py) — GDB extension; load with `source src/gdb/fiber.py`, then use `fiber-list`, `fiber-savecontext`, `fiber-restorecontext`, `fiber-switchcontext`

## Requirements

- CMake >= 3.28
- Ninja
- Clang 21
- ccache (optional)
- Boost (`libboost-dev`, `libboost-context-dev`, `libboost-program-options-dev`)
- libelf (`libelf-dev`) — optional, required only for `src/profiler`; the profiler is silently skipped if absent.

GTest, Google Benchmark, libbacktrace, liburing, librseq, libbpf, and bpftool are bundled as submodules under `contrib/` and do not need to be installed separately. Poco, the AWS SDK, and jemalloc are built on demand via `--build-poco`, `--build-aws`, and `--build-jemalloc` passed to `configure`.

Runtime dependencies for optional benchmarks: nginx (only for `http-perf --nginx`; the default uses an internal Poco-based server built into the `http-perf` binary), fio (for `fio-perf`), and MinIO (for `s3-perf`). MinIO is downloaded automatically to `.tools/` if not in PATH; the others must be installed separately.

## Build

```
./bb [options] [command]
```

### Global options

| Option | Values | Default | Description |
|---|---|---|---|
| `-b`, `--build` | `debug`, `release` | `debug` | Build type |
| `-s`, `--sanitizer` | `thread`, `address`, `memory`, `undefined` | | Enable sanitizer |
| `-v`, `--verbose` | | | Print every command before running it; also passes `--verbose` to perf binaries to enable their debug logging |

### Commands

#### `configure [--build-poco] [--build-aws] [--build-jemalloc]`

Configure (or reconfigure) the CMake build directory. Optional flags enable components that are off by default: `--build-poco` enables `http-perf` (requires Poco), `--build-aws` enables `s3-perf` (requires the AWS SDK), `--build-jemalloc` enables jemalloc (used by `http-perf` and `s3-perf` to improve allocator performance).

```
./bb configure
./bb configure --build-poco --build-aws
./bb -b release configure
```

#### `fmt [--check]`

Format all source files with clang-format-21. Pass `--check` to verify formatting without modifying files (exits non-zero if any file would be changed).

```
./bb fmt
./bb fmt --check
```

#### `clean`

Remove the entire `build/` directory.

```
./bb clean
```

#### `build [targets]`

Build the project. Configures automatically if the build directory does not exist. `build` is the default command when none is specified.

```
./bb                          # debug build
./bb -b release               # release build
./bb -s thread                # debug build with TSan
./bb -b release -s address    # release build with ASan
./bb build fibers-test        # build a specific target
```

#### `test [-R pattern] [-N] [ctest flags...]`

Build and run tests. Runs in parallel using all available CPUs. Any extra flags are forwarded directly to `ctest`.

| Flag | Description |
|---|---|
| `-R <pattern>` | Run only tests matching the regex pattern |
| `-N` | List tests without running them |
| `--timeout SECONDS` | Per-test timeout in seconds (default: 180, 0=none) |
| `--coverage` | Instrument with coverage, run tests, and generate an HTML report, an lcov file, and a Cobertura XML report under `build/debug-coverage/` |
| `--rerun-failed` | Rerun only tests that failed in the last run |
| `--repeat until-fail:<n>` | Repeat each test up to `n` times, stopping on first failure (useful for flaky test hunting) |
| `--output-on-failure` | Print test output when a test fails |

```
./bb test
./bb test -R FiberMutex
./bb -s thread test
./bb test --rerun-failed
./bb test --coverage
```

#### `bench [-R pattern] [-N] [gbench flags...]`

Build and run benchmarks.

| Flag | Description |
|---|---|
| `-R <pattern>` | Run only benchmarks matching the pattern |
| `-N` | List benchmarks without running them |
| `--timeout SECONDS` | Per-benchmark timeout in seconds (default: 180, 0=none) |

```
./bb -b release bench
./bb -b release bench -R LockFreeQueue
```

---

## Performance commands

Each perf command builds the relevant binary and runs the benchmark, printing results as a Markdown table. Duration, warmup, and delay options accept a unit suffix (`ns`, `us`, `ms`, `s`, `m`); a bare number is interpreted as seconds. All perf commands accept `--timeout SECONDS` (per-run timeout; default: 180, 0=none).

#### `file-perf`

Async file I/O benchmark using io_uring.

| Option | Default | Description |
|---|---|---|
| `--file PATH` | `/dev/shm/file-perf.bin` | Test file path |
| `--bs SIZE` | `4k` | Block size |
| `--size SIZE` | `1g` | File size |
| `--duration DURATION` | `10` | Measurement duration |
| `--warmup DURATION` | `2` | Warmup duration |
| `--numjobs N [N ...]` | `1` | Number of parallel jobs |
| `--iodepth N [N ...]` | `16` | IO queue depth per job |
| `--rw MODE [MODE ...]` | `randread` | Access mode(s): `randread`, `randwrite`, `seqread` |
| `--flamegraph` | | Profile and generate flamegraph SVG |
| `--print-counters` | | Print perf counters after each run |

```
./bb -b release file-perf
./bb -b release file-perf --bs 64k --size 4g
./bb -b release file-perf --numjobs 1 16 --iodepth 1 16
./bb -b release file-perf --rw randread randwrite
./bb -b release file-perf --flamegraph
```

#### `fio-perf`

fio comparison using io_uring engine. Same options as `file-perf` (except `--flamegraph` and `--print-counters`). Does not build anything.

```
./bb fio-perf
./bb fio-perf --bs 64k
./bb fio-perf --numjobs 1 16 --iodepth 1 16
```

#### `net-perf`

TCP echo benchmark. Starts a local server and runs the client against it. When `--host` points to a remote host, the server is not started locally.

| Option | Default | Description |
|---|---|---|
| `--host` | `127.0.0.1` | Server host |
| `--port` | `17777` | Server port |
| `--msg-size BYTES` | `64` | Echo message size |
| `--duration DURATION` | `10` | Measurement duration |
| `--warmup DURATION` | `2` | Warmup duration |
| `--connections N [N ...]` | `1000` | Connection counts to sweep |
| `--delay DURATION` | `0` | Server-side delay per message (e.g. `1ms`, `100us`) |
| `--flamegraph` | | Profile client and generate flamegraph SVG |
| `--print-counters` | | Print perf counters after each run |

```
./bb -b release net-perf
./bb -b release net-perf --connections 1 64 256 1024
./bb -b release net-perf --delay 1ms
./bb -b release net-perf --host 10.0.0.2
./bb -b release net-perf --flamegraph
```

#### `net-perf-asio`

TCP echo benchmark using Boost.Asio C++20 coroutines. Same options as `net-perf`.

```
./bb -b release net-perf-asio
./bb -b release net-perf-asio --connections 1 64 256 1024
./bb -b release net-perf-asio --delay 1ms
./bb -b release net-perf-asio --flamegraph
```

#### `http-perf`

HTTP/1.1 GET benchmark. Defaults to silk's internal HTTP server (Poco's `HTTPServerConnection` over `FiberSocketImpl`, one fiber per connection); pass `--nginx` to run against nginx instead.

| Option | Default | Description |
|---|---|---|
| `--host` | `127.0.0.1` | Server host |
| `--port` | `18080` | Server port |
| `--duration DURATION` | `10` | Measurement duration |
| `--warmup DURATION` | `2` | Warmup duration |
| `--connections N [N ...]` | `1000` | Connection counts to sweep |
| `--delay DURATION` | `0` | Server-side per-request delay (e.g. `1ms`, `100us`); fiber server uses `silk::FiberScheduler::sleep`, nginx uses lua sleep |
| `--threads` | | Use thread-per-connection client mode instead of fibers |
| `--nginx` | | Run client against nginx instead of the internal server |
| `--flamegraph` | | Profile client and generate flamegraph SVG |
| `--print-counters` | | Print perf counters after each run |

```
./bb -b release http-perf
./bb -b release http-perf --threads
./bb -b release http-perf --nginx
./bb -b release http-perf --delay 5ms
./bb -b release http-perf --connections 1 512 1024 2048
./bb -b release http-perf --flamegraph
```

#### `s3-perf`

S3 object storage benchmark. Starts a local MinIO server (downloaded automatically to `.tools/` if not in PATH) and runs the client against it.

| Option | Default | Description |
|---|---|---|
| `--endpoint URL` | `http://127.0.0.1:9000` | S3 endpoint |
| `--bucket NAME` | `test-bucket` | S3 bucket |
| `--key NAME` | `test-object` | S3 object key |
| `--region NAME` | `us-east-1` | S3 region |
| `--access-key KEY` | `minioadmin` | S3 access key |
| `--secret-key KEY` | `minioadmin` | S3 secret key |
| `--size SIZE` | `4096` | Object size; no units = bytes (e.g. `4096`, `64k`, `1g`) |
| `--duration DURATION` | `10` | Measurement duration |
| `--warmup DURATION` | `2` | Warmup duration |
| `--numjobs N [N ...]` | `1` | Number of parallel jobs |
| `--iodepth N [N ...]` | `16` | IO queue depth per job |
| `--rw MODE [MODE ...]` | `read` | Access mode(s): `read`, `write`, `readwrite` |
| `--threads` | | Also run with thread executor |
| `--flamegraph` | | Profile first config and generate flamegraph SVG |
| `--print-counters` | | Print perf counters after each run |
| `--data-dir PATH` | `/dev/shm/minio-data` | MinIO data directory |

```
./bb -b release s3-perf
./bb -b release s3-perf --rw read write
./bb -b release s3-perf --numjobs 1 16 --iodepth 1 64
./bb -b release s3-perf --threads
./bb -b release s3-perf --flamegraph
```

#### `perf`

Run multiple perf benchmarks in one shot. Targets are positional values, listed after the options.

| Target | Description |
|---|---|
| `file` | file-perf |
| `fio` | fio comparison |
| `net` | net-perf |
| `net-asio` | net-perf-asio |
| `net-epoll` | net-perf-epoll |
| `http` | http-perf (internal server, fiber client) |
| `http-threads` | http-perf (internal server, thread client) |
| `http-nginx` | http-perf against nginx (fiber client) |
| `s3` | s3-perf (fibers) |
| `s3-threads` | s3-perf (threads) |
| `all` | run every target above |

| Option | Description |
|---|---|
| `--duration DURATION` | Override per-binary measurement duration (e.g. `60s`) |
| `--warmup DURATION` | Override per-binary warmup duration (e.g. `10s`) |
| `--timeout SECONDS` | Per-run timeout (default 180, 0 = none) |

```
./bb -b release perf file net
./bb -b release perf all
./bb -b release perf --duration 60s --warmup 10s file net net-asio
./bb -b release perf --duration 60s --warmup 10s all
```
