#include "card/CardStore.h"

#include "card/CardFrontMatter.h"
#include "card/CardHierarchy.h"
#include "card/CardMutation.h"
#include "card/CardPaths.h"
#include "card/LinkRepo.h"
#include "card/TagExtractor.h"
#include "card/TagLineEditor.h"
#include "git/GitOps.h"
#include "git/GitRepo.h"
#include "identity/Uuid.h"
#include "platform/Fs.h"
#include "platform/Tx.h"
#include "privacy/ProjectPrivacy.h"
#include "project/Rebuilder.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <utility>

namespace holder::card {
namespace {

holder::core::Fs& resolve_fs(holder::core::Fs* fs) {
  static holder::core::RealFs real_fs;
  return fs ? *fs : real_fs;
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

std::optional<std::string> read_card_content_locked(
    holder::core::Fs& fs,
    holder::git::GitOps& git,
    const holder::model::Project& project,
    const holder::model::Card& card
) {
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = git.repo_dir() / card.rel_path;
  if (!fs.exists(full_path)) {
    return std::nullopt;
  }

  const auto raw = fs.read_file(full_path);
  const auto plain = decode_card_blob(project, raw);
  return holder::core::parse_card_file(plain).body;
}

} // namespace

CardStore::CardStore(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::core::Fs* fs,
    holder::git::GitOps* git
)
    : db_(db),
      fs_(&resolve_fs(fs)),
      owned_git_(git ? nullptr : std::make_unique<holder::git::RealGitOps>()),
      git_(git ? git : owned_git_.get()),
      card_repo_(db),
      link_repo_(db),
      milestone_repo_(db),
      tag_repo_(db),
      project_repo_(db),
      fts_(fts) {}

holder::model::Project CardStore::require_project(const std::string& project_id) const {
  const auto project_opt = project_repo_.get(project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found: " + project_id);
  }
  auto operation = git_->lock_operation(project_opt->root_path);
  if (CardMutation::pending(project_opt->root_path)) {
    holder::store::Rebuilder(db_, fts_, fs_).rebuild_project(*project_opt);
  }
  return project_opt.value();
}

void CardStore::create(
    holder::model::Card card,
    const std::string& content,
    const std::optional<double>& explicit_sort_key
) {
  const auto project = require_project(card.project_id);
  auto operation = git_->lock_operation(project.root_path);
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
  validate_card_parent(card_repo_, card);
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
      card.project_id,
      card.card_id,
      holder::core::extract_tags(content),
      card.created_at
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
  auto operation = git_->lock_operation(project.root_path);
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);

  // card_id is a global primary key, not scoped per project -- restored cards always get a
  // fresh id (see BatchCardInput's doc comment), so link targets need remapping from the
  // snapshot's original card_id to the id it was actually written under here.
  std::map<std::string, std::string> id_remap;
  for (const auto& item : items) {
    id_remap[item.card_id] = holder::identity::generate_id(project.id_scheme);
  }

  std::vector<std::string> staged_paths;
  staged_paths.reserve(items.size());

  // One transaction for every card's rows, not one auto-committed statement group per card --
  // at snapshot-restore scale (tens of thousands of cards) each individual commit's fsync-ish
  // cost adds up the same way stage_path's per-call index rewrite did (see the comment on
  // git_->stage_paths below). Rolled back automatically if anything throws before commit().
  holder::platform::Tx tx(db_);

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
    staged_paths.push_back(card.rel_path);

    card_repo_.create(card);
    if (fts_) {
      fts_->upsert_card(card.card_id, card.project_id, card.title, item.content);
    }
    tag_repo_.set_tags_for_card(
        project_id,
        card.card_id,
        holder::core::extract_tags(item.content),
        card.updated_at
    );
    if (!links.empty()) {
      link_repo_.upsert_links(project_id, card.card_id, links);
    }
    if (!milestones.empty()) {
      milestone_repo_.replace_for_card(project_id, card.card_id, milestones);
    }
  }

  tx.commit();

