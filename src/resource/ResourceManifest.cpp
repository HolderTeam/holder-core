#include "resource/ResourceManifest.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace holder::resource {
namespace {

constexpr int kManifestVersion = 1;

void require_non_empty(const std::string& value, const char* field) {
  if (value.empty()) {
    throw std::runtime_error(std::string("resource manifest missing ") + field);
  }
}

nlohmann::json placement_json(const holder::model::Placement& placement) {
  return {
      {"placement_id", placement.placement_id},
      {"location_id", placement.location_id},
      {"object_key", placement.object_key},
      {"encoding", placement.encoding},
      {"stored_byte_size", placement.stored_byte_size},
      {"stored_sha256", placement.stored_sha256},
      {"created_at", placement.created_at},
  };
}

nlohmann::json asset_json(const holder::model::Asset& asset) {
  nlohmann::json placements = nlohmann::json::array();
  for (const auto& placement : asset.placements) {
    if (placement.asset_id != asset.asset_id) {
      throw std::runtime_error("placement asset_id mismatch");
    }
    placements.push_back(placement_json(placement));
  }
  return {
      {"asset_id", asset.asset_id},
      {"original_filename", asset.original_filename},
      {"media_type", asset.media_type},
      {"byte_size", asset.byte_size},
      {"plaintext_sha256", asset.plaintext_sha256},
      {"created_at", asset.created_at},
      {"updated_at", asset.updated_at},
      {"placements", std::move(placements)},
  };
}

holder::model::Placement parse_placement(const nlohmann::json& item, const std::string& asset_id) {
  holder::model::Placement placement;
  placement.placement_id = item.at("placement_id").get<std::string>();
  placement.asset_id = asset_id;
  placement.location_id = item.at("location_id").get<std::string>();
  placement.object_key = item.at("object_key").get<std::string>();
  placement.encoding = item.at("encoding").get<std::string>();
  placement.stored_byte_size = item.at("stored_byte_size").get<long long>();
  placement.stored_sha256 = item.at("stored_sha256").get<std::string>();
  placement.created_at = item.at("created_at").get<long long>();
  require_non_empty(placement.placement_id, "placement_id");
  require_non_empty(placement.location_id, "location_id");
  require_non_empty(placement.object_key, "object_key");
  require_non_empty(placement.encoding, "encoding");
  if (placement.stored_byte_size < 0) {
    throw std::runtime_error("resource manifest has negative stored_byte_size");
  }
  return placement;
}

holder::model::Asset parse_asset(const nlohmann::json& item, const std::string& resource_id) {
  holder::model::Asset asset;
  asset.asset_id = item.at("asset_id").get<std::string>();
  asset.resource_id = resource_id;
  asset.original_filename = item.at("original_filename").get<std::string>();
  asset.media_type = item.at("media_type").get<std::string>();
  asset.byte_size = item.at("byte_size").get<long long>();
  asset.plaintext_sha256 = item.at("plaintext_sha256").get<std::string>();
  asset.created_at = item.at("created_at").get<long long>();
  asset.updated_at = item.at("updated_at").get<long long>();
  require_non_empty(asset.asset_id, "asset_id");
  require_non_empty(asset.plaintext_sha256, "plaintext_sha256");
  if (asset.byte_size < 0) throw std::runtime_error("resource manifest has negative byte_size");
  if (!item.at("placements").is_array()) {
    throw std::runtime_error("resource manifest placements must be an array");
  }
  for (const auto& placement : item.at("placements")) {
    asset.placements.push_back(parse_placement(placement, asset.asset_id));
  }
  return asset;
}

} // namespace

std::string render_resource_manifest(const holder::model::ResourceBundle& bundle) {
  const auto& resource = bundle.resource;
  require_non_empty(resource.resource_id, "resource_id");
  require_non_empty(resource.project_id, "project_id");
  require_non_empty(resource.type, "type");
  require_non_empty(resource.label, "label");

  nlohmann::json assets = nlohmann::json::array();
  for (const auto& asset : bundle.assets) {
    if (asset.resource_id != resource.resource_id) {
      throw std::runtime_error("asset resource_id mismatch");
    }
    assets.push_back(asset_json(asset));
  }

  nlohmann::json body = {
      {"format_version", kManifestVersion},
      {"resource",
       {
           {"resource_id", resource.resource_id},
           {"project_id", resource.project_id},
           {"type", resource.type},
           {"label", resource.label},
           {"metadata", resource.metadata},
           {"created_at", resource.created_at},
           {"updated_at", resource.updated_at},
       }},
      {"assets", std::move(assets)},
  };
  return body.dump(2) + "\n";
}

holder::model::ResourceBundle parse_resource_manifest(const std::string& text) {
  const auto body = nlohmann::json::parse(text);
  if (!body.is_object() || body.at("format_version").get<int>() != kManifestVersion) {
    throw std::runtime_error("unsupported resource manifest version");
  }
  const auto& item = body.at("resource");
  holder::model::ResourceBundle bundle;
  auto& resource = bundle.resource;
  resource.resource_id = item.at("resource_id").get<std::string>();
  resource.project_id = item.at("project_id").get<std::string>();
  resource.type = item.at("type").get<std::string>();
  resource.label = item.at("label").get<std::string>();
  resource.metadata = item.at("metadata").get<holder::model::ResourceMetadata>();
  resource.created_at = item.at("created_at").get<long long>();
  resource.updated_at = item.at("updated_at").get<long long>();
  require_non_empty(resource.resource_id, "resource_id");
  require_non_empty(resource.project_id, "project_id");
  require_non_empty(resource.type, "type");
  require_non_empty(resource.label, "label");

  if (!body.at("assets").is_array()) {
    throw std::runtime_error("resource manifest assets must be an array");
  }
  for (const auto& asset : body.at("assets")) {
    bundle.assets.push_back(parse_asset(asset, resource.resource_id));
  }
  return bundle;
}

std::string render_location_manifest(const holder::model::Location& location) {
  require_non_empty(location.location_id, "location_id");
  require_non_empty(location.project_id, "project_id");
  require_non_empty(location.name, "name");
  require_non_empty(location.provider, "provider");
  nlohmann::json body = {
      {"format_version", kManifestVersion},
      {"location",
       {
           {"location_id", location.location_id},
           {"project_id", location.project_id},
           {"name", location.name},
           {"provider", location.provider},
           {"configuration", location.configuration},
           {"created_at", location.created_at},
           {"updated_at", location.updated_at},
       }},
  };
  return body.dump(2) + "\n";
}

holder::model::Location parse_location_manifest(const std::string& text) {
  const auto body = nlohmann::json::parse(text);
  if (!body.is_object() || body.at("format_version").get<int>() != kManifestVersion) {
    throw std::runtime_error("unsupported location manifest version");
  }
  const auto& item = body.at("location");
  holder::model::Location location;
  location.location_id = item.at("location_id").get<std::string>();
  location.project_id = item.at("project_id").get<std::string>();
  location.name = item.at("name").get<std::string>();
  location.provider = item.at("provider").get<std::string>();
  location.configuration = item.at("configuration").get<std::map<std::string, std::string>>();
  location.created_at = item.at("created_at").get<long long>();
  location.updated_at = item.at("updated_at").get<long long>();
  require_non_empty(location.location_id, "location_id");
  require_non_empty(location.project_id, "project_id");
  require_non_empty(location.name, "name");
  require_non_empty(location.provider, "provider");
  return location;
}

} // namespace holder::resource
