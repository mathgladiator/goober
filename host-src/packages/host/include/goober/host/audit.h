#pragma once

// The operations audit (design/machine/028.Panel.md § 8): what a deploy checks about how the
// host is run, beyond whether it is up. Each check has an id; a finding is ok, a warning, a
// suggestion with the steps to take, or quiet when host.json's audit.quiet names it.

#include <functional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "goober/config/config.h"

namespace goober::host {

// Every check, by id: the ids audit.quiet may name.
inline const std::vector<std::string> kAuditChecks = {"panel", "caddy", "mailer", "backup", "operators"};

struct AuditFinding {
  std::string id;
  std::string level;  // "ok", "warn", "suggest", or "quiet"
  std::string title;
  std::vector<std::string> steps;
  boost::json::object data;  // what a script needs to act on it (the panel's domain, its DNS)
};

// A name's addresses, as text; empty when it doesn't resolve.
using Resolver = std::function<std::vector<std::string>(const std::string& name)>;
std::vector<std::string> resolve_addresses(const std::string& name);

// The site names a Caddyfile serves: each site block's addresses, without scheme or port.
std::vector<std::string> caddy_sites(const std::string& caddyfile);

// The audit of one config. `caddyfile` is the proxy's Caddyfile, or "" when unknown (the
// caddy check is then skipped).
std::vector<AuditFinding> audit_operations(const config::HostConfig& config, const std::string& caddyfile,
                                           const Resolver& resolve);

boost::json::object audit_json(const std::vector<AuditFinding>& findings);

// `goober-host audit [--config host.json] [--caddyfile PATH] [--json]`: prints the findings.
// Exits 0 whatever it finds (an audit advises; it never fails a deploy), 1 on a bad config.
int run_audit(const std::string& config_path, const std::string& caddyfile_path, bool json);

}  // namespace goober::host
