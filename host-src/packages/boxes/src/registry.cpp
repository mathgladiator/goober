#include "goober/boxes/registry.h"

#include <algorithm>
#include <set>

#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"

namespace goober::boxes {
namespace fs = std::filesystem;

const router::RouteTable<RouteRef>& ActiveSet::table_for(const std::string& domain) const {
  static const router::RouteTable<RouteRef> kEmpty;
  auto it = tables_.find(domain);
  if (it != tables_.end()) return it->second;
  it = tables_.find("*");
  return it == tables_.end() ? kEmpty : it->second;
}

bool ActiveSet::build(std::string* error) {
  tables_.clear();
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
    }
  }
  return true;
}

BoxRegistry::BoxRegistry(wal::HostWal& wal, runtime::wasm::Engine& engine)
    : wal_(wal), engine_(engine), active_(std::make_shared<ActiveSet>()) {}

std::shared_ptr<const ActiveSet> BoxRegistry::active() const {
  std::lock_guard lock(snapshot_mu_);
  return active_;
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
  std::vector<std::string> exports;
  for (const auto& r : m->routes) exports.push_back(r.export_name());
  auto compiled = engine_.compile(*wasm, exports, error);
  if (!compiled) return std::nullopt;
  return ActiveBox{version, std::move(*m), compiled};
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
    warnings.push_back("route conflict on load, no boxes served: " + err);
    next = std::make_shared<ActiveSet>();
    next->build(&err);
  }
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return warnings;
}

bool BoxRegistry::activate(std::shared_ptr<const ActiveBox> candidate, const wal::Bundle& bundle,
                           std::string* error) {
  auto current = active();
  auto next = std::make_shared<ActiveSet>();
  next->boxes = current->boxes;
  const std::string& name = candidate->manifest.box;
  // Names are unique across the host: no route name may repeat in another box.
  std::set<std::string> mine;
  for (const auto& r : candidate->manifest.routes) mine.insert(r.name);
  for (const auto& [other, box] : current->boxes) {
    if (other == name) continue;
    for (const auto& r : box->manifest.routes) {
      if (mine.count(r.name)) {
        *error = "route name \"" + r.name + "\" is already used by box " + other;
        return false;
      }
    }
  }
  next->boxes[name] = candidate;
  if (!next->build(error)) return false;
  if (!wal_.commit(bundle, error)) return false;
  std::lock_guard snap(snapshot_mu_);
  active_ = next;
  return true;
}

std::optional<UploadResult> BoxRegistry::upload(std::string_view manifest_text, std::string_view module,
                                                std::string* error) {
  std::lock_guard lock(mu_);
  auto m = manifest::parse_manifest(manifest_text, error);
  if (!m) return std::nullopt;
  if (check_ && !check_(*m, error)) return std::nullopt;
  std::string wasm;
  if (!runtime::wasm::Engine::is_binary(module)) {
    if (!runtime::wasm::Engine::wat_to_wasm(module, &wasm, error)) return std::nullopt;
  } else {
    wasm.assign(module);
  }
  std::vector<std::string> exports;
  for (const auto& r : m->routes) exports.push_back(r.export_name());
  auto compiled = engine_.compile(wasm, exports, error);
  if (!compiled) return std::nullopt;

  std::string version = base::sha256_hex("goober-version-1\n" + std::string(manifest_text) + "\n" + wasm).substr(0, 24);
  std::string dir = "boxes/" + m->box + "/versions/" + version;
  wal::Bundle bundle = {
      {wal::FileChange::Kind::kWrite, dir + "/manifest.json", std::string(manifest_text)},
      {wal::FileChange::Kind::kWrite, dir + "/box.wasm", wasm},
      {wal::FileChange::Kind::kWrite, "boxes/" + m->box + "/active", version},
  };
  UploadResult result{version, m->warnings};
  auto candidate = std::make_shared<const ActiveBox>(ActiveBox{version, std::move(*m), compiled});
  if (!activate(candidate, bundle, error)) return std::nullopt;
  return result;
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
    out.push_back(VersionInfo{v, v == active});
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

}  // namespace goober::boxes
