#pragma once

#include <cstdint>
#include <string>

namespace holder::git {

enum class RemoteProbeStatus : std::uint8_t {
  Reachable,
  AuthFailed,
  NotFound,
  NetworkError,
  InvalidRemoteUrl,
  RemoteUnset,
  UnknownError,
};

struct RemoteProbeResult {
  RemoteProbeStatus status = RemoteProbeStatus::UnknownError;
  bool remote_has_head = false;
  std::string error_message;
};

inline const char* remote_probe_status_name(RemoteProbeStatus status) {
  switch (status) {
  case RemoteProbeStatus::Reachable:
    return "reachable";
  case RemoteProbeStatus::AuthFailed:
    return "auth_failed";
  case RemoteProbeStatus::NotFound:
    return "not_found";
  case RemoteProbeStatus::NetworkError:
    return "network_error";
  case RemoteProbeStatus::InvalidRemoteUrl:
    return "invalid_remote_url";
  case RemoteProbeStatus::RemoteUnset:
    return "remote_unset";
  case RemoteProbeStatus::UnknownError:
  default:
    return "unknown_error";
  }
}

} // namespace holder::git
