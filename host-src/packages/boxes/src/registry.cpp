#include "goober/boxes/registry.h"
#include "goober/mail_inbound/address.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <tuple>

#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/json/json.h"

namespace goober::boxes {
namespace fs = std::filesystem;

const router::RouteTable<RouteRef>& ActiveSet::table_for(const std::string& domain) const {
  static const router::RouteTable<RouteRef> kEmpty;
  auto it = tables_.find(domain);
  if (it != tables_.end()) return it->second;
  it = tables_.find("*");
  return it == tables_.end() ? kEmpty : it->second;
}

const std::map<std::string, RouteRef>& ActiveSet::tools_for(const std::string& domain) const {
  static const std::map<std::string, RouteRef> kEmpty;
  auto it = tools_.find(domain);
  if (it != tools_.end()) return it->second;
  it = tools_.find("*");
  return it == tools_.end() ? kEmpty : it->second;
}

std::optional<RouteRef> ActiveSet::candidate_for(const RouteRef& ref, int64_t bucket) const {
  if (splits.empty() || !ref.box || !ref.route) return std::nullopt;
  auto it = splits.find(ref.box->manifest.box);
  if (it == splits.end() || bucket >= it->second.share_bp) return std::nullopt;
  auto r = it->second.routes.find(ref.route->verb + " " + ref.route->name);
  if (r == it->second.routes.end()) return std::nullopt;
  return RouteRef{it->second.candidate, r->second};
}

std::vector<std::string> split_refusals(const manifest::Manifest& a, const manifest::Manifest& b) {
  std::vector<std::string> out;
  auto routes = [](const manifest::Manifest& m) {
    std::set<std::string> s;
    auto add = [&](const char* kind, const std::vector<manifest::HttpRoute>& list) {
      for (const auto& r : list) s.insert(std::string(kind) + " " + r.verb + " " + r.pattern.source + " " + r.name + (r.mail_address.empty() ? "" : " " + r.mail_address));
    };
    add("http", m.routes);
    add("mcp", m.tools);
    add("timer", m.timers);
    add("internal", m.internal);
    add("identity", m.identity);
    add("mail", m.mail);
    add("result", m.results);
    add("job", m.jobs);
    add("upgrade", m.upgrades);
    return s;
  };
  auto data = [](const manifest::Manifest& m) {
    std::set<std::string> s;
    for (const auto& d : m.data) s.insert(d.name + " " + d.pattern + " " + d.schema + " " + d.database + " " + d.tenant);
    return s;
  };
  auto diff = [&](const std::set<std::string>& x, const std::set<std::string>& y, const char* what) {
    for (const auto& k : x) {
      if (!y.count(k)) out.push_back(std::string("the live version has ") + what + " \"" + k + "\" and the candidate doesn't");
    }
    for (const auto& k : y) {
      if (!x.count(k)) out.push_back(std::string("the candidate has ") + what + " \"" + k + "\" and the live version doesn't");
    }
  };
  diff(routes(a), routes(b), "route");
  diff(data(a), data(b), "data strategy");
  if (a.domains != b.domains) out.push_back("the two versions serve different domains");
  return out;
}

namespace {

int64_t num(const boost::json::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_int64() ? v->as_int64() : 0;
}

bool valid_version(const std::string& v) {
  return !v.empty() && v.size() <= 64 && v.find('/') == std::string::npos && v.find("..") == std::string::npos;
}

// A split as the active set holds it, its routes by "verb name".
std::optional<ActiveSet::Split> split_of(std::shared_ptr<const ActiveBox> candidate, int64_t share_bp, std::string salt) {
  ActiveSet::Split sp;
  sp.share_bp = std::clamp<int64_t>(share_bp, 0, 10000);
  sp.salt = std::move(salt);
  for (const auto* list : {&candidate->manifest.routes, &candidate->manifest.tools}) {
    for (const auto& r : *list) sp.routes[r.verb + " " + r.name] = &r;
  }
  sp.candidate = std::move(candidate);
  return sp;
}

}  // namespace

std::shared_ptr<const ActiveBox> ActiveSet::box_for(const std::string& name, bool on_beta) const {
  if (on_beta) {
    if (auto it = beta.find(name); it != beta.end()) return it->second;
  }
  auto it = boxes.find(name);
  return it == boxes.end() ? nullptr : it->second;
}

std::map<std::string, std::shared_ptr<const ActiveBox>> ActiveSet::versions(bool on_beta) const {
  if (!on_beta) return boxes;
  auto out = boxes;
  for (const auto& [name, box] : beta) out[name] = box;
  return out;
}

bool ActiveSet::build(std::string* error) {
  tables_.clear();
  tools_.clear();
  std::set<std::string> keys = {"*"};
  for (const auto& [_, box] : boxes) {
    for (const auto& d : box->manifest.domains) keys.insert(d);
  }
  // Every beta domain has its own tables, from the beta versions (053 § 3).
  for (const auto& d : beta_domains) keys.insert(d);
  std::map<std::string, std::shared_ptr<const ActiveBox>> on_beta = boxes;
  for (const auto& [name, box] : beta) on_beta[name] = box;
  for (const auto& key : keys) {
    auto& table = tables_[key];
    for (const auto& [name, box] : beta_domains.count(key) ? on_beta : boxes) {
      if (!box->manifest.applies_to(key)) continue;
      for (const auto& route : box->manifest.routes) {
        if (!table.add(route.pattern, route.verb, RouteRef{box, &route})) {
          *error = "route " + route.verb + " " + route.pattern.source + " in box " + name +
                   " conflicts with another box on domain " + key;
          return false;
        }
      }
      // Tool names conflict per domain, like paths (019 § per-protocol overlap rules).
      auto& tools = tools_[key];
      for (const auto& tool : box->manifest.tools) {
        if (!tools.emplace(tool.name, RouteRef{box, &tool}).second) {
          *error = "tool " + tool.name + " in box " + name + " conflicts with another box on domain " + key;
          return false;
        }
      }
    }
  }
  return true;
}

namespace {
// A split the registry ends (a new live version, a disable; 052 § 4): its record moves to
// split-last, as Host::split_end keeps it, so split_report says how it ended.
void end_split(wal::Bundle& bundle, const std::filesystem::path& root, const std::string& box, const std::string& why) {
  auto text = base::read_file(root / "boxes" / box / "split");
  if (!text) return;
  bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/split", ""});
  std::string err;
  auto v = json::parse(*text, &err);
  if (!v || !v->is_object()) return;
  auto last = v->as_object();
  last.erase("salt");
  last["ended_ms"] = base::now_unix_ms();
  last["outcome"] = "ended";
  last["ended_by"] = "host";
  last["end_why"] = why;
  bundle.push_back({wal::FileChange::Kind::kWrite, "boxes/" + box + "/split-last", json::dump(last)});
}

// Production wins over beta (053 § 6): builds `set` with its beta versions left out one by one
// where they conflict, answering the boxes whose beta versions were left out; nullopt when the set
// conflicts even with none (production's own conflict).
std::optional<std::vector<std::string>> fit_beta(ActiveSet& set) {
  auto beta = set.beta;
  set.beta.clear();
  std::string err;
  if (!set.build(&err)) return std::nullopt;
  std::vector<std::string> left;
  for (const auto& [name, box] : beta) {
    set.beta[name] = box;
    if (!set.build(&err)) {
      set.beta.erase(name);
      left.push_back(name);
    }
  }
  set.build(&err);
  return left;
}

std::vector<std::string> exports_of(const manifest::Manifest& m) {
  std::vector<std::string> out;
  for (const auto& r : m.routes) out.push_back(r.export_name());
  for (const auto& t : m.tools) out.push_back(t.export_name());
  for (const auto& t : m.timers) out.push_back(t.export_name());
  for (const auto& r : m.internal) out.push_back(r.export_name());
  for (const auto& r : m.identity) out.push_back(r.export_name());
  for (const auto& r : m.mail) out.push_back(r.export_name());
  for (const auto& r : m.results) out.push_back(r.export_name());
  for (const auto& r : m.jobs) out.push_back(r.export_name());
  for (const auto& r : m.upgrades) out.push_back(r.export_name());
  return out;
}

// The version's types document, parsed and checked against what the manifest names.
std::optional<std::shared_ptr<const types::Types>> load_types(const manifest::Manifest& m, std::string_view text,
                                                               std::string* error) {
  std::shared_ptr<const types::Types> out;
  if (!text.empty()) {
    auto t = types::Types::parse(text, error);
    if (!t) {
      *error = (m.types.empty() ? std::string("types") : m.types) + ": " + *error;
      return std::nullopt;
    }
    out = std::make_shared<const types::Types>(std::move(*t));
  }
  if (!manifest::check_types(m, out.get(), error)) return std::nullopt;
  return out;
}
}  // namespace

BoxRegistry::BoxRegistry(wal::HostWal& wal, runtime::wasm::Engine& engine)
    : wal_(wal), engine_(engine), active_(std::make_shared<ActiveSet>()) {}

std::shared_ptr<const ActiveSet> BoxRegistry::active() const {
  std::lock_guard lock(snapshot_mu_);
  return active_;
}

std::shared_ptr<const ActiveBox> BoxRegistry::load(const std::string& box, const std::string& version,
                                                  std::string* error) const {
  if (archived(box, version, error)) return nullptr;
  auto loaded = load_version(box, version, error);
  if (!loaded) return nullptr;
  return std::make_shared<const ActiveBox>(std::move(*loaded));
}

std::optional<ActiveBox> BoxRegistry::load_version(const std::string& box, const std::string& version,
                                                   std::string* error) const {
  fs::path dir = wal_.root() / "boxes" / box / "versions" / version;
  auto text = base::read_file(dir / "manifest.json");
  auto wasm = base::read_file(dir / "box.wasm");
  if (!text || !wasm) {
    *error = "version " + version + " of " + box + " is missing files";
    return std::nullopt;
  }
  auto m = manifest::parse_manifest(*text, error);
  if (!m) return std::nullopt;
  auto t = load_types(*m, base::read_file(dir / "types.json").value_or(""), error);
  if (!t) return std::nullopt;
  auto compiled = engine_.compile(*wasm, exports_of(*m), error);
  if (!compiled) return std::nullopt;
  return ActiveBox{version, std::move(*m), compiled, *t};
}

std::vector<std::string> BoxRegistry::load_all() {
  std::lock_guard lock(mu_);
  std::vector<std::string> warnings;
  auto next = std::make_shared<ActiveSet>();
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(wal_.root() / "boxes", ec)) {
    std::string box = entry.path().filename().string();
    if (!base::is_valid_name(box)) continue;
    auto active = base::read_file(entry.path() / "active");
    if (!active) continue;
    std::string err;
    auto loaded = load_version(box, *active, &err);
    if (!loaded) {
      warnings.push_back("box " + box + " not loaded: " + err);
      continue;
    }
    next->boxes[box] = std::make_shared<const ActiveBox>(std::move(*loaded));
  }
  std::string err;
  if (!next->build(&err)) {
    // A conflict between stored versions: every box that fits is served, in name order, and
    // only those that conflict with what's already in are left out (by name, in the warnings).
    auto all = next->boxes;
    next = std::make_shared<ActiveSet>();
    for (const auto& [name, box] : all) {
      auto trial = std::make_shared<ActiveSet>(*next);
      trial->boxes[name] = box;
      std::string why;
      if (trial->build(&why)) {
        next = trial;
      } else {
        warnings.push_back("box " + name + " not loaded: a route conflict: " + why);
      }
    }
    next->build(&err);
  }
  // Beta versions (053 § 3), on the beta domains: one that can't be loaded or conflicts there is
  // left out, said. Production's own boxes conflicting on a beta domain (one names it outright):
  // beta gets no tables of its own rather than production losing its.
  next->beta_domains = beta_domains_;
  if (std::string why; !next->build(&why)) {
    warnings.push_back("beta domains not served from beta versions: production's boxes conflict there: " + why);
    next->beta_domains.clear();
    next->build(&err);
  }
  for (const auto& entry : fs::directory_iterator(wal_.root() / "boxes", ec)) {
    std::string box = entry.path().filename().string();
    auto version = base::read_file(entry.path() / "beta");
    if (!base::is_valid_name(box) || !version) continue;
    auto loaded = load_version(box, *version, &err);
    if (!loaded) {
      warnings.push_back("box " + box + ": beta version " + *version + " not loaded: " + err);
      continue;
    }
    auto trial = std::make_shared<ActiveSet>(*next);
    trial->beta[box] = std::make_shared<const ActiveBox>(std::move(*loaded));
    std::string why;
    if (trial->build(&why)) {
      next = trial;
    } else {
      warnings.push_back("box " + box + ": beta version " + *version + " not loaded: " + why);
    }
  }
  next->build(&err);
  // Splits (052): each box's candidate, loaded again; one that can't be is left out, said.
  for (const auto& [box, live] : next->boxes) {
    auto text = base::read_file(wal_.root() / "boxes" / box / "split");
    if (!text) continue;
    auto v = json::parse(*text, &err);
    if (!v || !v->is_object()) continue;
    const auto& o = v->as_object();
    std::string version = o.contains("version") && o.at("version").is_string() ? std::string(o.at("version").as_string()) : "";
    auto loaded = version.empty() ? std::nullopt : load_version(box, version, &err);
    if (!loaded || !split_refusals(live->manifest, loaded->manifest).empty()) {
      warnings.push_back("box " + box + ": its split to " + version + " not resumed" + (loaded ? "" : ": " + err));
      continue;
    }
    if (auto sp = split_of(std::make_shared<const ActiveBox>(std::move(*loaded)), num(o, "share_bp"),
                           o.contains("salt") && o.at("salt").is_string() ? std::string(o.at("salt").as_string()) : version)) {
      next->splits[box] = std::move(*sp);
    }
  }
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return warnings;
}

