#include "card/LinkKindCatalog.h"

namespace holder::core {

const std::vector<LinkKindInfo>& link_kind_catalog() {
  static const std::vector<LinkKindInfo> kCatalog = {
      // General / symmetric
      {"related_to", "Related to", "Related to"},
      {"similar_to", "Similar to", "Similar to"},
      {"opposite_of", "Opposite of", "Opposite of"},
      {"alternative_to", "Alternative to", "Alternative to"},
      {"compatible_with", "Compatible with", "Compatible with"},
      {"incompatible_with", "Incompatible with", "Incompatible with"},
      {"conflicts_with", "Conflicts with", "Conflicts with"},
      {"agrees_with", "Agrees with", "Agrees with"},
      {"disagrees_with", "Disagrees with", "Disagrees with"},

      // Dependency & blocking
      {"requires", "Requires", "Required by"},
      {"depends_on", "Depends on", "Required by"},
      {"needs", "Needs", "Needed by"},
      {"blocks", "Blocks", "Blocked by"},
      {"unblocks", "Unblocks", "Unblocked by"},
      {"prevents", "Prevents", "Prevented by"},
      {"enables", "Enables", "Enabled by"},
      {"waiting_for", "Waiting for", "Awaited by"},

      // Sequence & causality
      {"precedes", "Precedes", "Follows"},
      {"before", "Before", "After"},
      {"during", "During", "Contains event"},
      {"causes", "Causes", "Caused by"},
      {"caused_by", "Caused by", "Causes"},
      {"resolves", "Resolves", "Resolved by"},

      // Evidence & argument
      {"supports", "Supports", "Supported by"},
      {"contradicts", "Contradicts", "Contradicted by"},
      {"explains", "Explains", "Explained by"},
      {"motivates", "Motivates", "Motivated by"},
      {"justifies", "Justifies", "Justified by"},
      {"challenges", "Challenges", "Challenged by"},
      {"invalidates", "Invalidates", "Invalidated by"},
      {"verifies", "Verifies", "Verified by"},
      {"tests", "Tests", "Tested by"},

      // Documentation & reference
      {"ref", "References", "Referenced by"},
      {"describes", "Describes", "Described by"},
      {"defines", "Defines", "Defined by"},
      {"summarises", "Summarises", "Summarised by"},
      {"quotes", "Quotes", "Quoted by"},
      {"cites", "Cites", "Cited by"},
      {"mentions", "Mentions", "Mentioned by"},
      {"about", "About", "Subject of"},
      {"answers", "Answers", "Answered by"},
      {"asks", "Asks", "Asked by"},
      {"documents", "Documents", "Documented by"},
      {"authored_by", "Authored by", "Authored"},

      // Composition & taxonomy
      {"example_of", "Example of", "Has example"},
      {"instance_of", "Instance of", "Has instance"},
      {"type_of", "Type of", "Has type"},
      {"part_of", "Part of", "Has part"},
      {"contains", "Contains", "Contained by"},
      {"includes", "Includes", "Included in"},
      {"belongs_to", "Belongs to", "Has member"},
      {"duplicates", "Duplicates", "Duplicated by"},
      {"replaces", "Replaces", "Replaced by"},
      {"supersedes", "Supersedes", "Superseded by"},
      {"extends", "Extends", "Extended by"},
      {"implements", "Implements", "Implemented by"},
      {"inside_of", "Inside of", "Outside of"},
      {"merges_into", "Merges into", "Merge of"},

      // Process (input/output)
      {"produces", "Produces", "Produced by"},
      {"creates", "Creates", "Created by"},
      {"transforms", "Transforms", "Transformed by"},
      {"consumes", "Consumes", "Consumed by"},
      {"input_to", "Input to", "Has input"},
      {"output_of", "Output of", "Has output"},
      {"configures", "Configures", "Configured by"},

      // Provenance & inspiration
      {"derived_from", "Derived from", "Source of"},
      {"originates_from", "Originates from", "Origin of"},
      {"inspired_by", "Inspired by", "Inspired"},
      {"based_on", "Based on", "Basis for"},
      {"adapted_from", "Adapted from", "Adapted as"},

      // Ownership & workflow
      {"owned_by", "Owned by", "Owns"},
      {"assigned_to", "Assigned to", "Assigned"},
      {"requested_by", "Requested by", "Requested"},
      {"reported_by", "Reported by", "Reported"},
      {"reviewed_by", "Reviewed by", "Reviewed"},
      {"approved_by", "Approved by", "Approved"},
      {"authorized_by", "Authorized by", "Authorizes"},
      {"subtask_of", "Subtask of", "Has subtask"},
      {"milestone_for", "Milestone for", "Has milestone"},
      {"goal_of", "Goal of", "Has goal"},
      {"contributes_to", "Contributes to", "Has contribution"},
      {"uses", "Uses", "Used by"},
      {"fixes", "Fixes", "Fixed by"},
      {"regresses", "Regresses", "Regressed by"},
      {"deprecates", "Deprecates", "Deprecated by"},
      {"mitigates", "Mitigates", "Mitigated by"},
      {"tracks", "Tracks", "Tracked by"},
      {"escalates", "Escalates", "Escalated by"},
      {"scheduled_for", "Scheduled for", "Has schedule"},
      {"triggers", "Triggers", "Triggered by"},

      // Risk, security & governance
      {"threatens", "Threatens", "Threatened by"},
      {"protects", "Protects", "Protected by"},
      {"funds", "Funds", "Funded by"},
      {"competes_with", "Competes with", "Competes with"},

      // Spatial, social & personal
      {"located_at", "Located at", "Location of"},
      {"attends", "Attends", "Attended by"},
      {"organizes", "Organizes", "Organized by"},
      {"participates_in", "Participates in", "Has participant"},
      {"teaches", "Teaches", "Taught by"},
      {"reminds_of", "Reminds of", "Reminds of"},
  };
  return kCatalog;
}

}  // namespace holder::core
