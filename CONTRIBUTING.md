# Contributing to Silk

## Build requirements

- CMake >= 3.28
- Ninja
- Clang 21
- ccache (optional, speeds up incremental builds)
- Boost (`libboost-dev`, `libboost-context-dev`, `libboost-program-options-dev`)

GTest, Google Benchmark, libbacktrace, liburing, and librseq are bundled as
submodules under `contrib/` and do not need to be installed separately.

Initialize the required submodules after cloning:

```
git submodule update --init --depth=1 \
    contrib/libbacktrace \
    contrib/librseq \
    contrib/liburing \
    contrib/googletest \
    contrib/benchmark
```

## Running tests

Configure and run all tests with:

```
./bb configure
./bb test
```

To run a specific test by name pattern:

```
./bb test -R FiberMutex
```

## Formatting

All source files must pass `clang-format-21`. Format in place with:

```
./bb fmt
```

Check without modifying (as CI does):

```
./bb fmt --check
```

## Pull requests

All PRs must pass every CI job before merging: `fmt`, and the full test
matrix (coverage, release, TSan, ASan, UBSan, MSan) on both amd64 and arm64.
