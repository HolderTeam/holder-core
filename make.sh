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
  ./make.sh [command] [args...]

Commands:
  test [BuildType]             Configure, build libholder, and run CTest
  build [BuildType]            Configure and build libholder
  coverage                    Build, run tests, and generate coverage reports
  warnings [BuildType]         Build libholder with warnings as errors
  memcheck [test-regex]         Run Valgrind memcheck tests
  san [sanitizers] [BuildType]  Run sanitizer build and tests
  tidy                        Run clang-tidy through run-clang-tidy
  install [BuildType]          Build and install libholder into out/install/local
  format                      Format C++ source, header, and test files
  format-check                Check C++ source, header, and test formatting
  clean                       Remove local build and install output
  help                        Show this help

Examples:
  ./make.sh
  ./make.sh coverage
  ./make.sh build Debug
  ./make.sh warnings Debug
  ./make.sh memcheck 'UUID'
  ./make.sh san address,undefined
  ./make.sh san thread
  HOLDER_SAN_DETECT_LEAKS=1 ./make.sh san
  INSTALL_PREFIX=/tmp/holder-core ./make.sh install

Environment:
  HOLDER_CCACHE              auto (default), 1 to require, or 0 to disable ccache
  HOLDER_CTEST_TIMEOUT       Per-test timeout for memcheck and sanitizer runs
  HOLDER_MEMCHECK_BUILD_TYPE Build type for memcheck, default Debug
  HOLDER_SAN_DETECT_LEAKS     Set to 1 to enable ASan leak detection
  HOLDER_CLANG_TIDY          Override the clang-tidy executable
  HOLDER_RUN_CLANG_TIDY      Override the run-clang-tidy executable
  HOLDER_TSAN_SUPPRESSIONS   Optional ThreadSanitizer suppression file
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
  echo "Install ccache with your package manager (see README.md for dependencies)." >&2
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
  if [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then
    printf '%s\n' "${NUMBER_OF_PROCESSORS}"
    return
  fi
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

require_tool() {
  if ! command -v "${1}" >/dev/null 2>&1; then
    echo "Missing dependency: ${1} is required for ./make.sh ${MODE}." >&2
    echo "${2:-See README.md for diagnostic tool packages.}" >&2
    exit 1
  fi
}

warnings_all() {
  local build_dir="build-warnings"
  local build_type="${1:-Debug}"

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DHOLDER_CORE_WARNINGS_AS_ERRORS=ON
  cmake_build "${build_dir}" --target holder --parallel "$(jobs)"
}

memcheck_all() {
  local build_dir="build-memcheck"
  local build_type="${HOLDER_MEMCHECK_BUILD_TYPE:-Debug}"
  local test_regex="${1:-}"
  local valgrind_bin
  local valgrind_options
  local -a test_args=()

  require_tool valgrind
  valgrind_bin="$(command -v valgrind)"
  valgrind_options="--leak-check=full --show-leak-kinds=definite,possible --errors-for-leak-kinds=definite,possible --track-origins=yes --error-exitcode=1"

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DBUILD_TESTING=ON \
    -DMEMORYCHECK_COMMAND="${valgrind_bin}" \
    -DMEMORYCHECK_COMMAND_OPTIONS="${valgrind_options}"
  cmake_build "${build_dir}" --parallel "$(jobs)"

  if [ -n "${test_regex}" ]; then
    test_args+=(-R "${test_regex}")
  fi
  ctest --test-dir "${build_dir}" -T memcheck --output-on-failure --no-tests=error \
    --timeout "${HOLDER_CTEST_TIMEOUT:-900}" "${test_args[@]}"
}

san_all() {
  local build_dir="build-san"
  local sanitizers="${1:-address}"
  local build_type="${2:-Debug}"
  local detect_leaks="${HOLDER_SAN_DETECT_LEAKS:-0}"
  local catch_discovery="ON"
  local tsan_use_setarch="OFF"
  local test_timeout="${HOLDER_CTEST_TIMEOUT:-30}"
  local test_jobs=8
  local tsan_options="halt_on_error=1:second_deadlock_stack=1"

  if [ -n "${HOLDER_TSAN_SUPPRESSIONS:-}" ]; then
    tsan_options+=":suppressions=${HOLDER_TSAN_SUPPRESSIONS}"
  fi

  case ",${sanitizers}," in
    *",thread,"*)
      catch_discovery="OFF"
      tsan_use_setarch="ON"
      test_timeout="${HOLDER_CTEST_TIMEOUT:-300}"
      test_jobs=1
      ;;
  esac

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DBUILD_TESTING=ON \
    -DCMAKE_CXX_FLAGS="-O1 -g" \
    -DHOLDER_CORE_SANITIZE="${sanitizers}" \
    -DHOLDER_CORE_CATCH_DISCOVER_TESTS="${catch_discovery}" \
    -DHOLDER_CORE_TSAN_USE_SETARCH="${tsan_use_setarch}"

  ASAN_OPTIONS="detect_leaks=${detect_leaks}:halt_on_error=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    cmake_build "${build_dir}" --parallel "$(jobs)"

  ASAN_OPTIONS="detect_leaks=${detect_leaks}:halt_on_error=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    TSAN_OPTIONS="${tsan_options}" \
    ctest --test-dir "${build_dir}" --output-on-failure --no-tests=error \
      --parallel "${test_jobs}" --timeout "${test_timeout}"
}

