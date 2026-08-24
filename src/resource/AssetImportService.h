#pragma once

#include "git/GitOps.h"
#include "model/Resource.h"
#include "platform/Db.h"
#include "platform/Fs.h"
#include "resource/StorageProvider.h"

#include <filesystem>
#include <functional>
#include <string>

namespace holder::resource {

struct AssetImportRequest {
  std::string project_id;
  std::string card_id;
  std::string location_id;
  std::filesystem::path source_file;
  long long now = 0;
};

struct AssetImportResult {
  std::string resource_id;
  std::string asset_id;
  bool duplicate_reused = false;
};

enum class AssetImportStage {
  Staging,
  Storing,
  Committing,
};

class AssetImportService {
 public:
  AssetImportService(
      holder::platform::Db& db,
      std::filesystem::path staging_root,
      std::function<std::string()> uuid_v4,
      holder::core::Fs* fs = nullptr,
      holder::git::GitOps* git = nullptr,
      std::function<void(AssetImportStage)> on_stage = {}
  );

  AssetImportResult import_file(
      const AssetImportRequest& request,
      StorageProvider& provider
  );

  void retrieve(
      const std::string& resource_id,
      const std::string& asset_id,
      const std::string& placement_id,
      StorageProvider& provider,
      const std::filesystem::path& destination
  );

 private:
  holder::platform::Db& db_;
  std::filesystem::path staging_root_;
  std::function<std::string()> uuid_v4_;
  holder::core::Fs* fs_;
  holder::git::GitOps* git_;
  std::function<void(AssetImportStage)> on_stage_;
};

} // namespace holder::resource
