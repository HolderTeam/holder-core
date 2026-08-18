#pragma once

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "platform/Db.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace holder::test {

inline void set_env_var(const char* key, const std::string& value) {
#ifdef _WIN32
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}

inline void unset_env_var(const char* key) {
#ifdef _WIN32
  _putenv_s(key, "");
#else
  unsetenv(key);
#endif
}

class EnvGuard {
 public:
  EnvGuard(const char* key, std::string value)
      : key_(key) {
    const char* current = std::getenv(key_);
    if (current != nullptr) {
      had_old_ = true;
      old_ = current;
    }
    set_env_var(key_, value);
  }

  ~EnvGuard() {
    if (had_old_) {
      set_env_var(key_, old_);
    } else {
      unset_env_var(key_);
    }
  }

 private:
  const char* key_;
  bool had_old_ = false;
  std::string old_;
};

inline std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_core_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

inline platform::Db open_db_with_schema(const std::filesystem::path& db_path) {
  platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  return db;
}

} // namespace holder::test
