#include "card/CardFrontMatter.h"

#include <yaml-cpp/yaml.h>

#include <string_view>

namespace holder::core {
namespace {

constexpr std::string_view kLfOpeningDelimiter = "---\n";
constexpr std::string_view kLfClosingDelimiter = "\n---\n";
constexpr std::string_view kCrlfOpeningDelimiter = "---\r\n";
constexpr std::string_view kCrlfClosingDelimiter = "\r\n---\r\n";

} // namespace

std::string render_card_front_matter(
    const holder::model::Card& card,
    const std::vector<holder::model::CardLink>& links,
    const std::vector<holder::model::Milestone>& milestones
) {
  YAML::Emitter out;
  out << YAML::BeginMap;
  out << YAML::Key << "card_id" << YAML::Value << card.card_id;
  out << YAML::Key << "project_id" << YAML::Value << card.project_id;
  out << YAML::Key << "title" << YAML::Value << card.title;
  out << YAML::Key << "created_at" << YAML::Value << card.created_at;
  out << YAML::Key << "updated_at" << YAML::Value << card.updated_at;
  out << YAML::Key << "parent_card_id" << YAML::Value;
  if (card.parent_card_id.has_value()) {
    out << card.parent_card_id.value();
  } else {
    out << YAML::Null;
  }
  out << YAML::Key << "sort_key" << YAML::Value << card.sort_key;
  out << YAML::Key << "rel_path" << YAML::Value << card.rel_path;
  out << YAML::Key << "deleted_at" << YAML::Value;
  if (card.deleted_at.has_value()) {
    out << card.deleted_at.value();
  } else {
    out << YAML::Null;
  }
  out << YAML::Key << "links" << YAML::Value << YAML::BeginSeq;
  for (const auto& link : links) {
    out << YAML::BeginMap;
    out << YAML::Key << "to" << YAML::Value << link.to_card_id;
    out << YAML::Key << "to_type" << YAML::Value
        << (link.to_type.empty() ? std::string("card") : link.to_type);
    out << YAML::Key << "kind" << YAML::Value << link.kind;
    out << YAML::Key << "created_at" << YAML::Value << link.created_at;
    if (link.label.has_value()) {
      out << YAML::Key << "label" << YAML::Value << link.label.value();
    } else {
      out << YAML::Key << "label" << YAML::Value << YAML::Null;
    }
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
  out << YAML::Key << "milestones" << YAML::Value << YAML::BeginSeq;
  for (const auto& milestone : milestones) {
    out << YAML::BeginMap;
    out << YAML::Key << "milestone_id" << YAML::Value << milestone.milestone_id;
    out << YAML::Key << "start_at" << YAML::Value << milestone.start_at;
    out << YAML::Key << "end_at" << YAML::Value;
    if (milestone.end_at.has_value()) {
      out << milestone.end_at.value();
    } else {
      out << YAML::Null;
    }
    out << YAML::Key << "all_day" << YAML::Value << milestone.all_day;
    out << YAML::Key << "kind" << YAML::Value;
    if (milestone.kind.has_value()) {
      out << milestone.kind.value();
    } else {
      out << YAML::Null;
    }
    out << YAML::Key << "description" << YAML::Value;
    if (milestone.description.has_value()) {
      out << milestone.description.value();
    } else {
      out << YAML::Null;
    }
    out << YAML::Key << "created_at" << YAML::Value << milestone.created_at;
    out << YAML::Key << "updated_at" << YAML::Value << milestone.updated_at;
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
  out << YAML::EndMap;

  return std::string("---\n") + out.c_str() + "\n---\n";
}

ParsedCardFile parse_card_file(const std::string& raw) {
  ParsedCardFile parsed;
  parsed.body = raw;

  std::string_view opening_delimiter;
  std::string_view closing_delimiter;
  if (raw.rfind(kCrlfOpeningDelimiter, 0) == 0) {
    opening_delimiter = kCrlfOpeningDelimiter;
    closing_delimiter = kCrlfClosingDelimiter;
  } else if (raw.rfind(kLfOpeningDelimiter, 0) == 0) {
    opening_delimiter = kLfOpeningDelimiter;
    closing_delimiter = kLfClosingDelimiter;
  } else {
    return parsed;
  }

  const auto end = raw.find(closing_delimiter, opening_delimiter.size());
  if (end == std::string::npos) {
    return parsed;
  }

  const std::string yaml_text =
      raw.substr(opening_delimiter.size(), end - opening_delimiter.size());
  try {
    const auto node = YAML::Load(yaml_text);
    if (!node || !node.IsMap()) {
      return parsed;
    }

    parsed.has_front_matter = true;
    auto& card = parsed.card;

    if (node["card_id"]) card.card_id = node["card_id"].as<std::string>();
    if (node["project_id"]) card.project_id = node["project_id"].as<std::string>();
    if (node["title"]) card.title = node["title"].as<std::string>();
    if (node["rel_path"]) card.rel_path = node["rel_path"].as<std::string>();
    if (node["created_at"]) card.created_at = node["created_at"].as<long long>();
    if (node["updated_at"]) card.updated_at = node["updated_at"].as<long long>();
    if (node["sort_key"]) card.sort_key = node["sort_key"].as<double>();

    if (node["parent_card_id"] && !node["parent_card_id"].IsNull()) {
      card.parent_card_id = node["parent_card_id"].as<std::string>();
    }
    if (node["deleted_at"] && !node["deleted_at"].IsNull()) {
      card.deleted_at = node["deleted_at"].as<long long>();
    }

    if (node["links"] && node["links"].IsSequence()) {
      for (const auto& item : node["links"]) {
        if (!item.IsMap()) continue;
        holder::model::CardLink link;
        link.project_id = card.project_id;
        link.from_card_id = card.card_id;
        if (item["to"]) link.to_card_id = item["to"].as<std::string>();
        if (item["to_type"]) link.to_type = item["to_type"].as<std::string>();
        if (item["kind"]) link.kind = item["kind"].as<std::string>();
        if (link.to_type.empty()) link.to_type = "card";
        if (link.kind.empty()) link.kind = "ref";
        if (item["label"] && !item["label"].IsNull()) {
          link.label = item["label"].as<std::string>();
        }
        if (item["created_at"]) {
          link.created_at = item["created_at"].as<long long>();
        }
        if (!link.to_card_id.empty()) {
          parsed.links.push_back(std::move(link));
        }
      }
    }

    if (node["milestones"] && node["milestones"].IsSequence()) {
      for (const auto& item : node["milestones"]) {
        if (!item.IsMap()) continue;
        holder::model::Milestone milestone;
        milestone.project_id = card.project_id;
        milestone.card_id = card.card_id;
        if (item["milestone_id"]) milestone.milestone_id = item["milestone_id"].as<std::string>();
        if (item["start_at"]) milestone.start_at = item["start_at"].as<long long>();
        if (item["end_at"] && !item["end_at"].IsNull()) {
          milestone.end_at = item["end_at"].as<long long>();
        }
        if (item["all_day"]) milestone.all_day = item["all_day"].as<bool>();
        if (item["kind"] && !item["kind"].IsNull()) {
          milestone.kind = item["kind"].as<std::string>();
        }
        if (item["description"] && !item["description"].IsNull()) {
          milestone.description = item["description"].as<std::string>();
        }
        if (item["created_at"]) milestone.created_at = item["created_at"].as<long long>();
        if (item["updated_at"]) milestone.updated_at = item["updated_at"].as<long long>();
        if (!milestone.milestone_id.empty()) {
          parsed.milestones.push_back(std::move(milestone));
        }
      }
    }

    parsed.body = raw.substr(end + closing_delimiter.size());
    return parsed;
  } catch (const std::exception&) {
    return parsed;
  }
}

} // namespace holder::core
