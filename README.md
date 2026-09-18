# holder-core

`holder-core` is Holder's platform-independent storage and domain library.

This first cut contains the parts that should be shared by `holderd` and future non-desktop clients:

- SQLite repositories and migrations.
- Card, project, resource, sync-policy, search-index, and AI data models.
- Git repository operations built on libgit2.
- Project privacy, encryption, and secret storage abstractions.

The daemon still owns process supervision, HTTP routes, local model runners, platform startup paths, and service behavior.

## Build

```sh
./make.sh
```

That configures CMake, builds `libholder` and the core tests, and runs CTest.
Install the dependencies below first. A C++20 compiler and CMake 3.22 or newer
are required; Ninja is recommended.

The raw CMake commands are:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Required dependencies are intentionally core dependencies, not optional plugins:

- SQLite
- libgit2
- libsodium
- md4c
- OpenSSL
- nlohmann-json
- yaml-cpp
- spdlog

On Linux and macOS, CMake currently finds SQLite, libgit2, libsodium, and md4c via `pkg-config`. On Windows and Android, use the vcpkg packages that `holder-daemon` already uses.

Catch2 is also required by the default configuration (`BUILD_TESTING=ON`). For
a library-only build, configure with `-DBUILD_TESTING=OFF`. On Linux, optional
libsecret development files enable desktop keyring integration; the commands
below include them.

### Fedora

```sh
sudo dnf install -y gcc-c++ cmake ninja-build git pkgconf-pkg-config \
  sqlite-devel 'pkgconfig(libgit2)' libsodium-devel md4c-devel \
  openssl-devel json-devel yaml-cpp-devel spdlog-devel catch-devel \
  libsecret-devel
```

Use the quoted `pkgconfig(libgit2)` capability because the package name varies
between Fedora releases (Fedora 45 prerelease provides `libgit2_1.9-devel`).
`json-devel` supplies nlohmann-json, and `catch-devel` supplies Catch2.

A fresh RelWithDebInfo build and all 757 CTest tests passed on Fedora 45
prerelease with GCC 16.2.1, CMake 4.3.0, OpenSSL 4.0.2, libgit2 1.9.7, and
Catch2 3.16.0. Coverage generation was not verified on this setup.

Optional compiler caching and coverage tools:

```sh
sudo dnf install -y ccache lcov gcovr
```

### Ubuntu / Debian

```sh
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config \
  libsqlite3-dev libgit2-dev libsodium-dev libmd4c-dev libssl-dev \
  nlohmann-json3-dev libyaml-cpp-dev libspdlog-dev catch2 libsecret-1-dev
```

Optional compiler caching and coverage tools:

```sh
sudo apt install -y ccache lcov gcovr
```

`make.sh` automatically uses ccache when installed (`HOLDER_CCACHE=0` disables
it). `./make.sh coverage` uses GCC/gcov, lcov and genhtml; gcovr adds a JSON report.

### Moving an existing checkout between systems

Do not reuse CMake caches or compiled output from the previous OS/toolchain.
Use a fresh build directory, for example:

```sh
BUILD_DIR=out/build/fedora ./make.sh
```

The coverage command uses its own fixed `build-coverage` directory; move aside
any copied version of that directory before running coverage on the new system.

## Consumption model

For now, Holder consumers should build this repository from source and link `libholder` statically. The exported CMake target is:

```cmake
Holder::Core
```

The first supported consumer is `holder-daemon`, which can use `third_party/holder-core` as a submodule or a sibling checkout during local development.

This repository can install headers, `libholder`, and CMake package files for smoke testing, but the project is not promising a stable C++ ABI yet. Do not ship a separate runtime `libholder` package until there is a real external native consumer.

Future non-C++ consumers, such as a C# frontend, should use a separate thin C ABI wrapper rather than binding directly to the C++ API.
