#include "resource/LocationBindingStore.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace holder::resource {
namespace {

constexpr const char* kBindingService = "org.holder.StorageLocation";
constexpr const char* kPreferredService = "org.holder.PreferredStorageLocation";

std::string binding_account(const std::string& project_id, const std::string& location_id) {
  if (project_id.empty() || location_id.empty()) throw std::invalid_argument("binding ids required");
  return project_id + ":" + location_id;
}

} // namespace

LocationBindingStore::LocationBindingStore(holder::privacy::SecretStore& secrets)
    : secrets_(secrets) {}

void LocationBindingStore::bind(
    const std::string& project_id,
    const std::string& location_id,
    const LocationBinding& binding,
    const std::string& safe_preview,
    long long now
) {
  if (binding.version != 1 || binding.provider.empty()) {
    throw std::invalid_argument("invalid location binding");
  }
  const auto account = binding_account(project_id, location_id);
  const auto existing = secrets_.get(kBindingService, account);
  const long long created_at = existing.has_value() ? existing->metadata.created_at : now;
  const nlohmann::json body = {
      {"provider", binding.provider}, {"values", binding.values}, {"version", binding.version}
  };
  secrets_.set(kBindingService, account, body.dump(), safe_preview, created_at, now);
}

std::optional<LocationBinding> LocationBindingStore::get(
    const std::string& project_id,
    const std::string& location_id
) const {
  const auto stored = secrets_.get(kBindingService, binding_account(project_id, location_id));
  if (!stored.has_value()) return std::nullopt;
  const auto body = nlohmann::json::parse(stored->secret);
  LocationBinding binding;
  binding.version = body.at("version").get<int>();
  binding.provider = body.at("provider").get<std::string>();
  binding.values = body.at("values").get<std::map<std::string, std::string>>();
  if (binding.version != 1 || binding.provider.empty()) {
    throw std::runtime_error("unsupported location binding");
  }
  return binding;
}

std::optional<std::string> LocationBindingStore::preview(
    const std::string& project_id,
    const std::string& location_id
) const {
  const auto stored = secrets_.get(kBindingService, binding_account(project_id, location_id));
  return stored.has_value() ? std::optional<std::string>(stored->metadata.preview) : std::nullopt;
}

void LocationBindingStore::unbind(const std::string& project_id, const std::string& location_id) {
  secrets_.remove(kBindingService, binding_account(project_id, location_id));
}

void LocationBindingStore::set_preferred(
    const std::string& project_id,
    const std::string& location_id,
    long long now
) {
  if (project_id.empty() || location_id.empty()) throw std::invalid_argument("preferred ids required");
  const auto existing = secrets_.get(kPreferredService, project_id);
  const long long created_at = existing.has_value() ? existing->metadata.created_at : now;
  secrets_.set(kPreferredService, project_id, location_id, location_id, created_at, now);
}

std::optional<std::string> LocationBindingStore::preferred(const std::string& project_id) const {
  const auto value = secrets_.get(kPreferredService, project_id);
  return value.has_value() ? std::optional<std::string>(value->secret) : std::nullopt;
}

void LocationBindingStore::clear_preferred(const std::string& project_id) {
  secrets_.remove(kPreferredService, project_id);
}

} // namespace holder::resource
