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

The portable entrypoints used by the Holder farm are:

```bash
./make.sh build
./make.sh test
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

ccache is recommended to speed up rebuilds. `make.sh` automatically enables it
when installed; set `HOLDER_CCACHE=0` to disable it. Use `ccache --show-stats` to
inspect cache use.

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

### macOS (Homebrew)

With Xcode Command Line Tools and Homebrew installed:

```sh
brew install cmake ninja git pkg-config ccache \
  sqlite libgit2 libsodium md4c openssl@3 \
  nlohmann-json yaml-cpp spdlog catch2

./make.sh
```

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

On Fedora with GCC 16 headers, Clang 18 cannot parse the system C++ headers.
Install Fedora's current analysis tools and select them explicitly:

```sh
sudo dnf install -y clang-tools-extra
HOLDER_CLANG_TIDY=clang-tidy HOLDER_RUN_CLANG_TIDY=run-clang-tidy ./make.sh tidy
```

The repository's `.clang-tidy` enables analyzer and selected bug checks explicitly
and treats their warnings as errors. Formatting requires `clang-format-18`,
provided separately by `clang18-tools-extra` on Fedora.

The normal suite includes a bounded, deterministic malformed-manifest corpus and
concurrent storage-provider replacement checks, tagged `[stress]`. The corpus
tries every truncated prefix and 512 byte mutations of each resource/location
manifest; successful parses must round-trip to stable canonical manifests.
The provider test performs 128 replacements while an import/retrieval callback
is active and checks cleanup ownership. Run these alongside Git concurrency tests:

```sh
./build/tests/holder_core_tests '[stress],[concurrency]'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./build-san/tests/holder_core_tests '[stress],[concurrency]'
```

The second command requires a preceding ASan/UBSan build, rather than a thread
sanitizer build in `build-san`. `./make.sh san thread` includes these tests too.
These bounded checks complement sanitizers; they are not exhaustive fuzzing.

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

Set `-DHOLDER_CORE_BUILD_SHARED=ON` in a standalone CMake build to build and
install both `libholder.a` (`Holder::Core`) and the shared library
(`Holder::Shared`). On Linux the shared files are `libholder.so.0.2.0`,
`libholder.so.0`, and `libholder.so`; ABI generation `0` is independent of the
application version. An incompatible public ABI change requires a new SONAME
and runtime package name. The default remains static-only for existing source
consumers.

Tagged `v<VERSION>` releases produce static libholder SDK archives for Linux x86_64,
macOS arm64, and Windows x86_64, each in `RelWithDebInfo` and `Release`.
Each archive has `include/`, `lib/`, CMake package files, the schema and
welcome resource under `share/holder/`, and a
`libholder-manifest.json` recording the exact commit, build type, platform,
architecture, and compiler. Windows archives also carry the vcpkg runtime
DLLs and license notices in `bin/` and `share/vcpkg-licenses/`.
Windows archives also include prebuilt dependency headers, import libraries,
CMake packages, and vcpkg's CMake scripts under `vcpkg/`. Configure consumers
with `-DCMAKE_TOOLCHAIN_FILE=<sdk>/vcpkg/scripts/buildsystems/vcpkg.cmake`,
`-DVCPKG_INSTALLED_DIR=<sdk>/vcpkg/installed`,
`-DVCPKG_TARGET_TRIPLET=x64-windows`, `-DVCPKG_MANIFEST_MODE=OFF`, and
`-DVCPKG_APPLOCAL_DEPS=OFF` (the consumer supplies runtime DLLs from `bin/`).
No vcpkg executable or dependency build is required. The archive smoke test
uses these bundled files rather than the producer's dependency installation.

On Linux and macOS, the SDK uses distribution or Homebrew libraries; consumers
must install the same dependency packages listed above and provide compatible
versions. `find_package(holder CONFIG REQUIRED)` and `Holder::Core` expose the
link dependencies through CMake. A static `libholder` archive does not contain
those libraries. The project is not promising a stable C++ ABI across SDK
releases yet, so each consumer build should resolve one exact SDK revision and
verify its manifest.

The `libholder SDK release` workflow builds, tests, installs, archives, and
smoke-tests each configuration. A `v<VERSION>` tag publishes all six archives
as GitHub Release assets after every matrix job succeeds. Manual workflow runs
validate SDKs without publishing a release. Existing consumer builds continue
to use source until the separate consumer migration is implemented.

Development builds should follow the newest green core build automatically.
After `Holder core` CI succeeds for a push to `main`, the SDK workflow checks
out that exact commit and validates all six configurations. It then publishes
a prerelease named `sdk-<full-commit-SHA>` containing the six archives and
`sdk-index.json`. The index identifies the commit and core/SDK workflow runs,
and lists each archive's platform, architecture, configuration, size, download
URL, and SHA-256 checksum. Published snapshots are retained and never
overwritten by this workflow.

The lightweight `latest-green` tag points to the newest fully published green
snapshot. Failed candidates leave the previous snapshot available. Publication
is queued and serialized, and an older completed run cannot move the pointer
backward. Development consumers resolve this tag once per build or CI run:

```sh
gh api repos/HolderTeam/holder-core/git/ref/tags/latest-green --jq '.object.sha'
```

Download `sdk-index.json` from the `sdk-<resolved-SHA>` release, select the
matching archive, check its SHA-256 checksum, and verify the archive's manifest
against the resolved commit and configuration. Cache SDKs by that resolved
commit and configuration, and resolve the pointer again for a new build. A
whole Holder Framework RC or release pins an exact core version across its
components; ordinary development does not require dependency version bumps.

## Ubuntu packages

`packaging/linux/debian` builds `libholder0` and `libholder-dev` from this
repository's CMake install output. `libholder0` carries the shared runtime and
schema/resources. `libholder-dev` carries headers, the `libholder.so` linker
symlink, `libholder.a`, CMake targets, and `holder.pc` for pkg-config. The
`libholder Ubuntu packages` workflow builds and installs both packages on
Ubuntu 24.04, then compiles external static and shared consumers against them.

The manual `Upload libholder to Launchpad` workflow prepares signed source
uploads for noble and resolute. Launchpad builds each series with its own
compiler, dependencies, and hardening flags. These packages do not use the
GitHub Linux SDK archive. Check Launchpad binary build/publication results
before relying on an uploaded package.

Future non-C++ consumers, such as a C# frontend, should use a separate thin C ABI wrapper rather than binding directly to the C++ API.
