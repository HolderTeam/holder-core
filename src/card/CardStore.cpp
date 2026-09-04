#include "card/CardStore.h"

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "card/LinkRepo.h"
#include "card/TagExtractor.h"
#include "git/GitOps.h"
#include "platform/Fs.h"
#include "privacy/ProjectPrivacy.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <map>
#include <stdexcept>
namespace holder::card {
namespace {

holder::core::Fs& resolve_fs(holder::core::Fs* fs) {
  static holder::core::RealFs real_fs;
  return fs ? *fs : real_fs;
}

holder::git::GitOps& resolve_git(holder::git::GitOps* git) {
  static holder::git::RealGitOps real_git;
  return git ? *git : real_git;
}

const std::string& require_project_key_id(const holder::model::Project& project) {
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::runtime_error("encrypted project missing project_key_id");
  }
  return *project.project_key_id;
}

void write_card_file(
    holder::git::GitOps& repo,
    const holder::model::Project& project,
    const holder::model::Card& card,
    const std::vector<holder::model::CardLink>& links,
    const std::vector<holder::model::Milestone>& milestones,
    const std::string& content
) {
  const auto plain = holder::core::render_card_front_matter(card, links, milestones) + content;
  if (project.privacy_mode == "encrypted_git") {
    const auto& project_key_id = require_project_key_id(project);
    repo.write_file(
        card.rel_path,
        holder::privacy::encrypt_project_blob(project.project_id, project_key_id, plain)
    );
    return;
  }
  repo.write_file(card.rel_path, plain);
}

std::string decode_card_blob(const holder::model::Project& project, const std::string& blob) {
  if (project.privacy_mode != "encrypted_git") {
    return blob;
  }
  const auto& project_key_id = require_project_key_id(project);
  return holder::privacy::decrypt_project_blob(project.project_id, project_key_id, blob);
}

void assert_project_staged_blobs_safe(
    const holder::model::Project& project,
    const std::vector<std::string>& relative_paths
) {
  if (project.privacy_mode != "encrypted_git") {
    return;
  }
  holder::privacy::assert_encryption_index_paths_safe(project.root_path, relative_paths);
}

} // namespace

CardStore::CardStore(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::core::Fs* fs,
    holder::git::GitOps* git
)
    : fs_(&resolve_fs(fs)),
      git_(&resolve_git(git)),
      card_repo_(db),
      link_repo_(db),
      milestone_repo_(db),
      tag_repo_(db),
      project_repo_(db),
      fts_(fts) {}

holder::model::Project CardStore::require_project(const std::string& project_id) {
  const auto project_opt = project_repo_.get(project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found: " + project_id);
  }
  git_->open_or_init(project_opt->root_path);
  if (project_opt->git_remote_url.has_value()) {
    git_->set_remote("origin", project_opt->git_remote_url.value());
  }
  return project_opt.value();
}

void CardStore::create(
    holder::model::Card card,
    const std::string& content,
    const std::optional<double>& explicit_sort_key
) {
  const auto project = require_project(card.project_id);
  if (explicit_sort_key.has_value()) {
    card.sort_key = explicit_sort_key.value();
  } else {
    card.sort_key = card_repo_.next_sort_key(card.project_id, card.parent_card_id);
  }

  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path.empty()) {
    card.rel_path = expected;
  } else if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  if (card_repo_.get(card.card_id).has_value()) {
    throw std::runtime_error("conflict: card_id already exists");
  }

  const auto full_path = git_->repo_dir() / card.rel_path;
  if (fs_->exists(full_path)) {
    throw std::runtime_error("conflict: card file already exists");
  }

  const auto links = link_repo_.list_outgoing(card.project_id, card.card_id);
  write_card_file(*git_, project, card, links, {}, content);

  git_->stage_path(card.rel_path);
  assert_project_staged_blobs_safe(project, {card.rel_path});

  try {
    card_repo_.create(card);
  } catch (...) {
    fs_->remove(git_->repo_dir() / card.rel_path);
    throw;
  }

  if (fts_) {
    fts_->upsert_card(card.card_id, card.project_id, card.title, content);
  }
  tag_repo_.set_tags_for_card(
      card.project_id, card.card_id, holder::core::extract_tags(content), card.created_at
  );

  git_->commit("Add card " + card.title);
}