bool BoxRegistry::split_set(const std::string& box, const std::string& version, int64_t share_bp,
                            const boost::json::object& record, bool existing, std::string* error) {
  std::lock_guard lock(mu_);
  if (!base::is_valid_name(box) || !valid_version(version)) return *error = "bad box or version", false;
  auto current = active();
  auto live = current->boxes.find(box);
  if (live == current->boxes.end()) return *error = "box " + box + " isn't live: only a live box splits", false;
  if (live->second->version == version) return *error = "version " + version + " is the live one", false;
  // Starting and changing are told apart under the lock, so a change never brings back a split
  // that ended meanwhile, nor does a second start replace the first.
  auto now_split = current->splits.find(box);
  if (!existing && now_split != current->splits.end()) return *error = box + " is split already", false;
  if (existing && (now_split == current->splits.end() || now_split->second.candidate->version != version)) {
    return *error = box + "'s split with " + version + " ended meanwhile", false;
  }
  std::shared_ptr<const ActiveBox> candidate;
  std::string salt;
  if (auto it = current->splits.find(box); it != current->splits.end() && it->second.candidate->version == version) {
    candidate = it->second.candidate;
    salt = it->second.salt;
  } else {
    if (archived(box, version, error)) return false;
    auto loaded = load_version(box, version, error);
    if (!loaded) return false;
    if (check_ && !check_(loaded->manifest, error)) return false;
    auto why = split_refusals(live->second->manifest, loaded->manifest);
    if (!why.empty()) {
      std::string all;
      for (const auto& w : why) all += (all.empty() ? "" : "; ") + w;
      return *error = "these versions can't split traffic: " + all, false;
    }
    candidate = std::make_shared<const ActiveBox>(std::move(*loaded));
    salt = base::sha256_hex(box + "|" + version + "|" + std::to_string(base::now_unix_ms())).substr(0, 16);
  }
  auto sp = split_of(candidate, share_bp, salt);
  auto next = std::make_shared<ActiveSet>(*current);
  next->splits[box] = *sp;
  boost::json::object rec = record;
  rec["version"] = version;
  rec["share_bp"] = sp->share_bp;
  rec["salt"] = salt;
  if (!wal_.commit({{wal::FileChange::Kind::kWrite, "boxes/" + box + "/split", json::dump(rec)}}, error)) return false;
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return true;
}

