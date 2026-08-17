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
cmake -S . -B build
cmake --build build
```

Required dependencies are intentionally core dependencies, not optional plugins:

- SQLite
- libgit2
- libsodium
- OpenSSL
- nlohmann-json
- yaml-cpp
- spdlog

On Linux and macOS, CMake currently finds SQLite, libgit2, and libsodium via `pkg-config`. On Windows, use the vcpkg packages that `holder-daemon` already uses.

## Consumption model

For now, Holder consumers should build this repository from source and link `libholder` statically. The exported CMake target is:

```cmake
Holder::Core
```

The first supported consumer is `holder-daemon`, which can use `third_party/holder-core` as a submodule or a sibling checkout during local development.

This repository can install headers, `libholder`, and CMake package files for smoke testing, but the project is not promising a stable C++ ABI yet. Do not ship a separate runtime `libholder` package until there is a real external native consumer.

Future non-C++ consumers, such as a C# frontend, should use a separate thin C ABI wrapper rather than binding directly to the C++ API.
