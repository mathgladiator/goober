#include "goober/host/audit.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <iostream>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/json/json.h"

namespace goober::host {

namespace bj = boost::json;

std::vector<std::string> resolve_addresses(const std::string& name) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  std::set<std::string> out;
  if (getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0) return {};
  for (auto* a = res; a; a = a->ai_next) {
    char text[INET6_ADDRSTRLEN] = {};
    const void* at = a->ai_family == AF_INET ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr)
                                             : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(a->ai_addr)->sin6_addr);
    if (inet_ntop(a->ai_family, at, text, sizeof text)) out.insert(text);
  }
  freeaddrinfo(res);
  return {out.begin(), out.end()};
}

std::vector<std::string> caddy_sites(const std::string& caddyfile) {
  std::vector<std::string> sites;
  size_t start = 0;
  while (start < caddyfile.size()) {
    size_t end = caddyfile.find('\n', start);
    if (end == std::string::npos) end = caddyfile.size();
    std::string line = caddyfile.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
    // A site block opens at the start of a line: addresses, then "{". Not the global options
    // ("{" alone), a snippet ("(name) {"), or anything indented.
    if (line.size() < 2 || line.back() != '{' || line[0] == ' ' || line[0] == '\t' || line[0] == '#' ||
        line[0] == '{' || line[0] == '(') {
      continue;
    }
    line.pop_back();
    for (char& c : line) c = c == ',' ? ' ' : c;
    size_t at = 0;
    while (at < line.size()) {
      size_t s = line.find_first_not_of(" \t", at);
      if (s == std::string::npos) break;
      size_t e = line.find_first_of(" \t", s);
      std::string addr = line.substr(s, e == std::string::npos ? std::string::npos : e - s);
      at = e == std::string::npos ? line.size() : e;
      if (auto scheme = addr.find("://"); scheme != std::string::npos) addr = addr.substr(scheme + 3);
      addr = addr.substr(0, addr.find_first_of(":/"));
      if (!addr.empty()) sites.push_back(base::to_lower(addr));
    }
  }
  return sites;
}

namespace {

// Whether Caddy's sites cover a name: listed, or under a wildcard (*.example.com covers one
// label in front of example.com).
bool served(const std::vector<std::string>& sites, const std::string& name) {
  for (const auto& s : sites) {
    if (s == name) return true;
    if (s.starts_with("*.")) {
      auto dot = name.find('.');
      if (dot != std::string::npos && dot > 0 && name.substr(dot + 1) == s.substr(2)) return true;
    }
  }
  return false;
}

bool is_public_name(const std::string& d) { return d.find('.') != std::string::npos && !d.ends_with("localhost"); }

// Whether every address of `name` is one of `ours`: "ready", "missing" (no address),
// "elsewhere" (some address isn't ours), or "unknown" (we have no address to compare with).
std::string dns_state(const std::vector<std::string>& theirs, const std::vector<std::string>& ours) {
  if (theirs.empty()) return "missing";
  if (ours.empty()) return "unknown";
  for (const auto& a : theirs) {
    if (std::find(ours.begin(), ours.end(), a) == ours.end()) return "elsewhere";
  }
  return "ready";
}

std::string records_for(const std::string& name, const std::vector<std::string>& addresses) {
  std::string out;
  for (const auto& a : addresses) {
    out += (out.empty() ? "" : "; ") + std::string(a.find(':') == std::string::npos ? "A " : "AAAA ") + name + " -> " + a;
  }
  return out.empty() ? "an A record for " + name + " -> this server's address" : out;
}

bj::array strings(const std::vector<std::string>& v) {
  bj::array a;
  for (const auto& s : v) a.emplace_back(s);
  return a;
}

AuditFinding panel_finding(const config::HostConfig& config, const std::string& site, const Resolver& resolve) {
  AuditFinding f{"panel", "ok", "", {}, {}};
  auto ours = resolve(site);
  if (!config.operations_domain.empty()) {
    const auto& d = config.operations_domain;
    auto theirs = resolve(d);
    std::string dns = dns_state(theirs, ours);
    f.data = {{"domain", d}, {"dns", dns}, {"addresses", strings(theirs)}};
    if (dns == "missing") {
      f.level = "warn";
      f.title = "the panel's domain " + d + " doesn't resolve";
      f.steps.push_back("Add DNS: " + records_for(d, ours));
    } else if (dns == "elsewhere") {
      f.level = "warn";
      f.title = "the panel's domain " + d + " points somewhere other than " + site;
      f.steps.push_back("Point it here: " + records_for(d, ours));
    } else {
      f.title = "the panel is at https://" + d + "/";
    }
    return f;
  }
  std::string d = "ops." + site;
  auto theirs = resolve(d);
  std::string dns = dns_state(theirs, ours);
  bj::array others;
  for (const auto& [name, entry] : config.domains) {
    if (name != site && is_public_name(name)) others.emplace_back("ops." + name);
  }
  f.level = "suggest";
  f.title = "no operator panel: suggest https://" + d + "/ (traffic, boxes, changes, what Grok did)";
  f.data = {{"domain", d}, {"dns", dns}, {"addresses", strings(ours)}, {"alternatives", others}};
  if (dns != "ready") {
    f.steps.push_back("Add DNS: " + records_for(d, ours) +
                      (dns == "elsewhere" ? " (it resolves elsewhere now)" : ""));
  }
  f.steps.push_back("Set \"operations\": {\"domain\": \"" + d + "\"} in host.json, add " + d +
                    " to the Caddyfile's site line, and restart both; a deploy offers to do this once DNS is ready");
  if (!others.empty()) {
    f.steps.push_back("One panel serves every tenant; ops. on another domain works as well, like " +
                      std::string(others.at(0).as_string()));
  }
  f.steps.push_back("Not wanted: add \"audit\": {\"quiet\": [\"panel\"]} to host.json, or answer \"never\" at the deploy prompt");
  return f;
}

}  // namespace

