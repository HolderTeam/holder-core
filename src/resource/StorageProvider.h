#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace holder::resource {

enum class StorageErrorCode {
  Unavailable,
  Authentication,
  Permission,
  Capacity,
  Integrity,
  Conflict,
  InvalidConfiguration,
  Transient,
};

class StorageError : public std::runtime_error {
 public:
  StorageError(StorageErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}
  StorageErrorCode code() const noexcept { return code_; }

 private:
  StorageErrorCode code_;
};

class StorageProvider {
 public:
  virtual ~StorageProvider() = default;
  virtual void put(
      const std::string& object_key,
      const std::filesystem::path& staged_file,
      long long stored_size,
      const std::string& stored_sha256
  ) = 0;
  virtual void get(
      const std::string& object_key,
      const std::filesystem::path& destination_file
  ) = 0;
  virtual bool exists(const std::string& object_key) = 0;
  virtual void remove(const std::string& object_key) = 0;
};

} // namespace holder::resource