tidy_all() {
  local build_dir="build-tidy"
  local source_regex
  local tidy_bin="${HOLDER_CLANG_TIDY:-clang-tidy-18}"
  local tidy_runner="${HOLDER_RUN_CLANG_TIDY:-run-clang-tidy-18}"
  local gcc_version gcc_major
  local -a tidy_extra_args=()

  source_regex="^${PWD}/(src|include|tests)/.*\\.(cpp|cc|cxx|h|hpp)$"

  require_tool "${tidy_bin}" \
    "Install clang-tidy-18 or explicitly set HOLDER_CLANG_TIDY (see README.md)."
  require_tool "${tidy_runner}" \
    "Install run-clang-tidy-18 or explicitly set HOLDER_RUN_CLANG_TIDY (see README.md)."

  if command -v g++ >/dev/null 2>&1; then
    gcc_version="$(g++ -dumpfullversion -dumpversion)"
    gcc_major="${gcc_version%%.*}"
    if [ -d "/usr/include/c++/${gcc_major}" ]; then
      tidy_extra_args+=(-extra-arg="-isystem/usr/include/c++/${gcc_major}")
    fi
    if [ -d "/usr/include/x86_64-linux-gnu/c++/${gcc_major}" ]; then
      tidy_extra_args+=(-extra-arg="-isystem/usr/include/x86_64-linux-gnu/c++/${gcc_major}")
    fi
  fi

  cmake_configure -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

  "${tidy_runner}" -clang-tidy-binary "${tidy_bin}" -p "${build_dir}" \
    -quiet "${tidy_extra_args[@]}" "${source_regex}"
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
      --exclude-noncode-lines \
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

format_files() {
  local format_bin="clang-format-18"
  local mode="${1:?}"
  local format_args
  local file_list
  local status

  if ! command -v "${format_bin}" >/dev/null 2>&1; then
    echo "Missing dependency: clang-format-18 is required for ./make.sh format and format-check." >&2
    echo "Install clang-format-18 and ensure it is available on PATH." >&2
    exit 1
  fi

  case "${mode}" in
    write)
      format_args="-i"
      ;;
    check)
      format_args="--dry-run --Werror"
      ;;
    *)
      echo "Unknown format mode: ${mode}" >&2
      exit 1
      ;;
  esac

  file_list="$(mktemp)"
  if command -v rg >/dev/null 2>&1; then
    rg --files -0 src include tests -g '*.cpp' -g '*.cc' -g '*.cxx' -g '*.h' -g '*.hpp' >"${file_list}" || true
  else
    find src include tests \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.h' -o -name '*.hpp' \) -print0 >"${file_list}"
  fi

  if [ ! -s "${file_list}" ]; then
    rm -f "${file_list}"
    echo "No C++ files found to format." >&2
    return
  fi

  if xargs -0 "${format_bin}" ${format_args} <"${file_list}"; then
    status=0
  else
    status=$?
  fi
  rm -f "${file_list}"
  return "${status}"
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
  warnings)
    warnings_all "${2:-Debug}"
    ;;
  memcheck)
    memcheck_all "${2:-}"
    ;;
  san)
    san_all "${2:-address}" "${3:-Debug}"
    ;;
  tidy)
    tidy_all
    ;;
  install)
    install_core
    ;;
  format)
    format_files write
    ;;
  format-check)
    format_files check
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