std::vector<AuditFinding> audit_operations(const config::HostConfig& config, const std::string& caddyfile,
                                           const Resolver& resolve) {
  std::vector<AuditFinding> out;
  auto quiet = [&](const std::string& id) {
    return std::find(config.audit_quiet.begin(), config.audit_quiet.end(), id) != config.audit_quiet.end();
  };
  auto add = [&](AuditFinding f) {
    if (quiet(f.id) && f.level != "ok") {
      f.level = "quiet";
      f.title = "not suggested (audit.quiet in host.json): " + f.title;
      f.steps.clear();
    }
    out.push_back(std::move(f));
  };
  for (const auto& id : config.audit_quiet) {
    if (std::find(kAuditChecks.begin(), kAuditChecks.end(), id) == kAuditChecks.end()) {
      out.push_back({"audit", "warn", "audit.quiet names \"" + id + "\", which no check has", {}, {}});
    }
  }

  // The site this server answers as: the platform domain, or the first public domain.
  std::string site = is_public_name(config.platform_domain) ? config.platform_domain : "";
  for (const auto& [name, entry] : config.domains) {
    if (site.empty() && is_public_name(name)) site = name;
  }

  if (!site.empty()) add(panel_finding(config, site, resolve));

  if (!caddyfile.empty()) {
    auto sites = caddy_sites(caddyfile);
    std::vector<std::string> missing;
    for (const auto& [name, entry] : config.domains) {
      if (is_public_name(name) && !served(sites, name)) missing.push_back(name);
    }
    if (!config.operations_domain.empty() && !served(sites, config.operations_domain)) {
      missing.push_back(config.operations_domain);
    }
    AuditFinding f{"caddy", "ok", "Caddy serves every domain in host.json", {}, {{"missing", strings(missing)}}};
    if (!missing.empty()) {
      f.level = "warn";
      f.title = "Caddy doesn't serve ";
      for (size_t i = 0; i < missing.size(); ++i) f.title += (i ? ", " : "") + missing[i];
      f.title += ": no certificate, so browsers and agents can't reach it";
      f.steps.push_back("Add it to the site line of /etc/caddy/Caddyfile, then: sudo systemctl reload caddy");
    }
    add(std::move(f));
  }

  if (config.identity.mailer == "dev") {
    add({"mailer", "warn", "identity.mailer is \"dev\": sign-in codes and mail stay in storage/mail/, and nobody gets them",
         {"Set identity.mailer to \"ses\" or \"smtp\" (design/machine/018.Mail.md)"}, {}});
  } else {
    add({"mailer", "ok", "mail goes out through " + config.identity.mailer + " as " + config.identity.from_address(), {}, {}});
  }

  if (!config.backup.enabled()) {
    add({"backup", "suggest", "backups stay on this machine: a lost disk loses everything",
         {"Set \"backup\": {\"s3\": {\"bucket\": \"...\"}} and the AWS keys (design/machine/019.Backups.md)"}, {}});
  } else {
    add({"backup", "ok", "backups go to s3://" + config.backup.bucket + "/" + config.backup.prefix, {}, {}});
  }

  if (config.identity.operators.empty()) {
    add({"operators", "warn", "identity.operators is empty: nobody can sign in to the panel or connect an agent as operator",
         {"Add \"identity\": {\"operators\": {\"you@example.com\": [\"host:owner\"]}} to host.json"}, {}});
  } else {
    add({"operators", "ok", std::to_string(config.identity.operators.size()) + " host operator(s)", {}, {}});
  }
  return out;
}

bj::object audit_json(const std::vector<AuditFinding>& findings) {
  bj::array list;
  for (const auto& f : findings) {
    list.push_back(bj::object{{"id", f.id}, {"level", f.level}, {"title", f.title}, {"steps", strings(f.steps)}, {"data", f.data}});
  }
  return {{"findings", std::move(list)}};
}

int run_audit(const std::string& config_path, const std::string& caddyfile_path, bool json) {
  std::string err;
  auto config = config::load_config(config_path, &err);
  if (!config) return std::cerr << "goober-host: " << err << "\n", 1;
  std::string caddyfile;
  if (!caddyfile_path.empty()) {
    auto text = base::read_file(caddyfile_path);
    if (!text) return std::cerr << "goober-host: can't read " << caddyfile_path << "\n", 1;
    caddyfile = *text;
  }
  auto findings = audit_operations(*config, caddyfile, resolve_addresses);
  if (json) {
    std::cout << json::dump(audit_json(findings)) << "\n";
    return 0;
  }
  for (const auto& f : findings) {
    const char* mark = f.level == "ok" ? "✓" : f.level == "warn" ? "!" : f.level == "suggest" ? "?" : "·";
    std::cout << "  " << mark << " " << f.id << ": " << f.title << "\n";
    for (const auto& s : f.steps) std::cout << "      - " << s << "\n";
  }
  return 0;
}

}  // namespace goober::host
