#if __has_include(<catch2/catch_session.hpp>)
#include <catch2/catch_session.hpp>

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
  if (std::getenv("HOLDER_TEST_KEYSTORE_DIR")) {
    return;
  }

  std::filesystem::path dir = std::filesystem::temp_directory_path() / "holder_test_keystore";
#ifdef _WIN32
  const int pid = static_cast<int>(GetCurrentProcessId());
#else
  const int pid = static_cast<int>(::getpid());
#endif
  dir /= std::to_string(pid);
  std::filesystem::create_directories(dir);

#ifdef _WIN32
  _putenv_s("HOLDER_TEST_KEYSTORE_DIR", dir.string().c_str());
#else
  setenv("HOLDER_TEST_KEYSTORE_DIR", dir.string().c_str(), 1);
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
  return Catch::Session().run(argc, argv);
}
#else
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#endif