bool BoxRegistry::beta_set(const std::string& box, const std::string& version, std::string* error) {
  std::lock_guard lock(mu_);
  if (!base::is_valid_name(box) || !valid_version(version)) return *error = "bad box or version", false;
  if (archived(box, version, error)) return false;
  auto loaded = load_version(box, version, error);
  if (!loaded) return false;
  if (check_ && !check_(loaded->manifest, error)) return false;
  auto current = active();
  auto next = std::make_shared<ActiveSet>(*current);
  next->beta[box] = std::make_shared<const ActiveBox>(std::move(*loaded));
  if (!next->build(error)) return false;
  wal::Bundle bundle = {{wal::FileChange::Kind::kWrite, "boxes/" + box + "/beta", version}};
  std::error_code ec;
  if (fs::exists(wal_.root() / "boxes" / box / "dropped", ec)) bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/dropped", ""});
  if (!wal_.commit(bundle, error)) return false;
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return true;
}

bool BoxRegistry::beta_clear(const std::string& box, std::string* error) {
  std::lock_guard lock(mu_);
  if (!base::is_valid_name(box)) return *error = "bad box name", false;
  std::error_code ec;
  if (!fs::exists(wal_.root() / "boxes" / box / "beta", ec)) return *error = "box " + box + " has no beta version", false;
  auto current = active();
  auto next = std::make_shared<ActiveSet>(*current);
  next->beta.erase(box);
  if (!next->build(error)) return false;
  if (!wal_.commit({{wal::FileChange::Kind::kDelete, "boxes/" + box + "/beta", ""}}, error)) return false;
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return true;
}

std::optional<std::string> BoxRegistry::beta_version(const std::string& box) const {
  if (!base::is_valid_name(box)) return std::nullopt;
  return base::read_file(wal_.root() / "boxes" / box / "beta");
}

void BoxRegistry::set_beta_domains(std::set<std::string> domains) {
  std::lock_guard lock(mu_);
  beta_domains_ = std::move(domains);
  auto next = std::make_shared<ActiveSet>(*active());
  next->beta_domains = beta_domains_;
  std::string err;
  if (!next->build(&err)) {
    // A beta version that conflicts on a newly named beta domain answers with live there, left
    // out as load_all leaves it out after a restart (its beta file stays; beta_status shows it
    // as "not served").
    if (auto left = fit_beta(*next)) {
      for (const auto& b : *left) {
        std::cerr << "goober-host: box " << b << "'s beta version conflicts on a beta domain and isn't served\n";
      }
    } else {
      // Production's own boxes conflict on a beta domain: beta gets no tables of its own rather
      // than production losing its.
      std::cerr << "goober-host: beta domains not served from beta versions: production's boxes conflict there: "
                << err << "\n";
      next->beta = active()->beta;
      next->beta_domains.clear();
      next->build(&err);
    }
  }
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
}

bool BoxRegistry::split_end(const std::string& box, std::string* error) {
  std::lock_guard lock(mu_);
  if (!base::is_valid_name(box)) return *error = "bad box name", false;
  auto current = active();
  std::error_code ec;
  if (!current->splits.count(box) && !fs::exists(wal_.root() / "boxes" / box / "split", ec)) {
    return *error = "box " + box + " isn't split", false;
  }
  auto next = std::make_shared<ActiveSet>(*current);
  next->splits.erase(box);
  if (!wal_.commit({{wal::FileChange::Kind::kDelete, "boxes/" + box + "/split", ""}}, error)) return false;
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return true;
}

