#pragma once

#include "model/Project.h"

#include <filesystem>
#include <string>

namespace holder::resource {

struct FileDigest {
  long long byte_size = 0;
  std::string sha256;
};

struct StagedAsset {
  std::string encoding;
  FileDigest plaintext;
  FileDigest stored;
};

FileDigest digest_file(const std::filesystem::path& path);

StagedAsset stage_asset_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const holder::model::Project& project,
    const std::string& resource_id,
    const std::string& asset_id
);

void recover_asset_file(
    const std::filesystem::path& stored,
    const std::filesystem::path& destination,
    const holder::model::Project& project,
    const std::string& resource_id,
    const std::string& asset_id,
    const std::string& encoding,
    const FileDigest& expected_stored,
    const FileDigest& expected_plaintext
);

} // namespace holder::resource
