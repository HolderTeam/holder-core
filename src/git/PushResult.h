#pragma once

#include <cstdint>
#include <string>

namespace holder::git {

enum class PushStatus : std::uint8_t {
  Pushed,
  UpToDate,
  AuthFailed,
  NotFound,
  NetworkError,
  NonFastForward,
  RemoteUnset,
  UnknownError,
};

struct PushResult {
  PushStatus status = PushStatus::UnknownError;
  int ahead_count = 0;
  int behind_count = 0;
  std::string local_head_commit;
  std::string error_message;
};

inline const char* push_status_name(PushStatus status) {
  switch (status) {
  case PushStatus::Pushed:
    return "pushed";
  case PushStatus::UpToDate:
    return "up_to_date";
  case PushStatus::AuthFailed:
    return "auth_failed";
  case PushStatus::NotFound:
    return "not_found";
  case PushStatus::NetworkError:
    return "network_error";
  case PushStatus::NonFastForward:
    return "non_fast_forward";
  case PushStatus::RemoteUnset:
    return "remote_unset";
  case PushStatus::UnknownError:
  default:
    return "unknown_error";
  }
}

} // namespace holder::git
