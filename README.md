<div align="center">
  <a href="https://ton.org">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="./doc/assets/logo_dark.svg">
      <img alt="TON logo" src="./doc/assets/logo_light.svg" width="200">
    </picture>
  </a>
  <h3>Reference implementation of TON Node and tools</h3>
  <hr/>
</div>

##

<p align="center">
  <a href="https://tonresear.ch">
    <img src="https://img.shields.io/badge/TON%20Research-0098EA?style=flat&logo=discourse&label=Forum&labelColor=gray" alt="Ton Research">
  </a>
  <a href="https://t.me/toncoin">
    <img src="https://img.shields.io/badge/TON%20Community-0098EA?logo=telegram&logoColor=white&style=flat" alt="Telegram Community Group">
  </a>
  <a href="https://t.me/tonblockchain">
    <img src="https://img.shields.io/badge/TON%20Foundation-0098EA?logo=telegram&logoColor=white&style=flat" alt="Telegram Foundation Group">
  </a>
  <a href="https://t.me/tondev_eng">
    <img src="https://img.shields.io/badge/chat-TONDev-0098EA?logo=telegram&logoColor=white&style=flat" alt="Telegram Community Chat">
  </a>
</p>

<p align="center">
  <a href="https://twitter.com/ton_blockchain">
    <img src="https://img.shields.io/twitter/follow/ton_blockchain" alt="Twitter Group">
  </a>
  <a href="https://answers.ton.org">
    <img src="https://img.shields.io/badge/-TON%20Overflow-FE7A16?style=flat&logo=stack-overflow&logoColor=white" alt="TON Overflow Group">
  </a>
  <a href="https://stackoverflow.com/questions/tagged/ton">
    <img src="https://img.shields.io/badge/-Stack%20Overflow-FE7A16?style=flat&logo=stack-overflow&logoColor=white" alt="Stack Overflow Group">
  </a>
</p>



Main TON monorepo, which includes the code of the node/validator, lite-client, tonlib, FunC compiler, etc.

## Native sidechain desktop benchmark

Use the benchmark wrapper from the sibling `MyLocalTonDocker` checkout for a
repeatable native-transfer capacity run.  First build this checkout as the
native CPU-specific TON base image selected by `.env.physical`; the wrapper
then builds the matching validator and load-generator images, verifies the
effective Compose configuration, captures host/container/cgroup and validator
telemetry, waits for canonical drain and proof catch-up, and writes a
timestamped result bundle.

```bash
docker build \
  --build-arg PORTABLE=0 \
  --build-arg TON_ARCH=native \
  --build-arg NINJA_JOBS=20 \
  --build-arg VCS_REF="$(git describe --always --dirty)" \
  --build-arg VCS_DATE="$(git show -s --format=%cI HEAD)" \
  --build-arg BUILD_DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  -t ghcr.io/corton-nommander/ton:max-tps-native .

cd ../MyLocalTonDocker
sudo ./run-native-benchmark.sh .env.physical
```

The tracked 24-vCPU profile assigns 18 logical CPUs to genesis and four to the
generator. Valid Cycle 1 at 4k TPS used about one generator core, and Cycle 3
still peaked below two generator cores while genesis repeatedly reached its old
16-vCPU allocation. Moving one complete SMT core pair to the validator addresses
that measured imbalance while retaining generator headroom. The validator
uses an early native-fragment cutoff plus a serialized-size reserve so it can
seal a valid partial candidate before either the collation deadline or the
10 MiB consensus limit. Raise load only through the documented staircase after
each run is valid and fully drained.

At the 6k stress point, Cycle 5 improved submission batching from 3.17 to 8.70
messages per liteserver query, but the uncapped aggregate AIMD window still
reached 1,593 messages and coincided with validator saturation and acceptance
gaps near 65 seconds. The desktop profile now sets
`NATIVE_LOAD_ADAPTIVE_MAX_CWND=768`, exactly one 64-message batch for each of 12
connections. This caps admission pressure independently of the unchanged
65,536-task proof backlog; `effective_cwnd_cap`, `clients_at_cwnd_cap`,
`cwnd_cap_limited_acks`, and the sampled peak show whether the bound is active.

Use `benchmark/run-fresh-native-cycle.sh` in the Docker checkout for iterative
source builds and deliberately fresh runs; it refuses to remove volumes unless
the resolved Compose project is exactly `mylocalton-desktop`. A reported
offered-TPS peak is not a benchmark result: only a final proof-consistent run
with zero canonical backlog and valid correctness/capacity flags should be used
as the sustained TPS figure.

Native external messages are not irreversibly removed merely because a local
Simplex session accepted a candidate: overlapping catchain sessions can later
replace that root at the same seqno. The validator tracks local accepts
reversibly and advances nonce watermarks only from shard account states
referenced by a masterchain state whose shard tops the shard client has applied.
The benchmark records this as `validator-pool-summary.json` canonical
reconciliation telemetry and requires admitted/proof totals plus pending-source
and nonce-gap counts to settle before accepting a run.

