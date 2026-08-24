#pragma once

#include "resource/StorageProvider.h"

#include <filesystem>

namespace holder::resource {

class LocalDirectoryProvider final : public StorageProvider {
 public:
  explicit LocalDirectoryProvider(std::filesystem::path root);

  void put(
      const std::string& object_key,
      const std::filesystem::path& staged_file,
      long long stored_size,
      const std::string& stored_sha256
  ) override;
  void get(
      const std::string& object_key,
      const std::filesystem::path& destination_file
  ) override;
  bool exists(const std::string& object_key) override;
  void remove(const std::string& object_key) override;

 private:
  std::filesystem::path resolve(const std::string& object_key) const;
  std::filesystem::path root_;
};

} // namespace holder::resource
