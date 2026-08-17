#include "card/CardFrontMatter.h"

#include <yaml-cpp/yaml.h>

#include <string_view>

namespace holder::core {
namespace {

constexpr std::string_view kDelimiter = "---\n";

} // namespace

std::string render_card_front_matter(
    const holder::model::Card& card,
    const std::vector<holder::model::CardLink>& links
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
  out << YAML::EndMap;

  return std::string("---\n") + out.c_str() + "\n---\n";
}

ParsedCardFile parse_card_file(const std::string& raw) {
  ParsedCardFile parsed;
  parsed.body = raw;

  if (raw.rfind(kDelimiter, 0) != 0) {
    return parsed;
  }

  const auto end = raw.find("\n---\n", kDelimiter.size());
  if (end == std::string::npos) {
    return parsed;
  }

  const std::string yaml_text = raw.substr(kDelimiter.size(), end - kDelimiter.size());
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

    parsed.body = raw.substr(end + 5);
    return parsed;
  } catch (const std::exception&) {
    return parsed;
  }
}

} // namespace holder::core