std::optional<boost::json::object> BoxRegistry::split_record(const std::string& box) const {
  if (!base::is_valid_name(box)) return std::nullopt;
  auto text = base::read_file(wal_.root() / "boxes" / box / "split");
  if (!text) return std::nullopt;
  std::string err;
  auto v = json::parse(*text, &err);
  return v && v->is_object() ? std::optional<boost::json::object>(v->as_object()) : std::nullopt;
}

std::vector<std::string> BoxRegistry::unloaded() const {
  std::vector<std::string> out;
  auto current = active();
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(wal_.root() / "boxes", ec)) {
    std::string box = entry.path().filename().string();
    if (!base::is_valid_name(box) || current->boxes.count(box)) continue;
    if (fs::exists(entry.path() / "active", ec)) out.push_back(box);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool BoxRegistry::activate(std::shared_ptr<const ActiveBox> candidate, const wal::Bundle& bundle, std::string* error,
                           bool dry_run, std::vector<std::string>* notes) {
  auto current = active();
  auto next = std::make_shared<ActiveSet>(*current);  // its splits, beta versions, and beta domains too
  const std::string& name = candidate->manifest.box;
  // Names are unique across the host: no route name may repeat in another box.
  std::set<std::string> mine;
  for (const auto& r : candidate->manifest.routes) mine.insert(r.name);
  for (const auto& t : candidate->manifest.tools) mine.insert(t.name);
  for (const auto& t : candidate->manifest.timers) mine.insert(t.name);
  for (const auto& r : candidate->manifest.internal) mine.insert(r.name);
  for (const auto& r : candidate->manifest.identity) mine.insert(r.name);
  for (const auto& r : candidate->manifest.mail) mine.insert(r.name);
  for (const auto& r : candidate->manifest.jobs) mine.insert(r.name);
  for (const auto& r : candidate->manifest.upgrades) mine.insert(r.name);
  for (const auto& [other, box] : current->boxes) {
    if (other == name) continue;
    for (const auto* list : {&box->manifest.routes, &box->manifest.tools, &box->manifest.timers, &box->manifest.internal,
                              &box->manifest.identity, &box->manifest.mail, &box->manifest.jobs, &box->manifest.upgrades}) {
      for (const auto& r : *list) {
        if (mine.count(r.name)) {
          *error = "route name \"" + r.name + "\" is already used by box " + other;
          return false;
        }
      }
    }
  }
  // Two boxes may not claim the same mail addresses on a domain they share (038): the
  // address is the path, and one message goes to one route.
  auto mail_domains = [](const manifest::Manifest& m, const manifest::HttpRoute& r) {
    std::string err;
    auto p = mail::inbound::parse_address_pattern(r.mail_address, &err);
    if (p && !p->domain.empty()) return std::vector<std::string>{p->domain};
    return m.domains;
  };
  auto overlap = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (const auto& x : a) {
      for (const auto& y : b) {
        if (x == "*" || y == "*" || x == y) return true;
      }
    }
    return false;
  };
  for (const auto& mine_route : candidate->manifest.mail) {
    std::string err;
    auto mp = mail::inbound::parse_address_pattern(mine_route.mail_address, &err);
    for (const auto& [other, box] : current->boxes) {
      if (other == name) continue;
      for (const auto& theirs : box->manifest.mail) {
        auto tp = mail::inbound::parse_address_pattern(theirs.mail_address, &err);
        if (!mp || !tp) continue;
        // Shapes compare with the domain left off: support@ and support@example.com collide too.
        auto bare = [](const mail::inbound::AddressPattern& p) { return p.shape().substr(0, p.shape().rfind('@')); };
        if (bare(*mp) == bare(*tp) && overlap(mail_domains(candidate->manifest, mine_route), mail_domains(box->manifest, theirs))) {
          *error = "mail address " + mine_route.mail_address + " is already claimed by box " + other + " (" +
                   theirs.mail_address + ")";
          return false;
        }
      }
    }
  }
  next->boxes[name] = candidate;
  // Its beta version going live (a promotion, 053 § 11): beta follows live again.
  bool beta_promoted = next->beta.count(name) && next->beta.at(name)->version == candidate->version;
  if (beta_promoted) next->beta.erase(name);
  std::vector<std::string> beta_left;
  if (!next->build(error)) {
    // A beta version in the way answers with live on beta again, and production goes on.
    auto left = fit_beta(*next);
    if (!left) return false;
    beta_left = std::move(*left);
    error->clear();
  }
  if (notes) {
    for (const auto& b : beta_left) {
      notes->push_back("going live clears box " + b + "'s beta version, which claims the same route on a beta domain "
                       "(production wins, 053 § 6)");
    }
  }
  if (dry_run) return true;
  // A new live version ends the box's split (052 § 4).
  next->splits.erase(name);
  wal::Bundle all = bundle;
  end_split(all, wal_.root(), name, "version " + candidate->version + " went live");
  if (beta_promoted) all.push_back({wal::FileChange::Kind::kDelete, "boxes/" + name + "/beta", ""});
  // Left out of beta for good, so memory and disk agree: the box's beta slot is cleared.
  for (const auto& b : beta_left) {
    all.push_back({wal::FileChange::Kind::kDelete, "boxes/" + b + "/beta", ""});
    std::cerr << "goober-host: box " << name << " going live cleared box " << b
              << "'s beta version, which conflicts with it on a beta domain (053 § 6)\n";
  }
  if (!wal_.commit(all, error)) return false;
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return true;
}

std::optional<UploadResult> BoxRegistry::upload(std::string_view manifest_text, std::string_view module,
                                                std::string_view types_text, std::string* error) {
  return upload_or_check(manifest_text, module, types_text, Mode::kActivate, error);
}

bool BoxRegistry::check(std::string_view manifest_text, std::string_view module, std::string_view types_text,
                        std::string* error) {
  return upload_or_check(manifest_text, module, types_text, Mode::kCheck, error).has_value();
}

std::optional<UploadResult> BoxRegistry::store(std::string_view manifest_text, std::string_view module,
                                               std::string_view types_text, std::string* error, bool for_beta) {
  return upload_or_check(manifest_text, module, types_text, Mode::kStore, error, for_beta);
}

