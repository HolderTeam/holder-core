#include "resource/ResourceRepo.h"

#include <sqlite3.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace holder::resource {
namespace {

class Statement {
 public:
  Statement(sqlite3* db, const char* sql, const std::string& what)
      : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(what + ": " + sqlite3_errmsg(db));
    }
  }
  ~Statement() { sqlite3_finalize(stmt_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  sqlite3_stmt* get() const { return stmt_; }
  void reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
  }

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
  if (sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind text failed");
  }
}

void bind_int64(sqlite3_stmt* stmt, int index, long long value) {
  if (sqlite3_bind_int64(stmt, index, value) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind integer failed");
  }
}

std::string text_column(sqlite3_stmt* stmt, int index) {
  const auto* value = sqlite3_column_text(stmt, index);
  return value ? reinterpret_cast<const char*>(value) : std::string();
}

void expect_done(sqlite3* db, sqlite3_stmt* stmt, const std::string& what) {
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error(what + ": " + sqlite3_errmsg(db));
  }
}

holder::model::Resource read_resource_row(sqlite3_stmt* stmt) {
  holder::model::Resource resource;
  resource.resource_id = text_column(stmt, 0);
  resource.project_id = text_column(stmt, 1);
  resource.type = text_column(stmt, 2);
  resource.label = text_column(stmt, 3);
  resource.created_at = sqlite3_column_int64(stmt, 4);
  resource.updated_at = sqlite3_column_int64(stmt, 5);
  return resource;
}

void validate_resource(const holder::model::Resource& resource) {
  if (resource.resource_id.empty() || resource.project_id.empty() || resource.type.empty() ||
      resource.label.empty()) {
    throw std::invalid_argument("resource identity, project, type and label are required");
  }
}

} // namespace

ResourceRepo::ResourceRepo(holder::platform::Db& db)
    : db_(db) {}

void ResourceRepo::add(const holder::model::Resource& resource) {
  if (get(resource.resource_id).has_value()) {
    throw std::runtime_error("conflict: resource_id already exists");
  }
  put_bundle(holder::model::ResourceBundle{resource, {}});
}

std::optional<holder::model::Resource> ResourceRepo::get(const std::string& resource_id) const {
  Statement resource_stmt(
      db_.handle(),
      "SELECT resource_id, project_id, type, label, created_at, updated_at "
      "FROM resources WHERE resource_id = ?;",
      "prepare get resource failed"
  );
  bind_text(resource_stmt.get(), 1, resource_id);
  const int rc = sqlite3_step(resource_stmt.get());
  if (rc == SQLITE_DONE) return std::nullopt;
  if (rc != SQLITE_ROW) {
    throw std::runtime_error(std::string("get resource failed: ") + sqlite3_errmsg(db_.handle()));
  }
  auto resource = read_resource_row(resource_stmt.get());

  Statement metadata_stmt(
      db_.handle(),
      "SELECT property, value FROM resource_metadata WHERE resource_id = ? "
      "ORDER BY property, value_index;",
      "prepare get resource metadata failed"
  );
  bind_text(metadata_stmt.get(), 1, resource_id);
  while (true) {
    const int metadata_rc = sqlite3_step(metadata_stmt.get());
    if (metadata_rc == SQLITE_DONE) break;
    if (metadata_rc != SQLITE_ROW) {
      throw std::runtime_error(
          std::string("get resource metadata failed: ") + sqlite3_errmsg(db_.handle())
      );
    }
    resource.metadata[text_column(metadata_stmt.get(), 0)].push_back(
        text_column(metadata_stmt.get(), 1)
    );
  }
  return resource;
}

