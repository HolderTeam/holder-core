#pragma once
#include <sqlite3.h>

#include <filesystem>
#include <string>

namespace holder::platform {

class Db {
 public:
  Db() = default;
  ~Db();

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  Db(Db&& other) noexcept;
  Db& operator=(Db&& other) noexcept;

  // Opens (and creates if missing) the DB file.
  void open(const std::filesystem::path& path);

  // Close explicitly (optional; destructor does it).
  void close();

  // Execute a SQL string (may contain multiple statements).
  void exec(const std::string& sql);

  // Convenience: current DB path, if opened.
  const std::filesystem::path& path() const { return path_; }

  // Expose raw handle when needed.
  sqlite3* handle() const { return db_; }

 private:
  static void throw_sqlite(const std::string& what, int rc, sqlite3* db);

  sqlite3* db_ = nullptr;
  std::filesystem::path path_;
};

} // namespace holder::platform