std::optional<UploadResult> BoxRegistry::upload_or_check(std::string_view manifest_text, std::string_view module,
                                                         std::string_view types_text, Mode mode, std::string* error,
                                                         bool for_beta) {
  std::lock_guard lock(mu_);
  auto m = manifest::parse_manifest(manifest_text, error);
  if (!m) return std::nullopt;
  auto t = load_types(*m, types_text, error);
  if (!t) return std::nullopt;
  if (check_ && !check_(*m, error)) return std::nullopt;
  if (advice_) {
    for (auto& w : advice_(*m)) m->warnings.push_back(std::move(w));
  }
  std::string wasm;
  if (!runtime::wasm::Engine::is_binary(module)) {
    if (!runtime::wasm::Engine::wat_to_wasm(module, &wasm, error)) return std::nullopt;
  } else {
    wasm.assign(module);
  }
  auto compiled = engine_.compile(wasm, exports_of(*m), error);
  if (!compiled) return std::nullopt;

  // The version is named by everything in it; a types document joins the hash only when
  // present, so versions without one keep their names.
  std::string hashed = "goober-version-1\n" + std::string(manifest_text) + "\n" + wasm;
  if (!types_text.empty()) hashed += "\ntypes\n" + std::string(types_text);
  std::string version = base::sha256_hex(hashed).substr(0, 24);
  std::string dir = "boxes/" + m->box + "/versions/" + version;
  std::string box = m->box;
  wal::Bundle bundle = {
      {wal::FileChange::Kind::kWrite, dir + "/manifest.json", std::string(manifest_text)},
      {wal::FileChange::Kind::kWrite, dir + "/box.wasm", wasm},
  };
  if (mode == Mode::kActivate) {
    bundle.push_back({wal::FileChange::Kind::kWrite, "boxes/" + box + "/active", version});
    std::error_code ec;
    if (fs::exists(wal_.root() / "boxes" / box / "disabled", ec)) {
      bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/disabled", ""});
    }
  }
  if (!types_text.empty()) bundle.push_back({wal::FileChange::Kind::kWrite, dir + "/types.json", std::string(types_text)});
  if (mode != Mode::kCheck) {
    // A dropped box uploaded again is a box again.
    std::error_code dec;
    if (fs::exists(wal_.root() / "boxes" / box / "dropped", dec)) {
      bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/dropped", ""});
    }
    // Its life (045 § 3.1): stored now, unless it was before; uploading an archived version
    // again brings it back, its inactive time started again.
    int64_t now = base::now_unix_ms();
    auto life = this->life(box, version);
    if (!life.contains("stored_ms")) {
      life["stored_ms"] = now;
      std::function<std::string()> actor;
      {
        std::lock_guard plock(policy_mu_);
        actor = actor_;
      }
      if (actor) {
        if (auto by = actor(); !by.empty()) life["stored_by"] = by;
      }
      // Stored for beta (053 § 6): what beta's operator may drop without the owner.
      if (for_beta) life["channel"] = "beta";
    } else if (!for_beta) {
      // The same bytes stored for production too are production's as well.
      life.erase("channel");
    }
    bool live_now = base::read_file(wal_.root() / "boxes" / box / "active").value_or("") == version;
    if (!live_now && (life.contains("archived_ms") || !life.contains("inactive_since_ms"))) {
      life.erase("archived_ms");
      life["inactive_since_ms"] = now;
    }
    if (mode == Mode::kActivate) live_moves(bundle, box, version, &life, now);
    bundle.push_back({wal::FileChange::Kind::kWrite, life_path(box, version), json::dump(life)});
  }
  UploadResult result{version, m->warnings};
  auto candidate = std::make_shared<const ActiveBox>(ActiveBox{version, std::move(*m), compiled, *t});
  // Every mode checks the candidate against the active boxes as if it went live now. A version
  // stored dark may conflict with a live box, because it is to take over that box's route
  // names or mail addresses once that box is disabled: it is stored anyway, the conflict is a
  // warning, and version_promote checks again when it goes live.
  if (mode == Mode::kStore) {
    std::string conflict;
    if (!activate(candidate, bundle, &conflict, true, &result.warnings)) {
      result.warnings.push_back("not live-able while the active boxes are as they are: " + conflict +
                                ". Stored dark, but version_promote will refuse it until the other box gives way: "
                                "box_disable it (or move its route) first, then promote this one.");
    }
    if (!wal_.commit(bundle, error)) return std::nullopt;
    return result;
  }
  if (!activate(candidate, bundle, error, mode != Mode::kActivate, &result.warnings)) return std::nullopt;
  return result;
}

std::optional<std::string> BoxRegistry::disable(const std::string& box, const std::string& why, std::string* error) {
  std::lock_guard lock(mu_);
  auto current = active();
  auto it = current->boxes.find(box);
  if (it == current->boxes.end()) return *error = "no active box named " + box, std::nullopt;
  std::string version = it->second->version;
  auto next = std::make_shared<ActiveSet>(*current);
  next->boxes.erase(box);
  next->splits.erase(box);
  if (!next->build(error)) return std::nullopt;
  std::string record = json::dump(boost::json::object{{"version", version}, {"why", why.substr(0, 500)}});
  wal::Bundle bundle = {{wal::FileChange::Kind::kDelete, "boxes/" + box + "/active", ""},
                        {wal::FileChange::Kind::kWrite, "boxes/" + box + "/disabled", record}};
  end_split(bundle, wal_.root(), box, "the box was disabled");
  live_moves(bundle, box, "", nullptr, base::now_unix_ms());
  if (!wal_.commit(bundle, error)) return std::nullopt;
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return version;
}

std::optional<BoxRegistry::Disabled> BoxRegistry::disabled(const std::string& box) const {
  if (!base::is_valid_name(box)) return std::nullopt;
  auto text = base::read_file(wal_.root() / "boxes" / box / "disabled");
  if (!text) return std::nullopt;
  std::string err;
  auto v = json::parse(*text, &err);
  if (!v || !v->is_object()) return Disabled{};
  const auto& o = v->as_object();
  auto str = [&](const char* k) {
    const auto* x = o.if_contains(k);
    return x && x->is_string() ? std::string(x->as_string()) : std::string();
  };
  return Disabled{str("version"), str("why")};
}