## The Open Network

__The Open Network (TON)__ is a fast, secure, scalable blockchain focused on handling _millions of transactions per second_ (TPS) with the goal of reaching hundreds of millions of blockchain users.
- To learn more about different aspects of TON blockchain and its underlying ecosystem check [documentation](https://ton.org/docs)
- To run node, validator or lite-server check [Participate section](https://ton.org/docs/participate/nodes/run-node)
- To develop decentralised apps check [Tutorials](https://docs.ton.org/v3/guidelines/smart-contracts/guidelines), [FunC docs](https://ton.org/docs/develop/func/overview) and [DApp tutorials](https://docs.ton.org/v3/guidelines/dapps/overview)
- To work on TON check [wallets](https://ton.app/wallets), [explorers](https://ton.app/explorers), [DEXes](https://ton.app/dex) and [utilities](https://ton.app/utilities)
- To interact with TON check [APIs](https://docs.ton.org/v3/guidelines/dapps/apis-sdks/overview)

## Updates flow

* **master branch** - mainnet is running on this stable branch.

    Only emergency updates, urgent updates, or updates that do not affect the main codebase (GitHub workflows / docker images / documentation) are committed directly to this branch.

* **testnet branch** - testnet is running on this branch. The branch contains a set of new updates. After testing, the testnet branch is merged into the master branch and then a new set of updates is added to testnet branch.

* **backlog** - other branches that are candidates to getting into the testnet branch in the next iteration.

Usually, the response to your pull request will indicate which section it falls into.


## "Soft" Pull Request rules

* Thou shall not merge your own PRs, at least one person should review the PR and merge it (4-eyes rule)
* Thou shall make sure that workflows are cleanly completed for your PR before considering merge

## Build TON blockchain

### Ubuntu 22.04, 24.04 (x86-64, aarch64)
Install additional system libraries
```bash
  sudo apt-get update
  sudo apt-get install -y build-essential git cmake ninja-build

  wget https://apt.llvm.org/llvm.sh
  chmod +x llvm.sh
  sudo ./llvm.sh 21 clang
```
Compile TON binaries
```bash
  cp assembly/native/build-ubuntu-shared.sh .
  chmod +x build-ubuntu-shared.sh
  ./build-ubuntu-shared.sh
```

### MacOS 11, 12 (x86-64, aarch64)
```bash
  cp assembly/native/build-macos-shared.sh .
  chmod +x build-macos-shared.sh
  ./build-macos-shared.sh
```

### Windows 10, 11, Server (x86-64)
You need to install `MS Visual Studio 2022` first.
Go to https://www.visualstudio.com/downloads/ and download `MS Visual Studio 2022 Community`.

Launch installer and select `Desktop development with C++`.
After installation, also make sure that `cmake` is globally available by adding
`C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` to the system `PATH` (adjust the path per your needs).

Open an elevated (Run as Administrator) `x86-64 Native Tools Command Prompt for VS 2022`, go to the root folder, and execute:
```bash
  copy assembly\native\build-windows-2022.bat .
  build-windows-2022.bat
```

### MSYS2 MinGW64 (x86-64)
Execute from MinGW64 shell
```bash
  cp assembly/msys2/build-mingw64.sh .
  chmod +x build-mingw64.sh
  ./build-mingw64.sh -a
```
As a result, you will get fully statically compiled TON windows binaries.

### MSYS2 UCRT64 (x86-64)
Execute from ucrt64 shell
```bash
  cp assembly/msys2/build-ucrt64.sh .
  chmod +x build-ucrt64.sh
  ./build-ucrt64.sh -a
```
As a result, you will get fully statically compiled TON windows binaries.

### Building TON to WebAssembly
Install additional system libraries on Ubuntu
```bash
  sudo apt-get update
  sudo apt-get install -y build-essential git cmake ninja-build

  wget https://apt.llvm.org/llvm.sh
  chmod +x llvm.sh
  sudo ./llvm.sh 21 clang
```
Compile TON binaries with emscripten
```bash
  cd assembly/wasm
  chmod +x fift-func-wasm-build-ubuntu.sh
  ./fift-func-wasm-build-ubuntu.sh
```

### Building TON tonlib library for Android (arm64-v8a, armeabi-v7a, x86, x86-64)
Install additional system libraries on Ubuntu
```bash
  sudo apt-get update
  sudo apt-get install -y build-essential git cmake ninja-build automake libtool texinfo autoconf libgflags-dev \
  libreadline-dev pkg-config libgsl-dev python3 python3-dev libtool autoconf
```
Compile TON tonlib library
```bash
  cp assembly/android/build-android-tonlib.sh .
  chmod +x build-android-tonlib.sh
  ./build-android-tonlib.sh
```

### TON portable binaries

Linux portable binaries are wrapped into AppImages, at the same time MacOS portable binaries are statically linked executables.
Linux and MacOS binaries are available for both x86-64 and arm64 architectures.

## Running tests

Tests are executed by running `ctest` in the build directory. See `doc/Tests.md` for more information.