void CardStore::create_batch(
    const std::string& project_id,
    const std::vector<BatchCardInput>& items,
    const std::function<std::string()>& uuid_v4,
    const std::string& commit_message
) {
  const auto project = require_project(project_id);

  // card_id is a global primary key, not scoped per project -- restored cards always get a
  // fresh id (see BatchCardInput's doc comment), so link targets need remapping from the
  // snapshot's original card_id to the id it was actually written under here.
  std::map<std::string, std::string> id_remap;
  for (const auto& item : items) {
    id_remap[item.card_id] = uuid_v4();
  }

  std::vector<std::string> staged_paths;
  staged_paths.reserve(items.size());

  for (const auto& item : items) {
    holder::model::Card card;
    card.card_id = id_remap.at(item.card_id);
    card.project_id = project_id;
    card.title = item.title;
    card.rel_path = holder::core::card_rel_path(card.card_id);
    card.sort_key = card_repo_.next_sort_key(project_id, std::nullopt);
    card.created_at = item.created_at;
    card.updated_at = item.updated_at;

    std::vector<holder::model::CardLink> links;
    for (const auto& link : item.links) {
      const auto remapped = id_remap.find(link.to_card_id);
      if (link.to_type != "card" || remapped == id_remap.end()) {
        // Dropped, not written dangling: the target either wasn't a card link or didn't make
        // it into this restore batch (evicted from the snapshot, or a resource-ref -- see
        // BACKUP_RESTORE_DESIGN.md's "open" section on resource-ref hydration being deferred).
        continue;
      }
      holder::model::CardLink resolved = link;
      resolved.to_card_id = remapped->second;
      resolved.project_id = project_id;
      resolved.from_card_id = card.card_id;
      if (resolved.created_at <= 0) {
        resolved.created_at = card.updated_at;
      }
      links.push_back(std::move(resolved));
    }

    std::vector<holder::model::Milestone> milestones;
    for (const auto& milestone : item.milestones) {
      holder::model::Milestone resolved = milestone;
      resolved.milestone_id = uuid_v4();
      resolved.project_id = project_id;
      resolved.card_id = card.card_id;
      if (resolved.created_at <= 0) {
        resolved.created_at = card.updated_at;
      }
      if (resolved.updated_at <= 0) {
        resolved.updated_at = card.updated_at;
      }
      milestones.push_back(std::move(resolved));
    }

    write_card_file(*git_, project, card, links, milestones, item.content);
    git_->stage_path(card.rel_path);
    staged_paths.push_back(card.rel_path);

    card_repo_.create(card);
    if (fts_) {
      fts_->upsert_card(card.card_id, card.project_id, card.title, item.content);
    }
    tag_repo_.set_tags_for_card(
        project_id, card.card_id, holder::core::extract_tags(item.content), card.updated_at
    );
    if (!links.empty()) {
      link_repo_.upsert_links(project_id, card.card_id, links);
    }
    if (!milestones.empty()) {
      milestone_repo_.replace_for_card(project_id, card.card_id, milestones);
    }
  }

  assert_project_staged_blobs_safe(project, staged_paths);

  if (!items.empty()) {
    git_->commit(commit_message);
  }
}

void CardStore::update_content(
    const std::string& card_id,
    const std::string& content,
    const std::optional<std::string>& title,
    long long updated_at
) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }

  const auto& card = card_opt.value();
  const auto project = require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = git_->repo_dir() / card.rel_path;
  bool unchanged = false;
  std::vector<holder::model::Milestone> existing_milestones;
  if (fs_->exists(full_path)) {
    const auto plain = decode_card_blob(project, fs_->read_file(full_path));
    const auto parsed = holder::core::parse_card_file(plain);
    unchanged = (parsed.body == content);
    existing_milestones = parsed.milestones;
  }

  if (!unchanged) {
    auto updated_card = card;
    if (title.has_value()) {
      updated_card.title = title.value();
    }
    updated_card.updated_at = updated_at;
    const auto links = link_repo_.list_outgoing(card.project_id, card.card_id);
    write_card_file(*git_, project, updated_card, links, existing_milestones, content);
  }

  if (!unchanged) {
    git_->stage_path(card.rel_path);
    assert_project_staged_blobs_safe(project, {card.rel_path});
  }

  if (title.has_value()) {
    card_repo_.update_title(card_id, title.value(), updated_at);
  } else {
    card_repo_.touch_updated(card_id, updated_at);
  }

  const std::string fts_title = title.has_value() ? title.value() : card.title;
  if (fts_) {
    fts_->upsert_card(card.card_id, card.project_id, fts_title, content);
  }
  tag_repo_.set_tags_for_card(
      card.project_id, card_id, holder::core::extract_tags(content), updated_at
  );

  if (!unchanged) {
    const std::string commit_title = title.has_value() ? title.value() : card.title;
    git_->commit("Update card " + commit_title);
  }
}

