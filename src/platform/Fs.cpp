#include "platform/Fs.h"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace holder::core {

bool RealFs::exists(const std::filesystem::path& path) const {
  return std::filesystem::exists(path);
}

void RealFs::create_directories(const std::filesystem::path& path) const {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    throw std::runtime_error("Failed to create dirs: " + path.string() + " (" + ec.message() + ")");
  }
}

void RealFs::rename(const std::filesystem::path& from, const std::filesystem::path& to) const {
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    throw std::runtime_error(
        "Failed to rename: " + from.string() + " -> " + to.string() + " (" + ec.message() + ")"
    );
  }
}

void RealFs::remove(const std::filesystem::path& path) const {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    throw std::runtime_error("Failed to remove: " + path.string() + " (" + ec.message() + ")");
  }
}

long long RealFs::last_write_time_seconds(const std::filesystem::path& path) const {
  std::error_code ec;
  const auto ftime = std::filesystem::last_write_time(path, ec);
  if (ec) {
    throw std::runtime_error("Failed to stat: " + path.string() + " (" + ec.message() + ")");
  }
  const auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
      ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now()
  );
  return sctp.time_since_epoch().count();
}

std::string RealFs::read_file(const std::filesystem::path& path) const {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("Failed to open for read: " + path.string());
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void RealFs::write_file(const std::filesystem::path& path, const std::string& content) const {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    throw std::runtime_error("Failed to open for write: " + path.string());
  }
  out << content;
}

} // namespace holder::core
