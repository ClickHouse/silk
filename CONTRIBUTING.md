# Contributing to Silk

## Build requirements

See [README.md](README.md#requirements) for the full list of build dependencies.

Initialize the required submodules after cloning:

```
git submodule update --init --depth=1 \
    contrib/libbacktrace \
    contrib/librseq \
    contrib/liburing \
    contrib/googletest \
    contrib/benchmark
```

To build optional components, initialize their submodules too:

```
# http-perf (requires Poco)
git submodule update --init --depth=1 contrib/poco

# s3-perf (requires AWS SDK and its nested submodule)
git submodule update --init --depth=1 contrib/aws-sdk-cpp
git submodule update --init --depth=1 --recursive contrib/aws-sdk-cpp

# jemalloc (used by http-perf and s3-perf for improved allocator performance)
git submodule update --init --depth=1 contrib/jemalloc
```

Then pass the relevant flags to `configure`:

```
./bb configure --build-poco
./bb configure --build-aws
./bb configure --build-jemalloc
./bb configure --build-poco --build-aws --build-jemalloc
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
