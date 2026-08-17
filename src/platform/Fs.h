#pragma once

#include <filesystem>
#include <string>

namespace holder::core {

class Fs {
 public:
  virtual ~Fs() = default; // LCOV_EXCL_LINE: compiler-generated destructor alias noise in headers

  virtual bool exists(const std::filesystem::path& path) const = 0;
  virtual void create_directories(const std::filesystem::path& path) const = 0;
  virtual void rename(const std::filesystem::path& from, const std::filesystem::path& to) const = 0;
  virtual void remove(const std::filesystem::path& path) const = 0;
  virtual long long last_write_time_seconds(const std::filesystem::path& path) const = 0;
  virtual std::string read_file(const std::filesystem::path& path) const = 0;
  virtual void write_file(const std::filesystem::path& path, const std::string& content) const = 0;
};

class RealFs final : public Fs {
 public:
  bool exists(const std::filesystem::path& path) const override;
  void create_directories(const std::filesystem::path& path) const override;
  void rename(const std::filesystem::path& from, const std::filesystem::path& to) const override;
  void remove(const std::filesystem::path& path) const override;
  long long last_write_time_seconds(const std::filesystem::path& path) const override;
  std::string read_file(const std::filesystem::path& path) const override;
  void write_file(const std::filesystem::path& path, const std::string& content) const override;
};

} // namespace holder::core
