#include "resource/LocalDirectoryProvider.h"

#include "resource/AssetEnvelope.h"

#include <array>
#include <chrono>
#include <fstream>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace holder::resource {
namespace {

void copy_file_streaming(const std::filesystem::path& source, const std::filesystem::path& target) {
  std::ifstream input(source, std::ios::binary);
  if (!input) throw StorageError(StorageErrorCode::Unavailable, "failed to open source object");
  std::ofstream output(target, std::ios::binary | std::ios::trunc);
  if (!output) throw StorageError(StorageErrorCode::Permission, "failed to create target object");
#ifndef _WIN32
  if (::chmod(target.c_str(), S_IRUSR | S_IWUSR) != 0) {
    throw StorageError(StorageErrorCode::Permission, "failed to secure target object");
  }
#endif
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size());
    const auto count = input.gcount();
    if (count > 0) output.write(buffer.data(), count);
  }
  if (!input.eof() || !output) {
    throw StorageError(StorageErrorCode::Capacity, "failed while copying storage object");
  }
}

std::filesystem::path temporary_sibling(const std::filesystem::path& target) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return target.parent_path() /
         ("." + target.filename().string() + ".holder-tmp-" + std::to_string(nonce));
}

} // namespace

LocalDirectoryProvider::LocalDirectoryProvider(std::filesystem::path root)
    : root_(std::filesystem::absolute(std::move(root)).lexically_normal()) {
  if (root_.empty()) {
    throw StorageError(StorageErrorCode::InvalidConfiguration, "local storage root is empty");
  }
}

std::filesystem::path LocalDirectoryProvider::resolve(const std::string& object_key) const {
  const std::filesystem::path relative(object_key);
  if (object_key.empty() || relative.is_absolute()) {
    throw StorageError(StorageErrorCode::InvalidConfiguration, "invalid storage object key");
  }
  for (const auto& part : relative) {
    if (part == ".." || part == ".") {
      throw StorageError(StorageErrorCode::InvalidConfiguration, "unsafe storage object key");
    }
  }
  const auto result = (root_ / relative).lexically_normal();
  const auto relative_to_root = result.lexically_relative(root_);
  if (relative_to_root.empty() || *relative_to_root.begin() == "..") {
    throw StorageError(StorageErrorCode::InvalidConfiguration, "storage object escapes root");
  }
  return result;
}

void LocalDirectoryProvider::put(
    const std::string& object_key,
    const std::filesystem::path& staged_file,
    long long stored_size,
    const std::string& stored_sha256
) {
  const auto source_digest = digest_file(staged_file);
  if (source_digest.byte_size != stored_size || source_digest.sha256 != stored_sha256) {
    throw StorageError(StorageErrorCode::Integrity, "staged object integrity mismatch");
  }
  const auto target = resolve(object_key);
  if (std::filesystem::exists(target)) {
    const auto existing = digest_file(target);
    if (existing.byte_size == stored_size && existing.sha256 == stored_sha256) return;
    throw StorageError(StorageErrorCode::Conflict, "object key already contains different bytes");
  }
  std::filesystem::create_directories(target.parent_path());
  const auto temporary = temporary_sibling(target);
  try {
    copy_file_streaming(staged_file, temporary);
    const auto copied = digest_file(temporary);
    if (copied.byte_size != stored_size || copied.sha256 != stored_sha256) {
      throw StorageError(StorageErrorCode::Integrity, "copied object integrity mismatch");
    }
    std::filesystem::rename(temporary, target);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

void LocalDirectoryProvider::get(
    const std::string& object_key,
    const std::filesystem::path& destination_file
) {
  const auto source = resolve(object_key);
  if (!std::filesystem::is_regular_file(source)) {
    throw StorageError(StorageErrorCode::Unavailable, "storage object not found");
  }
  std::filesystem::create_directories(destination_file.parent_path());
  const auto temporary = temporary_sibling(destination_file);
  try {
    copy_file_streaming(source, temporary);
    std::filesystem::rename(temporary, destination_file);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

bool LocalDirectoryProvider::exists(const std::string& object_key) {
  return std::filesystem::is_regular_file(resolve(object_key));
}

void LocalDirectoryProvider::remove(const std::string& object_key) {
  std::error_code error;
  std::filesystem::remove(resolve(object_key), error);
  if (error) throw StorageError(StorageErrorCode::Permission, "failed to remove storage object");
}

} // namespace holder::resource