bool BoxRegistry::promote(const std::string& box, const std::string& version, std::string* error, bool from_split) {
  std::lock_guard lock(mu_);
  if (!base::is_valid_name(box) || !base::is_safe_relative_path(version) || version.find('/') != std::string::npos) {
    *error = "bad box or version";
    return false;
  }
  // A split's promotion only while that split still runs: a disable or a new live version that
  // ended it meanwhile stands.
  if (from_split) {
    auto current = active();
    auto it = current->splits.find(box);
    if (it == current->splits.end() || it->second.candidate->version != version) {
      return *error = box + "'s split with " + version + " ended meanwhile", false;
    }
  }
  if (archived(box, version, error)) return false;
  auto loaded = load_version(box, version, error);
  if (!loaded) return false;
  if (check_ && !check_(loaded->manifest, error)) return false;
  wal::Bundle bundle = {{wal::FileChange::Kind::kWrite, "boxes/" + box + "/active", version}};
  std::error_code ec;
  if (fs::exists(wal_.root() / "boxes" / box / "disabled", ec)) {
    bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/disabled", ""});
  }
  auto life = this->life(box, version);
  int64_t now = base::now_unix_ms();
  if (!life.contains("stored_ms")) life["stored_ms"] = now;
  // Promoting the version already live changes no life.
  if (live_moves(bundle, box, version, &life, now)) {
    bundle.push_back({wal::FileChange::Kind::kWrite, life_path(box, version), json::dump(life)});
  }
  return activate(std::make_shared<const ActiveBox>(std::move(*loaded)), bundle, error);
}

std::vector<std::string> BoxRegistry::box_names() const {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(wal_.root() / "boxes", ec)) {
    std::string box = entry.path().filename().string();
    if (base::is_valid_name(box)) out.push_back(box);
  }
  std::sort(out.begin(), out.end());
  return out;
}

namespace {


int64_t mtime_ms(const fs::path& p) {
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  if (ec) return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch())
      .count();
}


// The stored versions of a box: directories holding a manifest (a deleted one's is empty).
std::vector<std::string> stored_versions(const fs::path& root, const std::string& box) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(root / "boxes" / box / "versions", ec)) {
    std::error_code e2;
    if (fs::exists(entry.path() / "manifest.json", e2)) out.push_back(entry.path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

std::optional<std::vector<std::string>> BoxRegistry::drop(const std::string& box, const boost::json::object& record,
                                                          std::string* error, bool only_unserved) {
  std::lock_guard lock(mu_);
  if (!base::is_valid_name(box)) return *error = "bad box name", std::nullopt;
  fs::path root = wal_.root() / "boxes" / box;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return *error = "no box named " + box, std::nullopt;
  // Checked again under the lock: a box that went live or was disabled since isn't beta's alone.
  if (only_unserved && (active()->boxes.count(box) || fs::exists(root / "disabled", ec))) {
    return *error = "box " + box + " is production's now (live or disabled): its drop waits for an owner", std::nullopt;
  }
  if (fs::exists(root / "dropped", ec)) return *error = "box " + box + " was dropped already", std::nullopt;
  auto current = active();
  std::shared_ptr<ActiveSet> next;
  wal::Bundle bundle;
  int64_t now = base::now_unix_ms();
  if (current->boxes.count(box) || current->beta.count(box)) {
    next = std::make_shared<ActiveSet>(*current);
    next->boxes.erase(box);
    next->splits.erase(box);
    next->beta.erase(box);
    if (!next->build(error)) return std::nullopt;
  }
  if (fs::exists(root / "beta", ec)) bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/beta", ""});
  if (fs::exists(root / "split", ec)) bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/split", ""});
  if (fs::exists(root / "active", ec)) bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/active", ""});
  if (fs::exists(root / "disabled", ec)) bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/disabled", ""});
  std::vector<std::string> gone;
  for (const auto& v : stored_versions(wal_.root(), box)) {
    auto l = life(box, v);
    fs::path dir = root / "versions" / v;
    auto wasm = base::read_file(dir / "box.wasm").value_or("");
    boost::json::object t{{"box", box},
                          {"version", v},
                          {"module_sha256", base::sha256_hex(wasm)},
                          {"module_bytes", int64_t(wasm.size())},
                          {"stored_ms", num(l, "stored_ms")},
                          {"stored_by", l.contains("stored_by") ? l.at("stored_by") : boost::json::value(nullptr)},
                          {"live", l.contains("live") ? l.at("live") : boost::json::array{}},
                          {"inactive_since_ms", num(l, "inactive_since_ms")},
                          {"archived_ms", num(l, "archived_ms")},
                          {"deleted_ms", now},
                          {"dropped", true}};
    bundle.push_back({wal::FileChange::Kind::kWrite, "boxes/" + box + "/deleted/" + v + ".json", json::dump(t)});
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (entry.is_regular_file()) {
        bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/versions/" + v + "/" + entry.path().filename().string(), ""});
      }
    }
    gone.push_back(v);
  }
  boost::json::object rec = record;
  rec["box"] = box;
  rec["dropped_ms"] = now;
  boost::json::array versions;
  for (const auto& v : gone) versions.push_back(boost::json::value(v));
  rec["versions"] = std::move(versions);
  bundle.push_back({wal::FileChange::Kind::kWrite, "boxes/" + box + "/dropped", json::dump(rec)});
  if (!wal_.commit(bundle, error)) return std::nullopt;
  for (const auto& v : gone) fs::remove(root / "versions" / v, ec);  // empty now
  if (next) {
    std::lock_guard snap(snapshot_mu_);
    active_ = next;
  }
  return gone;
}

std::optional<boost::json::object> BoxRegistry::dropped(const std::string& box) const {
  if (!base::is_valid_name(box)) return std::nullopt;
  auto text = base::read_file(wal_.root() / "boxes" / box / "dropped");
  if (!text) return std::nullopt;
  std::string err;
  auto v = json::parse(*text, &err);
  return v && v->is_object() ? std::optional<boost::json::object>(v->as_object()) : boost::json::object{};
}

std::vector<VersionInfo> BoxRegistry::versions(const std::string& box) const {
  std::vector<VersionInfo> out;
  if (!base::is_valid_name(box)) return out;
  auto active = base::read_file(wal_.root() / "boxes" / box / "active").value_or("");
  auto r = retention();
  std::map<std::string, boost::json::object> lives;
  for (const auto& v : stored_versions(wal_.root(), box)) lives[v] = life(box, v);
  auto keep = kept(box, lives, protected_versions());
  for (const auto& [v, l] : lives) {
    VersionInfo info;
    info.version = v;
    info.active = v == active;
    info.stored_ms = num(l, "stored_ms");
    if (!info.stored_ms) info.stored_ms = mtime_ms(wal_.root() / "boxes" / box / "versions" / v / "manifest.json");
    if (const auto* p = l.if_contains("live"); p && p->is_array()) info.live = p->as_array();
    if (const auto* b = l.if_contains("stored_by"); b && b->is_string()) info.stored_by = std::string(b->as_string());
    info.for_beta = l.contains("channel") && l.at("channel") == "beta";
    info.archived_ms = num(l, "archived_ms");
    info.state = info.active ? "live" : info.archived_ms ? "archived" : "stored";
    if (!info.active) {
      // No record yet: its inactive time starts at the host's next pruning.
      info.inactive_since_ms = num(l, "inactive_since_ms");
      if (auto k = keep.find(v); k != keep.end()) {
        info.kept = k->second;
      } else if (info.inactive_since_ms) {
        if (!info.archived_ms) info.archive_at_ms = info.inactive_since_ms + r.archive_after_ms;
        info.delete_at_ms = info.inactive_since_ms + r.delete_after_ms;
      }
    }
    out.push_back(std::move(info));
  }
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(wal_.root() / "boxes" / box / "deleted", ec)) {
    if (entry.path().extension() != ".json") continue;
    std::string v = entry.path().stem().string();
    if (lives.count(v)) continue;  // uploaded again since
    auto t = tombstone(box, v);
    if (!t) continue;
    VersionInfo info;
    info.version = v;
    info.state = "deleted";
    info.stored_ms = num(*t, "stored_ms");
    info.inactive_since_ms = num(*t, "inactive_since_ms");
    info.archived_ms = num(*t, "archived_ms");
    info.deleted_ms = num(*t, "deleted_ms");
    if (const auto* p = t->if_contains("live"); p && p->is_array()) info.live = p->as_array();
    if (const auto* s = t->if_contains("module_sha256"); s && s->is_string()) info.module_sha256 = std::string(s->as_string());
    if (const auto* b = t->if_contains("stored_by"); b && b->is_string()) info.stored_by = std::string(b->as_string());
    out.push_back(std::move(info));
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.version < b.version; });
  return out;
}