void CardStore::move(
    const std::string& card_id,
    bool has_parent_card_id,
    const std::optional<std::string>& parent_card_id,
    const std::optional<double>& sort_key,
    long long updated_at
) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }

  auto card = card_opt.value();
  const auto project = require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const std::optional<std::string> next_parent = has_parent_card_id ? parent_card_id
                                                                    : card.parent_card_id;
  double next_sort = card.sort_key;
  if (sort_key.has_value()) {
    next_sort = sort_key.value();
  } else if (has_parent_card_id && next_parent != card.parent_card_id) {
    next_sort = card_repo_.next_sort_key(card.project_id, next_parent);
  }

  const bool changed = (next_parent != card.parent_card_id) || (next_sort != card.sort_key);
  if (!changed) {
    return;
  }

  const auto full_path = git_->repo_dir() / card.rel_path;
  if (!fs_->exists(full_path)) {
    throw std::runtime_error("card content missing");
  }

  const auto raw = fs_->read_file(full_path);
  const auto plain = decode_card_blob(project, raw);
  const auto parsed = holder::core::parse_card_file(plain);
  const auto links = link_repo_.list_outgoing(card.project_id, card.card_id);

  card.parent_card_id = next_parent;
  card.sort_key = next_sort;
  card.updated_at = updated_at;
  const auto updated_plain =
      holder::core::render_card_front_matter(card, links, parsed.milestones) + parsed.body;
  const auto updated_raw = (project.privacy_mode == "encrypted_git")
                               ? holder::privacy::encrypt_project_blob(
                                     project.project_id,
                                     require_project_key_id(project),
                                     updated_plain
                                 )
                               : updated_plain;

  if (updated_raw == raw) {
    return;
  }

  git_->write_file(card.rel_path, updated_raw);
  git_->stage_path(card.rel_path);
  assert_project_staged_blobs_safe(project, {card.rel_path});
  card_repo_.move(card_id, next_parent, next_sort, updated_at);
  if (fts_) {
    fts_->upsert_card(card.card_id, card.project_id, card.title, parsed.body);
  }
  git_->commit("Move card " + card.title);
}

void CardStore::update_links(const std::string& card_id, long long updated_at) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }

  auto card = card_opt.value();
  const auto project = require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = git_->repo_dir() / card.rel_path;
  if (!fs_->exists(full_path)) {
    throw std::runtime_error("card content missing");
  }

  const auto raw = fs_->read_file(full_path);
  const auto plain = decode_card_blob(project, raw);
  const auto parsed = holder::core::parse_card_file(plain);
  const auto links = link_repo_.list_outgoing(card.project_id, card.card_id);

  card.updated_at = updated_at;
  const auto updated_plain =
      holder::core::render_card_front_matter(card, links, parsed.milestones) + parsed.body;
  const auto updated_raw = (project.privacy_mode == "encrypted_git")
                               ? holder::privacy::encrypt_project_blob(
                                     project.project_id,
                                     require_project_key_id(project),
                                     updated_plain
                                 )
                               : updated_plain;

  if (updated_raw == raw) {
    return;
  }

  git_->write_file(card.rel_path, updated_raw);
  git_->stage_path(card.rel_path);
  assert_project_staged_blobs_safe(project, {card.rel_path});
  card_repo_.touch_updated(card_id, updated_at);
  git_->commit("Update links for " + card.title);
}

void CardStore::update_milestones(const std::string& card_id, long long updated_at) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }

  auto card = card_opt.value();
  const auto project = require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = git_->repo_dir() / card.rel_path;
  if (!fs_->exists(full_path)) {
    throw std::runtime_error("card content missing");
  }

  const auto raw = fs_->read_file(full_path);
  const auto plain = decode_card_blob(project, raw);
  const auto parsed = holder::core::parse_card_file(plain);
  const auto milestones = milestone_repo_.list_for_card(card.project_id, card.card_id);

  card.updated_at = updated_at;
  const auto updated_plain =
      holder::core::render_card_front_matter(card, parsed.links, milestones) + parsed.body;
  const auto updated_raw = (project.privacy_mode == "encrypted_git")
                               ? holder::privacy::encrypt_project_blob(
                                     project.project_id,
                                     require_project_key_id(project),
                                     updated_plain
                                 )
                               : updated_plain;

  if (updated_raw == raw) {
    return;
  }

  git_->write_file(card.rel_path, updated_raw);
  git_->stage_path(card.rel_path);
  assert_project_staged_blobs_safe(project, {card.rel_path});
  card_repo_.touch_updated(card_id, updated_at);
  git_->commit("Update milestones for " + card.title);
}