std::optional<holder::model::ResourceBundle> ResourceRepo::get_bundle(
    const std::string& resource_id
) const {
  const auto resource = get(resource_id);
  if (!resource.has_value()) return std::nullopt;

  holder::model::ResourceBundle bundle;
  bundle.resource = *resource;
  Statement asset_stmt(
      db_.handle(),
      "SELECT asset_id, original_filename, media_type, byte_size, plaintext_sha256, "
      "created_at, updated_at FROM assets WHERE resource_id = ? ORDER BY created_at, asset_id;",
      "prepare get assets failed"
  );
  bind_text(asset_stmt.get(), 1, resource_id);
  while (true) {
    const int asset_rc = sqlite3_step(asset_stmt.get());
    if (asset_rc == SQLITE_DONE) break;
    if (asset_rc != SQLITE_ROW) {
      throw std::runtime_error(std::string("get assets failed: ") + sqlite3_errmsg(db_.handle()));
    }
    holder::model::Asset asset;
    asset.asset_id = text_column(asset_stmt.get(), 0);
    asset.resource_id = resource_id;
    asset.original_filename = text_column(asset_stmt.get(), 1);
    asset.media_type = text_column(asset_stmt.get(), 2);
    asset.byte_size = sqlite3_column_int64(asset_stmt.get(), 3);
    asset.plaintext_sha256 = text_column(asset_stmt.get(), 4);
    asset.created_at = sqlite3_column_int64(asset_stmt.get(), 5);
    asset.updated_at = sqlite3_column_int64(asset_stmt.get(), 6);

    Statement placement_stmt(
        db_.handle(),
        "SELECT placement_id, location_id, object_key, encoding, stored_byte_size, "
        "stored_sha256, created_at FROM asset_placements WHERE asset_id = ? "
        "ORDER BY created_at, placement_id;",
        "prepare get placements failed"
    );
    bind_text(placement_stmt.get(), 1, asset.asset_id);
    while (true) {
      const int placement_rc = sqlite3_step(placement_stmt.get());
      if (placement_rc == SQLITE_DONE) break;
      if (placement_rc != SQLITE_ROW) {
        throw std::runtime_error(
            std::string("get placements failed: ") + sqlite3_errmsg(db_.handle())
        );
      }
      holder::model::Placement placement;
      placement.placement_id = text_column(placement_stmt.get(), 0);
      placement.asset_id = asset.asset_id;
      placement.location_id = text_column(placement_stmt.get(), 1);
      placement.object_key = text_column(placement_stmt.get(), 2);
      placement.encoding = text_column(placement_stmt.get(), 3);
      placement.stored_byte_size = sqlite3_column_int64(placement_stmt.get(), 4);
      placement.stored_sha256 = text_column(placement_stmt.get(), 5);
      placement.created_at = sqlite3_column_int64(placement_stmt.get(), 6);
      asset.placements.push_back(std::move(placement));
    }
    bundle.assets.push_back(std::move(asset));
  }
  return bundle;
}

std::optional<holder::model::ResourceBundle> ResourceRepo::find_by_asset_hash(
    const std::string& project_id,
    const std::string& plaintext_sha256
) const {
  Statement stmt(
      db_.handle(),
      "SELECT resource_id FROM assets WHERE project_id = ? AND plaintext_sha256 = ? "
      "ORDER BY created_at LIMIT 1;",
      "prepare find asset hash failed"
  );
  bind_text(stmt.get(), 1, project_id);
  bind_text(stmt.get(), 2, plaintext_sha256);
  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_DONE) return std::nullopt;
  if (rc != SQLITE_ROW) {
    throw std::runtime_error(std::string("find asset hash failed: ") + sqlite3_errmsg(db_.handle()));
  }
  return get_bundle(text_column(stmt.get(), 0));
}

