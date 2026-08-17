#include "ai/AiMessageFrontMatter.h"

#include <yaml-cpp/yaml.h>

#include <string_view>

namespace holder::core {
namespace {

constexpr std::string_view kDelimiter = "---\n";

} // namespace

std::string render_ai_message_front_matter(
    const holder::model::AiMessage& message,
    const std::string& project_id,
    const std::vector<holder::model::CardLink>& links
) {
  YAML::Emitter out;
  out << YAML::BeginMap;
  out << YAML::Key << "message_id" << YAML::Value << message.message_id;
  out << YAML::Key << "project_id" << YAML::Value << project_id;
  out << YAML::Key << "thread_id" << YAML::Value << message.thread_id;
  out << YAML::Key << "role" << YAML::Value << message.role;
  out << YAML::Key << "source" << YAML::Value << message.source;
  out << YAML::Key << "provider" << YAML::Value;
  if (message.provider.has_value()) {
    out << message.provider.value();
  } else {
    out << YAML::Null;
  }
  out << YAML::Key << "model" << YAML::Value;
  if (message.model.has_value()) {
    out << message.model.value();
  } else {
    out << YAML::Null;
  }
  out << YAML::Key << "created_at" << YAML::Value << message.created_at;
  out << YAML::Key << "prompt_hash" << YAML::Value;
  if (message.prompt_hash.has_value()) {
    out << message.prompt_hash.value();
  } else {
    out << YAML::Null;
  }
  out << YAML::Key << "meta_json" << YAML::Value;
  if (message.meta_json.has_value()) {
    out << message.meta_json.value();
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

ParsedAiMessageFile parse_ai_message_file(const std::string& raw) {
  ParsedAiMessageFile parsed;
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
    auto& msg = parsed.message;

    if (node["message_id"]) msg.message_id = node["message_id"].as<std::string>();
    if (node["thread_id"]) msg.thread_id = node["thread_id"].as<std::string>();
    if (node["project_id"]) parsed.project_id = node["project_id"].as<std::string>();
    if (node["role"]) msg.role = node["role"].as<std::string>();
    if (node["source"]) msg.source = node["source"].as<std::string>();
    if (node["created_at"]) msg.created_at = node["created_at"].as<long long>();

    if (node["provider"] && !node["provider"].IsNull()) {
      msg.provider = node["provider"].as<std::string>();
    }
    if (node["model"] && !node["model"].IsNull()) {
      msg.model = node["model"].as<std::string>();
    }
    if (node["prompt_hash"] && !node["prompt_hash"].IsNull()) {
      msg.prompt_hash = node["prompt_hash"].as<std::string>();
    }
    if (node["meta_json"] && !node["meta_json"].IsNull()) {
      msg.meta_json = node["meta_json"].as<std::string>();
    }

    if (node["links"] && node["links"].IsSequence()) {
      for (const auto& item : node["links"]) {
        if (!item.IsMap()) continue;
        holder::model::CardLink link;
        link.project_id = parsed.project_id;
        link.from_card_id = msg.message_id;
        if (item["to"]) link.to_card_id = item["to"].as<std::string>();
        if (item["to_type"]) link.to_type = item["to_type"].as<std::string>();
        if (link.to_type.empty()) link.to_type = "card";
        if (item["kind"]) link.kind = item["kind"].as<std::string>();
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
