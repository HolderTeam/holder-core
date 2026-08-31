#if __has_include(<catch2/catch_session.hpp>)
#include <catch2/catch_session.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#else
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <windows.h>
#endif

namespace {

void ensure_test_keystore_env() {
  std::filesystem::path dir;
  if (const char* configured_dir = std::getenv("HOLDER_TEST_KEYSTORE_DIR")) {
    dir = configured_dir;
  } else {
    dir = std::filesystem::temp_directory_path() / "holder_test_keystore";
  }
#ifdef _WIN32
  const int pid = static_cast<int>(GetCurrentProcessId());
#else
  const int pid = static_cast<int>(::getpid());
#endif
  const auto started_at = std::chrono::steady_clock::now().time_since_epoch().count();
  dir /= std::to_string(pid) + "-" + std::to_string(started_at);
  std::filesystem::create_directories(dir);

#ifdef _WIN32
  _putenv_s("HOLDER_TEST_KEYSTORE_DIR", dir.string().c_str());
#else
  setenv("HOLDER_TEST_KEYSTORE_DIR", dir.string().c_str(), 1);
#endif
}

// Without this, code that resolves paths via holder::core::Paths (project registry,
// device config, etc.) falls through to the real ~/.local/share/holder,
// ~/.config/holder, ~/.cache/holder and both pollutes and races against the user's
// actual data when tests run in parallel. Setting this here rather than via CTest's
// ENVIRONMENT test property means every invocation is isolated the same way
// regardless of how the binary is run (ctest with or without a -R filter, or
// directly by a developer) — see tests/CMakeLists.txt for why the CTest property
// route doesn't work for more than one extra env var.
void ensure_test_xdg_env(const char* key, const char* leaf) {
  if (std::getenv(key)) {
    return;
  }

  std::filesystem::path dir = std::filesystem::temp_directory_path() / "holder_test_xdg";
#ifdef _WIN32
  const int pid = static_cast<int>(GetCurrentProcessId());
#else
  const int pid = static_cast<int>(::getpid());
#endif
  const auto started_at = std::chrono::steady_clock::now().time_since_epoch().count();
  dir /= std::to_string(pid) + "-" + std::to_string(started_at);
  dir /= leaf;
  std::filesystem::create_directories(dir);

#ifdef _WIN32
  _putenv_s(key, dir.string().c_str());
#else
  setenv(key, dir.string().c_str(), 1);
#endif
}

#ifdef _WIN32
void suppress_windows_error_dialogs() {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#ifdef _MSC_VER
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
}
#endif

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
  suppress_windows_error_dialogs();
#endif
  ensure_test_keystore_env();
  ensure_test_xdg_env("XDG_DATA_HOME", "data");
  ensure_test_xdg_env("XDG_CONFIG_HOME", "config");
  ensure_test_xdg_env("XDG_CACHE_HOME", "cache");
  return Catch::Session().run(argc, argv);
}
#else
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#endif