void ResourceRepo::put_bundle(const holder::model::ResourceBundle& bundle) {
  validate_resource(bundle.resource);
  remove(bundle.resource.resource_id);

  Statement resource_stmt(
      db_.handle(),
      "INSERT INTO resources(resource_id, project_id, type, label, created_at, updated_at) "
      "VALUES(?, ?, ?, ?, ?, ?);",
      "prepare put resource failed"
  );
  bind_text(resource_stmt.get(), 1, bundle.resource.resource_id);
  bind_text(resource_stmt.get(), 2, bundle.resource.project_id);
  bind_text(resource_stmt.get(), 3, bundle.resource.type);
  bind_text(resource_stmt.get(), 4, bundle.resource.label);
  bind_int64(resource_stmt.get(), 5, bundle.resource.created_at);
  bind_int64(resource_stmt.get(), 6, bundle.resource.updated_at);
  expect_done(db_.handle(), resource_stmt.get(), "put resource failed");

  Statement metadata_stmt(
      db_.handle(),
      "INSERT INTO resource_metadata(resource_id, property, value_index, value) VALUES(?, ?, ?, ?);",
      "prepare put resource metadata failed"
  );
  for (const auto& [property, values] : bundle.resource.metadata) {
    if (property.empty()) throw std::invalid_argument("resource metadata property is empty");
    for (std::size_t index = 0; index < values.size(); ++index) {
      metadata_stmt.reset();
      bind_text(metadata_stmt.get(), 1, bundle.resource.resource_id);
      bind_text(metadata_stmt.get(), 2, property);
      bind_int64(metadata_stmt.get(), 3, static_cast<long long>(index));
      bind_text(metadata_stmt.get(), 4, values[index]);
      expect_done(db_.handle(), metadata_stmt.get(), "put resource metadata failed");
    }
  }

  Statement asset_stmt(
      db_.handle(),
      "INSERT INTO assets(asset_id, resource_id, project_id, original_filename, media_type, "
      "byte_size, plaintext_sha256, created_at, updated_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);",
      "prepare put asset failed"
  );
  Statement placement_stmt(
      db_.handle(),
      "INSERT INTO asset_placements(placement_id, asset_id, location_id, object_key, encoding, "
      "stored_byte_size, stored_sha256, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?);",
      "prepare put placement failed"
  );
  for (const auto& asset : bundle.assets) {
    if (asset.asset_id.empty() || asset.resource_id != bundle.resource.resource_id ||
        asset.plaintext_sha256.empty() || asset.byte_size < 0) {
      throw std::invalid_argument("invalid asset in resource bundle");
    }
    asset_stmt.reset();
    bind_text(asset_stmt.get(), 1, asset.asset_id);
    bind_text(asset_stmt.get(), 2, bundle.resource.resource_id);
    bind_text(asset_stmt.get(), 3, bundle.resource.project_id);
    bind_text(asset_stmt.get(), 4, asset.original_filename);
    bind_text(asset_stmt.get(), 5, asset.media_type);
    bind_int64(asset_stmt.get(), 6, asset.byte_size);
    bind_text(asset_stmt.get(), 7, asset.plaintext_sha256);
    bind_int64(asset_stmt.get(), 8, asset.created_at);
    bind_int64(asset_stmt.get(), 9, asset.updated_at);
    expect_done(db_.handle(), asset_stmt.get(), "put asset failed");

    for (const auto& placement : asset.placements) {
      if (placement.placement_id.empty() || placement.asset_id != asset.asset_id ||
          placement.location_id.empty() || placement.object_key.empty() ||
          placement.encoding.empty() || placement.stored_byte_size < 0) {
        throw std::invalid_argument("invalid placement in resource bundle");
      }
      placement_stmt.reset();
      bind_text(placement_stmt.get(), 1, placement.placement_id);
      bind_text(placement_stmt.get(), 2, asset.asset_id);
      bind_text(placement_stmt.get(), 3, placement.location_id);
      bind_text(placement_stmt.get(), 4, placement.object_key);
      bind_text(placement_stmt.get(), 5, placement.encoding);
      bind_int64(placement_stmt.get(), 6, placement.stored_byte_size);
      bind_text(placement_stmt.get(), 7, placement.stored_sha256);
      bind_int64(placement_stmt.get(), 8, placement.created_at);
      expect_done(db_.handle(), placement_stmt.get(), "put placement failed");
    }
  }
}

void ResourceRepo::update(const holder::model::Resource& resource) {
  const auto existing = get_bundle(resource.resource_id);
  if (!existing.has_value()) throw std::runtime_error("resource not found");
  auto bundle = *existing;
  bundle.resource = resource;
  put_bundle(bundle);
}

std::vector<holder::model::Resource> ResourceRepo::list(const std::string& project_id) const {
  Statement stmt(
      db_.handle(),
      "SELECT resource_id FROM resources WHERE project_id = ? ORDER BY updated_at DESC, resource_id;",
      "prepare list resources failed"
  );
  bind_text(stmt.get(), 1, project_id);
  std::vector<std::string> ids;
  while (true) {
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      throw std::runtime_error(std::string("list resources failed: ") + sqlite3_errmsg(db_.handle()));
    }
    ids.push_back(text_column(stmt.get(), 0));
  }
  std::vector<holder::model::Resource> resources;
  resources.reserve(ids.size());
  for (const auto& id : ids) resources.push_back(get(id).value());
  return resources;
}

void ResourceRepo::remove(const std::string& resource_id) {
  Statement stmt(db_.handle(), "DELETE FROM resources WHERE resource_id = ?;", "prepare remove resource failed");
  bind_text(stmt.get(), 1, resource_id);
  expect_done(db_.handle(), stmt.get(), "remove resource failed");
}

void ResourceRepo::remove_project(const std::string& project_id) {
  Statement stmt(db_.handle(), "DELETE FROM resources WHERE project_id = ?;", "prepare remove project resources failed");
  bind_text(stmt.get(), 1, project_id);
  expect_done(db_.handle(), stmt.get(), "remove project resources failed");
}

} // namespace holder::resource
