#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "platform/Fs.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
  );
  auto dir = base / ("holder_realfs_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

TEST_CASE("RealFs round-trip write/read/stat/rename/remove", "[fs][realfs]") {
  holder::core::RealFs fs;
  const auto dir = make_temp_dir();
  const auto nested = dir / "a" / "b";
  const auto file_path = nested / "note.txt";
  const auto renamed_path = nested / "renamed.txt";
  const std::string body = "hello\nworld";

  fs.create_directories(nested);
  fs.write_file(file_path, body);

  REQUIRE(fs.exists(file_path));
  REQUIRE(fs.read_file(file_path) == body);
  REQUIRE(fs.last_write_time_seconds(file_path) > 0);

  fs.rename(file_path, renamed_path);
  REQUIRE_FALSE(fs.exists(file_path));
  REQUIRE(fs.exists(renamed_path));

  fs.remove(renamed_path);
  REQUIRE_FALSE(fs.exists(renamed_path));
}

TEST_CASE("RealFs read_file throws for missing path", "[fs][realfs]") {
  holder::core::RealFs fs;
  const auto missing = make_temp_dir() / "missing.txt";
  REQUIRE_THROWS_WITH(
      fs.read_file(missing),
      Catch::Matchers::ContainsSubstring("Failed to open for read")
  );
}

TEST_CASE("RealFs write_file throws when parent dir is missing", "[fs][realfs]") {
  holder::core::RealFs fs;
  const auto path = make_temp_dir() / "nope" / "missing" / "file.txt";
  REQUIRE_THROWS_WITH(
      fs.write_file(path, "x"),
      Catch::Matchers::ContainsSubstring("Failed to open for write")
  );
}

TEST_CASE("RealFs rename throws for missing source", "[fs][realfs]") {
  holder::core::RealFs fs;
  const auto dir = make_temp_dir();
  const auto from = dir / "from.txt";
  const auto to = dir / "to.txt";
  REQUIRE_THROWS_WITH(fs.rename(from, to), Catch::Matchers::ContainsSubstring("Failed to rename"));
}

TEST_CASE("RealFs remove throws for non-empty directory", "[fs][realfs]") {
  holder::core::RealFs fs;
  const auto dir = make_temp_dir();
  const auto non_empty_dir = dir / "non-empty";
  fs.create_directories(non_empty_dir);
  fs.write_file(non_empty_dir / "child.txt", "x");
  REQUIRE_THROWS_WITH(
      fs.remove(non_empty_dir),
      Catch::Matchers::ContainsSubstring("Failed to remove")
  );
}

TEST_CASE("RealFs last_write_time_seconds throws for missing path", "[fs][realfs]") {
  holder::core::RealFs fs;
  const auto missing = make_temp_dir() / "missing.txt";
  REQUIRE_THROWS_WITH(
      fs.last_write_time_seconds(missing),
      Catch::Matchers::ContainsSubstring("Failed to stat")
  );
}

TEST_CASE("RealFs create_directories throws when path traverses regular file", "[fs][realfs]") {
  holder::core::RealFs fs;
  const auto dir = make_temp_dir();
  const auto blocker = dir / "blocker";
  const auto child = blocker / "child";
  fs.write_file(blocker, "x");
  REQUIRE_THROWS_WITH(
      fs.create_directories(child),
      Catch::Matchers::ContainsSubstring("Failed to create dirs")
  );
}

TEST_CASE("Fs base pointer delete uses virtual destructor", "[fs][realfs]") {
  holder::core::Fs* fs = new holder::core::RealFs();
  delete fs;
}
