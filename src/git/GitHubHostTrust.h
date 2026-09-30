#pragma once

#include <git2.h>

namespace holder::git {

// Trust GitHub's published SSH fingerprints on first use. Other hosts and
// HTTPS certificates retain libgit2's normal validation.
int check_github_host_certificate(git_cert* certificate, int valid, const char* host, void* payload);

} // namespace holder::git