void CardStore::trash(const std::string& card_id, long long deleted_at) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto& card = card_opt.value();
  if (card.deleted_at.has_value()) {
    throw std::runtime_error("card already deleted");
  }

  const auto project = require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto src_path = git_->repo_dir() / card.rel_path;
  if (!fs_->exists(src_path)) {
    throw std::runtime_error("card content missing");
  }

  const std::string trash_rel = holder::core::card_trash_rel_path(card.card_id);
  const auto dst_path = git_->repo_dir() / trash_rel;
  fs_->create_directories(dst_path.parent_path());
  fs_->rename(src_path, dst_path);

  card_repo_.soft_delete(card_id, deleted_at, deleted_at);
  if (fts_) {
    fts_->delete_card(card_id);
  }
  tag_repo_.delete_tags_for_card(card.project_id, card_id);
  milestone_repo_.delete_for_card(card.project_id, card_id);
  git_->remove_path(card.rel_path);
  git_->stage_path(trash_rel);
  git_->commit("Delete card " + card.title);
}

void CardStore::restore(const std::string& card_id, long long updated_at) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto& card = card_opt.value();
  if (!card.deleted_at.has_value()) {
    throw std::runtime_error("card is not deleted");
  }

  const auto project = require_project(card.project_id);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const std::string trash_rel = holder::core::card_trash_rel_path(card.card_id);
  const auto src_path = git_->repo_dir() / trash_rel;
  if (!fs_->exists(src_path)) {
    throw std::runtime_error("card content missing");
  }

  const auto dst_path = git_->repo_dir() / card.rel_path;
  fs_->create_directories(dst_path.parent_path());
  fs_->rename(src_path, dst_path);

  git_->stage_path(card.rel_path);
  assert_project_staged_blobs_safe(project, {card.rel_path});

  card_repo_.restore(card_id, updated_at);
  const auto raw = fs_->read_file(dst_path);
  const auto parsed = holder::core::parse_card_file(decode_card_blob(project, raw));
  if (fts_) {
    fts_->upsert_card(card.card_id, card.project_id, card.title, parsed.body);
  }
  tag_repo_.set_tags_for_card(
      card.project_id, card_id, holder::core::extract_tags(parsed.body), updated_at
  );
  milestone_repo_.replace_for_card(card.project_id, card_id, parsed.milestones);
  git_->remove_path(trash_rel);
  git_->commit("Restore card " + card.title);
}

void CardStore::hard_delete(const std::string& card_id) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto& card = card_opt.value();
  if (!card.deleted_at.has_value()) {
    throw std::runtime_error("card is not deleted");
  }

  require_project(card.project_id);
  const std::string trash_rel = holder::core::card_trash_rel_path(card.card_id);
  const auto trash_path = git_->repo_dir() / trash_rel;
  if (fs_->exists(trash_path)) {
    fs_->remove(trash_path);
    git_->remove_path(trash_rel);
  }

  link_repo_.delete_links_from(card.project_id, card.card_id);
  link_repo_.delete_links_to_typed(card.project_id, card.card_id, "card");
  tag_repo_.delete_tags_for_card(card.project_id, card.card_id);
  milestone_repo_.delete_for_card(card.project_id, card.card_id);
  card_repo_.remove(card.card_id);
  git_->commit("Permanently delete card " + card.title);
}

std::optional<holder::model::Card> CardStore::get(const std::string& card_id) const {
  return card_repo_.get(card_id);
}

std::optional<std::string> CardStore::get_content(const holder::model::Card& card) {
  const auto project_opt = project_repo_.get(card.project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found: " + card.project_id);
  }
  git_->open_or_init(project_opt->root_path);

  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = git_->repo_dir() / card.rel_path;
  if (!fs_->exists(full_path)) {
    return std::nullopt;
  }

  const auto raw = fs_->read_file(full_path);
  const auto plain = decode_card_blob(project_opt.value(), raw);
  return holder::core::parse_card_file(plain).body;
}

} // namespace holder::card
