#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-test}"
BUILD_TYPE="${2:-RelWithDebInfo}"
BUILD_DIR="${BUILD_DIR:-build}"
INSTALL_PREFIX="${INSTALL_PREFIX:-out/install/local}"

print_usage() {
  cat <<'EOF'
Usage:
  ./make.sh [command] [BuildType]

Commands:
  test      Configure, build libholder, and run CTest
  build     Configure and build libholder
  install   Build and install libholder into out/install/local
  clean     Remove local build and install output
  help      Show this help

Examples:
  ./make.sh
  ./make.sh build Debug
  INSTALL_PREFIX=/tmp/holder-core ./make.sh install
EOF
}

jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  else
    getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1
  fi
}

is_windows_shell() {
  case "${OS:-}:$(uname -s 2>/dev/null || true)" in
    Windows_NT:*|*:MINGW*|*:MSYS*|*:CYGWIN*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

configure() {
  if is_windows_shell && [ -n "${VCPKG_ROOT:-}" ]; then
    cmake --preset windows-vcpkg-debug
    BUILD_DIR="out/build/windows-vcpkg-debug"
    return
  fi

  local -a cmake_args=(
    -S .
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  )

  if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then
    cmake_args+=(-G Ninja)
  fi

  cmake "${cmake_args[@]}"
}

build() {
  configure
  cmake --build "${BUILD_DIR}" --target holder --parallel "$(jobs)"
}

run_tests() {
  configure
  cmake --build "${BUILD_DIR}" --parallel "$(jobs)"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
}

install_core() {
  build
  cmake --install "${BUILD_DIR}" --prefix "${INSTALL_PREFIX}"
}

case "${MODE}" in
  test|"")
    run_tests
    ;;
  build)
    build
    ;;
  install)
    install_core
    ;;
  clean)
    rm -rf build out
    ;;
  help|-h|--help)
    print_usage
    ;;
  *)
    echo "Unknown command: ${MODE}" >&2
    echo >&2
    print_usage >&2
    exit 1
    ;;
esac