std::string BoxRegistry::life_path(const std::string& box, const std::string& version) const {
  return "boxes/" + box + "/versions/" + version + "/life.json";
}

boost::json::object BoxRegistry::life(const std::string& box, const std::string& version) const {
  if (!base::is_valid_name(box) || !valid_version(version)) return {};
  auto text = base::read_file(wal_.root() / life_path(box, version));
  if (!text) return {};
  std::string err;
  auto v = json::parse(*text, &err);
  return v && v->is_object() ? v->as_object() : boost::json::object{};
}

bool BoxRegistry::live_moves(wal::Bundle& bundle, const std::string& box, const std::string& version,
                             boost::json::object* next, int64_t now_ms) const {
  auto old = base::read_file(wal_.root() / "boxes" / box / "active").value_or("");
  if (!version.empty() && old == version) return false;
  if (!old.empty() && valid_version(old)) {
    auto l = life(box, old);
    if (!l.contains("stored_ms")) l["stored_ms"] = mtime_ms(wal_.root() / "boxes" / box / "versions" / old / "manifest.json");
    auto& live = l["live"];
    if (!live.is_array()) live = boost::json::array{};
    auto& periods = live.as_array();
    if (!periods.empty() && periods.back().is_object() && !periods.back().as_object().contains("until_ms")) {
      periods.back().as_object()["until_ms"] = now_ms;
    } else {
      periods.push_back(boost::json::object{{"until_ms", now_ms}});  // live since before records were kept
    }
    l["inactive_since_ms"] = now_ms;
    l.erase("first_seen");
    bundle.push_back({wal::FileChange::Kind::kWrite, life_path(box, old), json::dump(l)});
  }
  if (next) {
    auto& live = (*next)["live"];
    if (!live.is_array()) live = boost::json::array{};
    live.as_array().push_back(boost::json::object{{"from_ms", now_ms}});
    next->erase("inactive_since_ms");
    next->erase("archived_ms");
    next->erase("first_seen");
  }
  return true;
}

void BoxRegistry::set_retention(const Retention& r) {
  std::lock_guard lock(policy_mu_);
  retention_ = r;
}

void BoxRegistry::set_protector(Protector p) {
  std::lock_guard lock(policy_mu_);
  protector_ = std::move(p);
}

void BoxRegistry::set_actor(std::function<std::string()> actor) {
  std::lock_guard lock(policy_mu_);
  actor_ = std::move(actor);
}

Retention BoxRegistry::retention() const {
  std::lock_guard lock(policy_mu_);
  return retention_;
}

BoxRegistry::Protected BoxRegistry::protected_versions() const {
  Protector protector;
  {
    std::lock_guard lock(policy_mu_);
    protector = protector_;
  }
  return protector ? protector() : Protected{};
}

std::map<std::string, std::string> BoxRegistry::kept(const std::string& box,
                                                     const std::map<std::string, boost::json::object>& lives,
                                                     const Protected& protected_) const {
  std::map<std::string, std::string> out;
  if (auto it = protected_.find(box); it != protected_.end()) out = it->second;
  if (auto d = disabled(box); d && !d->version.empty()) {
    out[d->version] = "the version of the disabled box: box_enable serves it again";
  }
  if (auto b = beta_version(box)) out[*b] = "the beta version: it answers on beta domains";
  if (auto sp = split_record(box); sp && sp->contains("version") && sp->at("version").is_string()) {
    out[std::string(sp->at("version").as_string())] = "the candidate of a split: it serves a share of the traffic";
  }
  // The newest versions that were live before, by when they stopped: rollback targets.
  auto active = base::read_file(wal_.root() / "boxes" / box / "active").value_or("");
  // A version stored before records were kept may have been live: it counts too, the newest
  // stored first among those.
  std::vector<std::tuple<int64_t, int64_t, std::string>> were_live;
  for (const auto& [v, l] : lives) {
    const auto* p = l.if_contains("live");
    bool was = p && p->is_array() && !p->as_array().empty();
    if (v == active || !(was || l.contains("first_seen"))) continue;
    // By when it was last live (a restore restarts its inactive time, not this).
    int64_t until = num(l, "inactive_since_ms");
    if (was && p->as_array().back().is_object()) {
      if (int64_t u = num(p->as_array().back().as_object(), "until_ms")) until = u;
    }
    were_live.emplace_back(until, num(l, "stored_ms"), v);
  }
  std::sort(were_live.rbegin(), were_live.rend());
  int64_t keep = retention().keep;
  for (size_t i = 0; i < were_live.size() && int64_t(i) < keep; ++i) {
    out.emplace(std::get<2>(were_live[i]),
                "one of the newest " + std::to_string(keep) + " versions that were live: a rollback target");
  }
  return out;
}

