# Contributing to Silk

Silk is an open project to which you can contribute in many ways, for example, with ideas, code, or documentation.
We appreciate all efforts that help to make the project better.

Thank you!

## Legal Info

When you open your first pull request in the ClickHouse repository, a bot will ask to accept the ClickHouse Individual CLA (Contributor License Agreement).
Please review and sign it.

Optionally, to make contributions more legally binding, your employer as a legal entity may want to sign a ClickHouse Corporate CLA with ClickHouse, Inc.
If you're interested to do so, contact us at [legal@clickhouse.com](mailto:legal@clickhouse.com).

## Build requirements

See [README.md](README.md#requirements) for the full list of build dependencies.

Initialize the required submodules after cloning:

```
git submodule update --init --depth=1 \
    contrib/benchmark \
    contrib/cxxopts \
    contrib/googletest \
    contrib/libbacktrace \
    contrib/librseq \
    contrib/liburing
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
