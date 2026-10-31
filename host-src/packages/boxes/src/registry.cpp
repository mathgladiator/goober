#include "goober/boxes/registry.h"
#include "goober/mail_inbound/address.h"

#include <algorithm>
#include <chrono>
#include <set>

#include "goober/base/files.h"
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

bool ActiveSet::build(std::string* error) {
  tables_.clear();
  tools_.clear();
  std::set<std::string> keys = {"*"};
  for (const auto& [_, box] : boxes) {
    for (const auto& d : box->manifest.domains) keys.insert(d);
  }
  for (const auto& key : keys) {
    auto& table = tables_[key];
    for (const auto& [name, box] : boxes) {
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
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return warnings;
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
                           bool dry_run) {
  auto current = active();
  auto next = std::make_shared<ActiveSet>();
  next->boxes = current->boxes;
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
  for (const auto& [other, box] : current->boxes) {
    if (other == name) continue;
    for (const auto* list : {&box->manifest.routes, &box->manifest.tools, &box->manifest.timers, &box->manifest.internal,
                              &box->manifest.identity, &box->manifest.mail, &box->manifest.jobs}) {
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
  if (!next->build(error)) return false;
  if (dry_run) return true;
  if (!wal_.commit(bundle, error)) return false;
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
                                               std::string_view types_text, std::string* error) {
  return upload_or_check(manifest_text, module, types_text, Mode::kStore, error);
}

std::optional<UploadResult> BoxRegistry::upload_or_check(std::string_view manifest_text, std::string_view module,
                                                         std::string_view types_text, Mode mode, std::string* error) {
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
  UploadResult result{version, m->warnings};
  auto candidate = std::make_shared<const ActiveBox>(ActiveBox{version, std::move(*m), compiled, *t});
  // Every mode checks the candidate against the active boxes as if it went live now. A version
  // stored dark may conflict with a live box, because it is to take over that box's route
  // names or mail addresses once that box is disabled: it is stored anyway, the conflict is a
  // warning, and version_promote checks again when it goes live.
  if (mode == Mode::kStore) {
    std::string conflict;
    if (!activate(candidate, bundle, &conflict, true)) {
      result.warnings.push_back("not live-able while the active boxes are as they are: " + conflict +
                                "; version_promote checks again (box_disable the other box first)");
    }
    if (!wal_.commit(bundle, error)) return std::nullopt;
    return result;
  }
  if (!activate(candidate, bundle, error, mode != Mode::kActivate)) return std::nullopt;
  return result;
}

std::optional<std::string> BoxRegistry::disable(const std::string& box, const std::string& why, std::string* error) {
  std::lock_guard lock(mu_);
  auto current = active();
  auto it = current->boxes.find(box);
  if (it == current->boxes.end()) return *error = "no active box named " + box, std::nullopt;
  std::string version = it->second->version;
  auto next = std::make_shared<ActiveSet>();
  for (const auto& [name, b] : current->boxes) {
    if (name != box) next->boxes[name] = b;
  }
  if (!next->build(error)) return std::nullopt;
  std::string record = json::dump(boost::json::object{{"version", version}, {"why", why.substr(0, 500)}});
  wal::Bundle bundle = {{wal::FileChange::Kind::kDelete, "boxes/" + box + "/active", ""},
                        {wal::FileChange::Kind::kWrite, "boxes/" + box + "/disabled", record}};
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

bool BoxRegistry::promote(const std::string& box, const std::string& version, std::string* error) {
  std::lock_guard lock(mu_);
  if (!base::is_valid_name(box) || !base::is_safe_relative_path(version) || version.find('/') != std::string::npos) {
    *error = "bad box or version";
    return false;
  }
  auto loaded = load_version(box, version, error);
  if (!loaded) return false;
  if (check_ && !check_(loaded->manifest, error)) return false;
  wal::Bundle bundle = {{wal::FileChange::Kind::kWrite, "boxes/" + box + "/active", version}};
  std::error_code ec;
  if (fs::exists(wal_.root() / "boxes" / box / "disabled", ec)) {
    bundle.push_back({wal::FileChange::Kind::kDelete, "boxes/" + box + "/disabled", ""});
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

std::vector<VersionInfo> BoxRegistry::versions(const std::string& box) const {
  std::vector<VersionInfo> out;
  if (!base::is_valid_name(box)) return out;
  auto active = base::read_file(wal_.root() / "boxes" / box / "active").value_or("");
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(wal_.root() / "boxes" / box / "versions", ec)) {
    std::string v = entry.path().filename().string();
    int64_t stored = 0;
    std::error_code tec;
    auto t = fs::last_write_time(entry.path() / "manifest.json", tec);
    if (!tec) {
      stored = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch())
                   .count();
    }
    out.push_back(VersionInfo{v, v == active, stored});
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.version < b.version; });
  return out;
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