bool BoxRegistry::archived(const std::string& box, const std::string& version, std::string* error) const {
  auto l = life(box, version);
  int64_t at = num(l, "archived_ms");
  if (!at) return false;
  *error = "version " + version + " of " + box + " is archived (inactive since " +
           base::format_utc_ms(num(l, "inactive_since_ms")) +
           "): it can't be tried or promoted until a person restores it with version_restore {box, version, why}";
  return true;
}

PruneReport BoxRegistry::prune(int64_t now_ms, const Annotate& annotate) {
  std::lock_guard lock(mu_);
  PruneReport report;
  auto r = retention();
  auto protected_ = protected_versions();  // once for every box
  for (const auto& box : box_names()) {
    auto active = base::read_file(wal_.root() / "boxes" / box / "active").value_or("");
    std::map<std::string, boost::json::object> lives;
    for (const auto& v : stored_versions(wal_.root(), box)) {
      auto l = life(box, v);
      // Stored before records were kept: its inactive time starts now, whatever its age.
      if (v != active && !l.contains("inactive_since_ms")) {
        if (!l.contains("stored_ms")) l["stored_ms"] = mtime_ms(wal_.root() / "boxes" / box / "versions" / v / "manifest.json");
        l["inactive_since_ms"] = now_ms;
        l["first_seen"] = true;
        std::string err;
        if (!wal_.commit({{wal::FileChange::Kind::kWrite, life_path(box, v), json::dump(l)}}, &err)) {
          throw std::runtime_error("recording " + box + " " + v + ": " + err);
        }
      }
      lives[v] = std::move(l);
    }
    auto keep = kept(box, lives, protected_);
    for (auto& [v, l] : lives) {
      if (v == active || keep.count(v)) continue;
      int64_t inactive = num(l, "inactive_since_ms");
      if (!inactive) continue;
      std::string err;
      if (now_ms - inactive >= r.delete_after_ms) {
        fs::path dir = wal_.root() / "boxes" / box / "versions" / v;
        auto wasm = base::read_file(dir / "box.wasm").value_or("");
        boost::json::object t{{"box", box},
                              {"version", v},
                              {"module_sha256", base::sha256_hex(wasm)},
                              {"module_bytes", int64_t(wasm.size())},
                              {"stored_ms", num(l, "stored_ms")},
                              {"stored_by", l.contains("stored_by") ? l.at("stored_by") : boost::json::value(nullptr)},
                              {"live", l.contains("live") ? l.at("live") : boost::json::array{}},
                              {"inactive_since_ms", inactive},
                              {"archived_ms", num(l, "archived_ms")},
                              {"deleted_ms", now_ms}};
        if (annotate) {
          for (auto& kv : annotate(box, v)) {
            if (!t.contains(kv.key())) t[kv.key()] = std::move(kv.value());
          }
        }
        std::string rel = "boxes/" + box + "/versions/" + v + "/";
        wal::Bundle b = {{wal::FileChange::Kind::kWrite, "boxes/" + box + "/deleted/" + v + ".json", json::dump(t)}};
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
          if (entry.is_regular_file()) b.push_back({wal::FileChange::Kind::kDelete, rel + entry.path().filename().string(), ""});
        }
        if (!wal_.commit(b, &err)) throw std::runtime_error("deleting " + box + " " + v + ": " + err);
        fs::remove(dir, ec);  // empty now
        report.deleted.push_back(std::move(t));
      } else if (now_ms - inactive >= r.archive_after_ms && !l.contains("archived_ms")) {
        l["archived_ms"] = now_ms;
        if (!wal_.commit({{wal::FileChange::Kind::kWrite, life_path(box, v), json::dump(l)}}, &err)) {
          throw std::runtime_error("archiving " + box + " " + v + ": " + err);
        }
        report.archived.emplace_back(box, v);
      }
    }
  }
  return report;
}

bool BoxRegistry::restore(const std::string& box, const std::string& version, std::string* error) {
  std::lock_guard lock(mu_);
  if (!base::is_valid_name(box) || !valid_version(version)) return *error = "bad box or version", false;
  if (!manifest_text(box, version)) {
    if (tombstone(box, version)) {
      return *error = "version " + version + " of " + box + " was deleted; only its tombstone is left (version_get shows it)", false;
    }
    return *error = "no version " + version + " of " + box, false;
  }
  auto l = life(box, version);
  if (!l.contains("archived_ms")) return *error = "version " + version + " of " + box + " isn't archived", false;
  l.erase("archived_ms");
  l["inactive_since_ms"] = base::now_unix_ms();
  return wal_.commit({{wal::FileChange::Kind::kWrite, life_path(box, version), json::dump(l)}}, error);
}

std::optional<boost::json::object> BoxRegistry::tombstone(const std::string& box, const std::string& version) const {
  if (!base::is_valid_name(box) || !valid_version(version)) return std::nullopt;
  auto text = base::read_file(wal_.root() / "boxes" / box / "deleted" / (version + ".json"));
  if (!text) return std::nullopt;
  std::string err;
  auto v = json::parse(*text, &err);
  if (!v || !v->is_object()) return std::nullopt;
  return v->as_object();
}

std::optional<std::string> BoxRegistry::manifest_text(const std::string& box, const std::string& version) const {
  if (!base::is_valid_name(box) || version.empty() || version.find('/') != std::string::npos ||
      version.find("..") != std::string::npos) {
    return std::nullopt;
  }
  return base::read_file(wal_.root() / "boxes" / box / "versions" / version / "manifest.json");
}

std::optional<VersionFiles> BoxRegistry::files(const std::string& box, const std::string& version) const {
  auto manifest = manifest_text(box, version);
  if (!manifest) return std::nullopt;
  fs::path dir = wal_.root() / "boxes" / box / "versions" / version;
  auto wasm = base::read_file(dir / "box.wasm");
  if (!wasm) return std::nullopt;
  return VersionFiles{version, *manifest, *wasm, base::read_file(dir / "types.json").value_or("")};
}

std::optional<std::string> BoxRegistry::types_text(const std::string& box, const std::string& version) const {
  if (!manifest_text(box, version)) return std::nullopt;
  return base::read_file(wal_.root() / "boxes" / box / "versions" / version / "types.json");
}

}  // namespace goober::boxes
