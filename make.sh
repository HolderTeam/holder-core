#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-test}"
BUILD_TYPE="${2:-RelWithDebInfo}"
BUILD_DIR="${BUILD_DIR:-build}"
INSTALL_PREFIX="${INSTALL_PREFIX:-out/install/local}"
CCACHE_BIN=""
CCACHE_STATE="unchecked"

print_usage() {
  cat <<'EOF'
Usage:
  ./make.sh [command] [BuildType]

Commands:
  test      Configure, build libholder, and run CTest
  build     Configure and build libholder
  coverage  Build, run tests, and generate coverage reports
  install   Build and install libholder into out/install/local
  clean     Remove local build and install output
  help      Show this help

Examples:
  ./make.sh
  ./make.sh coverage
  ./make.sh build Debug
  INSTALL_PREFIX=/tmp/holder-core ./make.sh install

Environment:
  HOLDER_CCACHE  auto (default), 1 to require, or 0 to disable ccache
EOF
}

prepare_ccache() {
  local setting="${HOLDER_CCACHE:-auto}"
  local required="false"

  if [ "${CCACHE_STATE}" != "unchecked" ]; then
    return
  fi

  case "${setting}" in
    auto)
      ;;
    1|on|true)
      required="true"
      ;;
    0|off|false)
      CCACHE_STATE="disabled"
      echo "ccache: disabled by HOLDER_CCACHE=${setting}"
      return
      ;;
    *)
      echo "Invalid HOLDER_CCACHE value: ${setting} (expected auto, 1, or 0)." >&2
      exit 2
      ;;
  esac

  if CCACHE_BIN="$(command -v ccache 2>/dev/null)"; then
    CCACHE_STATE="enabled"
    echo "ccache: enabled (${CCACHE_BIN})"
    "${CCACHE_BIN}" --show-stats
    return
  fi

  CCACHE_BIN=""
  CCACHE_STATE="disabled"
  if [ "${required}" = "true" ]; then
    echo "Missing dependency: ccache is required by HOLDER_CCACHE=${setting}." >&2
    exit 1
  fi
  echo "ccache: not found; building without a compiler cache." >&2
  echo "Install it with: sudo apt install ccache" >&2
}

cmake_configure() {
  prepare_ccache
  if [ "${CCACHE_STATE}" = "enabled" ]; then
    cmake "$@" -DCMAKE_CXX_COMPILER_LAUNCHER="${CCACHE_BIN}"
  else
    # Clear a launcher cached by an earlier invocation when caching is now
    # explicitly disabled or ccache is no longer installed.
    cmake "$@" -DCMAKE_CXX_COMPILER_LAUNCHER=
  fi
}

cmake_build() {
  cmake --build "$@"
  if [ "${CCACHE_STATE}" = "enabled" ]; then
    echo "ccache statistics after build:"
    "${CCACHE_BIN}" --show-stats
  fi
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
    cmake_configure --preset windows-vcpkg-debug
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

  cmake_configure "${cmake_args[@]}"
}

build() {
  configure
  cmake_build "${BUILD_DIR}" --target holder --parallel "$(jobs)"
}

run_tests() {
  configure
  cmake_build "${BUILD_DIR}" --parallel "$(jobs)"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
}

coverage_all() {
  local build_dir="build-coverage"
  local report_dir="${build_dir}/coverage"
  local info_base="${build_dir}/coverage-base.info"
  local info_tests="${build_dir}/coverage-tests.info"
  local info_total="${build_dir}/coverage.info"
  local coverage_json="${report_dir}/coverage.json"
  local gcov_executable="gcov"

  if command -v gcov-13 >/dev/null 2>&1; then
    gcov_executable="gcov-13"
  fi

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="--coverage -O0 -g"

  cmake_build "${build_dir}" --parallel "$(jobs)"

  lcov --directory "${build_dir}" --zerocounters
  lcov --capture --initial --directory "${build_dir}" --output-file "${info_base}" \
    --ignore-errors gcov,gcov \
    --rc geninfo_unexecuted_blocks=1
  ctest --test-dir "${build_dir}" --output-on-failure
  lcov --capture --directory "${build_dir}" --output-file "${info_tests}" \
    --ignore-errors gcov,gcov \
    --rc geninfo_unexecuted_blocks=1
  lcov --add-tracefile "${info_base}" --add-tracefile "${info_tests}" --output-file "${info_total}"
  lcov --remove "${info_total}" \
    '/usr/*' \
    '*/tests/*' \
    '*/CMakeFiles/*/CompilerIdCXX/*' \
    --output-file "${info_total}"

  genhtml "${info_total}" --output-directory "${report_dir}" --title "holder core coverage"

  if command -v gcovr >/dev/null 2>&1; then
    gcovr \
      --root . \
      --object-directory "${build_dir}" \
      --filter 'src/' \
      --exclude 'tests/' \
      --gcov-executable "${gcov_executable}" \
      --gcov-ignore-errors all \
      --exclude-pattern-prefix LCOV \
      --exclude-unreachable-branches \
      --exclude-throw-branches \
      --exclude-function-lines \
      --json-pretty \
      --output "${coverage_json}"
    echo "Coverage JSON:   ${coverage_json}"
  else
    echo "Coverage JSON:   skipped (gcovr not found)" >&2
  fi
  echo "Coverage report: ${report_dir}/index.html"
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
  coverage)
    coverage_all
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
