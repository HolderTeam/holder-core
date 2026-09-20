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

Optional memory checks and Clang 18 analysis/formatting tools:

```sh
sudo dnf install -y valgrind libasan libubsan libtsan clang18-tools-extra
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

### Diagnostic commands

```sh
./make.sh warnings Debug         # Build libholder with warnings as errors
./make.sh memcheck               # Run tests under Valgrind
./make.sh memcheck 'UUID'         # Select CTest names with a regular expression
./make.sh san address,undefined  # Build and test with ASan and UBSan
./make.sh san thread             # Build and test with ThreadSanitizer
./make.sh tidy                   # Run clang-tidy on source and test files
```

These commands use separate `build-warnings`, `build-memcheck`, `build-san`, and
`build-tidy` directories. Warnings, memory checks, and sanitizer builds default
to Debug. Set `HOLDER_MEMCHECK_BUILD_TYPE` to change the Valgrind build type, or
pass the build type after the sanitizer list for `san`.

Valgrind reports definite and possible leaks, tracks uninitialized values, and
returns failure for detected memory errors. `HOLDER_CTEST_TIMEOUT` overrides
the per-test timeout: 900 seconds for Valgrind (including the 5 MB encryption
round trip), 300 seconds for the single ThreadSanitizer suite, and 30 seconds
for individual ASan/UBSan tests. Set
`HOLDER_SAN_DETECT_LEAKS=1` to enable ASan leak detection. On Linux, the
ThreadSanitizer suite runs through `setarch -R`, as in holder-daemon.

On Fedora 45, the uninstrumented glibc timezone code can report a race inside
`tzset_internal` during concurrent libgit2 signature creation. The narrowly scoped
`tools/tsan/glibc.supp` documents glibc's internal lock and the instrumentation
limitation. Opt in only for that report, using an absolute path because CTest
runs from the test build directory:

```sh
HOLDER_TSAN_SUPPRESSIONS="$PWD/tools/tsan/glibc.supp" ./make.sh san thread
```

Clang 18 is the project's supported/default tidy version. `./make.sh tidy`
requires `clang-tidy-18` and `run-clang-tidy-18` on `PATH`; it never automatically
selects unversioned tools or another version. Fedora's `clang18-tools-extra`
package supplies both executables. If either is missing, the command fails with
installation guidance before configuring the build.

Explicit executable overrides remain available through `HOLDER_CLANG_TIDY` and
`HOLDER_RUN_CLANG_TIDY`, for example for a Clang 18 installation outside `PATH`:

```sh
HOLDER_CLANG_TIDY=/path/to/clang-tidy-18 \
HOLDER_RUN_CLANG_TIDY=/path/to/run-clang-tidy-18 ./make.sh tidy
```

The repository's `.clang-tidy` enables analyzer and selected bug checks explicitly
and treats their warnings as errors. Formatting requires `clang-format-18`,
provided separately by `clang18-tools-extra` on Fedora.

### Moving an existing checkout between systems

Do not reuse CMake caches or compiled output from the previous OS/toolchain.
Use a fresh build directory, for example:

```sh
BUILD_DIR=out/build/fedora ./make.sh
```

Coverage and diagnostic commands use their own fixed build directories listed
above; move aside any copied versions before running them on the new system.

## Consumption model

For now, Holder consumers should build this repository from source and link `libholder` statically. The exported CMake target is:

```cmake
Holder::Core
```

The first supported consumer is `holder-daemon`, which can use `third_party/holder-core` as a submodule or a sibling checkout during local development.

This repository can install headers, `libholder`, and CMake package files for smoke testing, but the project is not promising a stable C++ ABI yet. Do not ship a separate runtime `libholder` package until there is a real external native consumer.

Future non-C++ consumers, such as a C# frontend, should use a separate thin C ABI wrapper rather than binding directly to the C++ API.