  if (!staged_paths.empty()) {
    // One index open/write for every path (see GitOps::stage_paths), not one per card --
    // stage_path's own per-call git_index_write is O(n) over the growing index, which made
    // this O(n^2) over the whole batch at snapshot-restore scale before this existed.
    std::vector<std::filesystem::path> paths(staged_paths.begin(), staged_paths.end());
    git_->stage_paths(paths);
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
  auto operation = git_->lock_operation(project.root_path);
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const auto full_path = git_->repo_dir() / card.rel_path;
  bool body_unchanged = false;
  std::vector<holder::model::Milestone> existing_milestones;
  if (fs_->exists(full_path)) {
    const auto plain = decode_card_blob(project, fs_->read_file(full_path));
    const auto parsed = holder::core::parse_card_file(plain);
    body_unchanged = (parsed.body == content);
    existing_milestones = parsed.milestones;
  }
  const bool title_changed = title.has_value() && title.value() != card.title;
  const bool file_changed = !body_unchanged || title_changed;

  if (file_changed) {
    auto updated_card = card;
    if (title.has_value()) {
      updated_card.title = title.value();
    }
    updated_card.updated_at = updated_at;
    const auto links = link_repo_.list_outgoing(card.project_id, card.card_id);
    write_card_file(*git_, project, updated_card, links, existing_milestones, content);
  }

  if (file_changed) {
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
  tag_repo_
      .set_tags_for_card(card.project_id, card_id, holder::core::extract_tags(content), updated_at);

  if (file_changed) {
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
  auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }

  const auto project = require_project(card_opt->project_id);
  auto operation = git_->lock_operation(project.root_path);
  card_opt = card_repo_.get(card_id);
  if (!card_opt) throw std::runtime_error("card not found: " + card_id);
  auto card = *card_opt;
  if (card.deleted_at) throw std::runtime_error("card already deleted");
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
  const std::string expected = holder::core::card_rel_path(card.card_id);
  if (card.rel_path != expected) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  const std::optional<std::string> next_parent = has_parent_card_id ? parent_card_id
                                                                    : card.parent_card_id;
  auto destination = card;
  destination.parent_card_id = next_parent;
  validate_card_parent(card_repo_, destination);
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
  auto operation = git_->lock_operation(project.root_path);
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
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
  auto operation = git_->lock_operation(project.root_path);
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
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

std::optional<holder::model::Milestone> CardStore::update_milestone(
    const std::string& project_id,
    const std::string& card_id,
    const std::string& milestone_id,
    const MilestoneUpdate& update,
    long long updated_at
) {
  const auto card_opt = card_repo_.get(card_id);
  if (!card_opt.has_value() || card_opt->project_id != project_id ||
      card_opt->deleted_at.has_value()) {
    return std::nullopt;
  }

  auto milestones = milestone_repo_.list_for_card(project_id, card_id);
  const auto position = std::find_if(milestones.begin(), milestones.end(), [&](const auto& value) {
    return value.milestone_id == milestone_id;
  });
  if (position == milestones.end()) return std::nullopt;

  auto updated = *position;
  if (update.start_at.has_value()) updated.start_at = *update.start_at;
  if (update.has_end_at) updated.end_at = update.end_at;
  if (update.all_day.has_value()) updated.all_day = *update.all_day;
  if (update.has_kind) updated.kind = update.kind;
  if (update.has_description) updated.description = update.description;

  if (updated.end_at.has_value() && *updated.end_at < updated.start_at) {
    throw std::invalid_argument("milestone end_at must not be before start_at");
  }

  const bool changed = updated.start_at != position->start_at ||
                       updated.end_at != position->end_at || updated.all_day != position->all_day ||
                       updated.kind != position->kind ||
                       updated.description != position->description;
  if (!changed) return *position;

  updated.updated_at = updated_at;
  *position = updated;

  auto card = *card_opt;
  const auto project = require_project(project_id);
  auto operation = git_->lock_operation(project.root_path);
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
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

  card.updated_at = updated_at;
  write_card_file(*git_, project, card, parsed.links, milestones, parsed.body);
  git_->stage_path(card.rel_path);
  assert_project_staged_blobs_safe(project, {card.rel_path});
  milestone_repo_.replace_for_card(project_id, card_id, milestones);
  card_repo_.touch_updated(card_id, updated_at);
  git_->commit("Update milestone for " + card.title);
  return updated;
}

void CardStore::apply_lifecycle(
    const holder::model::Project& project,
    const holder::model::Card& current,
    holder::core::ParsedCardFile target,
    const std::vector<holder::model::Card>& placements,
    const std::string& message
) {
  const auto live_path = holder::core::card_rel_path(current.card_id);
  if (current.rel_path != live_path &&
      !(current.deleted_at && current.rel_path == holder::core::card_trash_rel_path(current.card_id)
      ))
    throw std::runtime_error("card rel_path does not match card_id");
  const auto trash_path = holder::core::card_trash_rel_path(current.card_id);
  const auto source_path = current.deleted_at ? trash_path : live_path;
  const auto target_path = target.card.deleted_at ? trash_path : live_path;
  if (!fs_->exists(git_->repo_dir() / source_path))
    throw std::runtime_error("card content missing");
  if (source_path != target_path && fs_->exists(git_->repo_dir() / target_path)) {
    throw std::runtime_error("conflict: destination card file already exists");
  }

  std::vector<holder::core::ParsedCardFile> records;
  target.card.rel_path = live_path;
  records.push_back(std::move(target));
  std::vector<std::string> paths{live_path, trash_path};
  for (const auto& card : placements) {
    if (card.rel_path != holder::core::card_rel_path(card.card_id)) {
      throw std::runtime_error("card rel_path does not match card_id");
    }
    const auto path = git_->repo_dir() / card.rel_path;
    if (!fs_->exists(path)) throw std::runtime_error("card content missing");
    auto parsed = holder::core::parse_card_file(decode_card_blob(project, fs_->read_file(path)));
    parsed.card = card;
    parsed.links = link_repo_.list_outgoing(card.project_id, card.card_id);
    records.push_back(std::move(parsed));
    paths.push_back(card.rel_path);
  }

  // Decode and encrypt every affected card before the first file or database write.
  std::vector<std::pair<std::string, std::string>> writes;
  for (const auto& record : records) {
    auto durable_card = record.card;
    if (durable_card.deleted_at)
      durable_card.rel_path = holder::core::card_trash_rel_path(durable_card.card_id);
    const auto plain =
        holder::core::render_card_front_matter(durable_card, record.links, record.milestones) +
        record.body;
    writes.emplace_back(
        durable_card.rel_path,
        project.privacy_mode == "encrypted_git" ? holder::privacy::encrypt_project_blob(
                                                      project.project_id,
                                                      require_project_key_id(project),
                                                      plain
                                                  )
                                                : plain
    );
  }
  CardMutation mutation(*fs_, project.root_path, paths);
  holder::platform::Tx tx(db_);
  for (const auto& record : records) {
    const auto& card = record.card;
    card_repo_.restore_snapshot(card);
    link_repo_.delete_links_from(card.project_id, card.card_id);
    if (!record.links.empty()) link_repo_.upsert_links(card.project_id, card.card_id, record.links);
    if (card.deleted_at) {
      if (fts_) fts_->delete_card(card.card_id);
      tag_repo_.delete_tags_for_card(card.project_id, card.card_id);
      milestone_repo_.delete_for_card(card.project_id, card.card_id);
    } else {
      if (fts_) fts_->upsert_card(card.card_id, card.project_id, card.title, record.body);
      tag_repo_.set_tags_for_card(
          card.project_id,
          card.card_id,
          holder::core::extract_tags(record.body),
          card.updated_at
      );
      milestone_repo_.replace_for_card(card.project_id, card.card_id, record.milestones);
    }
  }
  try {
    mutation.begin();
    if (source_path != target_path) {
      fs_->create_directories((git_->repo_dir() / target_path).parent_path());
      fs_->rename(git_->repo_dir() / source_path, git_->repo_dir() / target_path);
      git_->remove_path(source_path);
    }
    std::vector<std::string> staged;
    for (const auto& [path, bytes] : writes) {
      git_->write_file(path, bytes);
      if (fs_->read_file(git_->repo_dir() / path) != bytes) {
        throw std::runtime_error("incomplete card lifecycle write");
      }
      git_->stage_path(path);
      staged.push_back(path);
    }
    assert_project_staged_blobs_safe(project, staged);
    git_->commit(message);
    mutation.preserve_unrelated_index();
    // The journal helper uses a separate Git handle; discard this adapter's cached index.
    git_->open_or_init(project.root_path);
    tx.commit();
  } catch (...) {
    // If compensation itself fails the durable journal remains for recovery.
    mutation.rollback();
    git_->open_or_init(project.root_path);
    throw;
  }
  mutation.finish();
}

void CardStore::trash(const std::string& card_id, long long deleted_at) {
  auto card = card_repo_.get(card_id);
  if (!card) throw std::runtime_error("card not found: " + card_id);
  const auto project = require_project(card->project_id);
  auto operation = git_->lock_operation(project.root_path);
  card = card_repo_.get(card_id);
  if (!card) throw std::runtime_error("card not found: " + card_id);
  if (card->deleted_at) throw std::runtime_error("card already deleted");
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
  if (card->rel_path != holder::core::card_rel_path(card_id))
    throw std::runtime_error("card rel_path does not match card_id");
  const auto source = git_->repo_dir() / card->rel_path;
  if (!fs_->exists(source)) throw std::runtime_error("card content missing");
  auto parsed = holder::core::parse_card_file(decode_card_blob(project, fs_->read_file(source)));
  parsed.card = *card;
  parsed.card.deleted_at = deleted_at;
  parsed.card.updated_at = deleted_at;
  parsed.links = link_repo_.list_outgoing(card->project_id, card_id);
  const auto placements = promoted_card_children(card_repo_, *card, deleted_at);
  apply_lifecycle(project, *card, std::move(parsed), placements, "Delete card " + card->title);
}

void CardStore::restore(const std::string& card_id, long long updated_at) {
  auto card = card_repo_.get(card_id);
  if (!card) throw std::runtime_error("card not found: " + card_id);
  const auto project = require_project(card->project_id);
  auto operation = git_->lock_operation(project.root_path);
  card = card_repo_.get(card_id);
  if (!card) throw std::runtime_error("card not found: " + card_id);
  if (!card->deleted_at) throw std::runtime_error("card is not deleted");
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
  if (card->rel_path != holder::core::card_rel_path(card_id) &&
      card->rel_path != holder::core::card_trash_rel_path(card_id))
    throw std::runtime_error("card rel_path does not match card_id");
  const auto source = git_->repo_dir() / holder::core::card_trash_rel_path(card_id);
  if (!fs_->exists(source)) throw std::runtime_error("card content missing");
  auto parsed = holder::core::parse_card_file(decode_card_blob(project, fs_->read_file(source)));
  parsed.card = *card;
  parsed.card.deleted_at.reset();
  parsed.card.updated_at = updated_at;
  parsed.links = link_repo_.list_outgoing(card->project_id, card_id);
  const auto placements = restored_card_placement(card_repo_, parsed.card, updated_at);
  apply_lifecycle(project, *card, std::move(parsed), placements, "Restore card " + card->title);
}

void CardStore::restore_version(
    const std::string& card_id,
    const std::string& historical_oid,
    long long updated_at
) {
  if (historical_oid.empty()) throw std::invalid_argument("historical_oid is required");
  auto current_opt = card_repo_.get(card_id);
  if (!current_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto project = require_project(current_opt->project_id);
  auto operation = git_->lock_operation(project.root_path);
  current_opt = card_repo_.get(card_id);
  if (!current_opt) throw std::runtime_error("card not found: " + card_id);
  const auto& current = *current_opt;
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
  const std::string expected = holder::core::card_rel_path(card_id);
  if (current.rel_path != expected &&
      !(current.deleted_at && current.rel_path == holder::core::card_trash_rel_path(card_id))) {
    throw std::runtime_error("card rel_path does not match card_id");
  }

  holder::git::GitRepo history_repo;
  history_repo.open_existing(project.root_path);
  auto raw = history_repo.read_blob_at(historical_oid, expected);
  const bool historical_is_trash = !raw.has_value();
  if (!raw.has_value()) {
    raw = history_repo.read_blob_at(historical_oid, holder::core::card_trash_rel_path(card_id));
  }
  if (!raw.has_value()) throw std::runtime_error("historical card content is missing");

  const auto plain = decode_card_blob(project, *raw);
  if (plain.find('\0') != std::string::npos) {
    throw std::runtime_error("historical card content is binary");
  }
  auto parsed = holder::core::parse_card_file(plain);
  if (!parsed.has_front_matter || parsed.card.card_id != card_id ||
      parsed.card.project_id != current.project_id) {
    throw std::runtime_error("historical card content is malformed");
  }

  auto restored = parsed.card;
  restored.card_id = current.card_id;
  restored.project_id = current.project_id;
  restored.rel_path = expected;
  restored.created_at = current.created_at;
  restored.updated_at = updated_at;
  restored.deleted_at = historical_is_trash ? std::optional<long long>{updated_at}
                                            : std::optional<long long>{};
  for (auto& link : parsed.links) {
    link.project_id = restored.project_id;
    link.from_card_id = restored.card_id;
  }
  for (auto& milestone : parsed.milestones) {
    milestone.project_id = restored.project_id;
    milestone.card_id = restored.card_id;
    milestone.updated_at = updated_at;
  }

  auto placements = restored.deleted_at ? promoted_card_children(card_repo_, current, updated_at)
                                        : restored_card_placement(card_repo_, restored, updated_at);
  parsed.card = restored;
  apply_lifecycle(
      project,
      current,
      std::move(parsed),
      placements,
      "Restore card " + restored.title
  );
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

  const auto project = require_project(card.project_id);
  auto operation = git_->lock_operation(project.root_path);
  git_->open_or_init(project.root_path);
  if (project.git_remote_url.has_value()) git_->set_remote("origin", *project.git_remote_url);
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
  const auto card = card_repo_.get(card_id);
  if (!card) return std::nullopt;
  const auto project = require_project(card->project_id);
  auto operation = git_->lock_operation(project.root_path);
  return card_repo_.get(card_id);
}

std::optional<std::string> CardStore::get_content(const holder::model::Card& card) {
  const auto project = require_project(card.project_id);
  auto operation = git_->lock_operation(project.root_path);
  git_->open_or_init(project.root_path);
  return read_card_content_locked(*fs_, *git_, project, card);
}

CompleteCardPage CardStore::list_complete_page(
    const std::string& project_id,
    const std::optional<std::string>& after_card_id,
    int limit
) {
  if (limit <= 0) {
    throw std::invalid_argument("limit must be positive");
  }

  const auto project = require_project(project_id);
  // Match every existing CardStore operation's lock order: resolve the project first, then
  // acquire the per-repository guard before repository, filesystem, or card-row work.
  auto operation = git_->lock_operation(project.root_path);
  git_->open_or_init(project.root_path);

  auto metadata = card_repo_.list_page_by_card_id(project_id, after_card_id, limit + 1);
  CompleteCardPage page;
  if (metadata.size() > static_cast<std::size_t>(limit)) {
    metadata.resize(static_cast<std::size_t>(limit));
    page.next_cursor = metadata.back().card_id;
  }

  page.cards.reserve(metadata.size());
  for (auto& card : metadata) {
    auto content = read_card_content_locked(*fs_, *git_, project, card);
    if (!content.has_value()) {
      throw std::runtime_error("card content missing: " + card.card_id);
    }
    page.cards.push_back({std::move(card), std::move(*content)});
  }
  return page;
}

AddTagResult CardStore::add_tag(
    const std::string& card_id,
    const std::string& tag,
    long long updated_at
) {
  if (!holder::core::is_valid_tag(tag)) {
    return AddTagResult::InvalidTag;
  }
  const auto normalized = holder::core::normalize_tag(tag);

  const auto card_opt = get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto project = require_project(card_opt->project_id);
  auto operation = git_->lock_operation(project.root_path);
  const auto content = get_content(card_opt.value()).value_or("");

  const auto existing = holder::core::extract_tags(content);
  if (std::find(existing.begin(), existing.end(), normalized) != existing.end()) {
    return AddTagResult::AlreadyPresent;
  }

  const auto new_content = holder::core::upsert_trailing_tag_line(content, normalized);
  update_content(card_id, new_content, std::nullopt, updated_at);
  return AddTagResult::Added;
}

RemoveTagResult CardStore::remove_tag(
    const std::string& card_id,
    const std::string& tag,
    long long updated_at
) {
  if (!holder::core::is_valid_tag(tag)) {
    return RemoveTagResult::InvalidTag;
  }
  const auto normalized = holder::core::normalize_tag(tag);

  const auto card_opt = get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto project = require_project(card_opt->project_id);
  auto operation = git_->lock_operation(project.root_path);
  const auto content = get_content(card_opt.value()).value_or("");

  const auto removal = holder::core::remove_from_trailing_tag_line(content, normalized);
  if (removal.outcome != holder::core::RemoveTagLineOutcome::Removed) {
    const auto existing = holder::core::extract_tags(content);
    const bool present = std::find(existing.begin(), existing.end(), normalized) != existing.end();
    return present ? RemoveTagResult::PresentOutsideEditableTagLine : RemoveTagResult::NotPresent;
  }

  update_content(card_id, removal.new_body, std::nullopt, updated_at);
  return RemoveTagResult::Removed;
}

std::vector<std::string> CardStore::list_editable_tags(const std::string& card_id) {
  const auto card_opt = get(card_id);
  if (!card_opt.has_value()) {
    throw std::runtime_error("card not found: " + card_id);
  }
  const auto content = get_content(card_opt.value()).value_or("");
  return holder::core::tags_on_trailing_line(content);
}

} // namespace holder::card
