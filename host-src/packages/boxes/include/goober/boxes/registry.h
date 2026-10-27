#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/manifest/manifest.h"
#include "goober/router/table.h"
#include "goober/runtime_wasm/engine.h"
#include "goober/wal/wal.h"

namespace goober::boxes {

// One deployed version of a box, ready to run.
struct ActiveBox {
  std::string version;
  manifest::Manifest manifest;
  std::shared_ptr<const runtime::wasm::CompiledBox> compiled;
};

struct RouteRef {
  std::shared_ptr<const ActiveBox> box;
  const manifest::HttpRoute* route = nullptr;
};

// An immutable snapshot of every active box and the route tables built from them.
// Requests hold a snapshot, so an upload never changes a request in flight.
class ActiveSet {
 public:
  std::map<std::string, std::shared_ptr<const ActiveBox>> boxes;

  // The route table for a domain: its own table when some manifest names it,
  // or else the table of routes on "*".
  const router::RouteTable<RouteRef>& table_for(const std::string& domain) const;

  // Builds the tables. Returns false with `error` set on a conflict.
  bool build(std::string* error);

 private:
  std::map<std::string, router::RouteTable<RouteRef>> tables_;  // "*" plus every named domain
};

struct UploadResult {
  std::string version;
  std::vector<std::string> warnings;
};

struct VersionInfo {
  std::string version;
  bool active = false;
};

// Box versions on disk (storage/boxes/$box/...), and which one is active.
// All writes go through the host WAL.
class BoxRegistry {
 public:
  BoxRegistry(wal::HostWal& wal, runtime::wasm::Engine& engine);

  // A check the host adds for what this package can't see, like a manifest's schemas
  // and databases. It runs on every upload and promotion, before anything is stored.
  using ManifestCheck = std::function<bool(const manifest::Manifest&, std::string* error)>;
  void set_manifest_check(ManifestCheck check) { check_ = std::move(check); }

  // Loads every box's active version. Problems are returned as warnings.
  std::vector<std::string> load_all();

  // Checks, stores, and activates a version. `module` is binary WASM or WAT text.
  std::optional<UploadResult> upload(std::string_view manifest_text, std::string_view module, std::string* error);

  // Makes an existing version active.
  bool promote(const std::string& box, const std::string& version, std::string* error);

  std::shared_ptr<const ActiveSet> active() const;
  std::vector<std::string> box_names() const;
  std::vector<VersionInfo> versions(const std::string& box) const;
  std::optional<std::string> manifest_text(const std::string& box, const std::string& version) const;

 private:
  std::optional<ActiveBox> load_version(const std::string& box, const std::string& version, std::string* error) const;
  // Checks the candidate against every other active box, then swaps it in.
  bool activate(std::shared_ptr<const ActiveBox> candidate, const wal::Bundle& bundle, std::string* error);

  wal::HostWal& wal_;
  runtime::wasm::Engine& engine_;
  ManifestCheck check_;
  mutable std::mutex mu_;  // serializes uploads and promotions
  mutable std::mutex snapshot_mu_;
  std::shared_ptr<const ActiveSet> active_;
};

}  // namespace goober::boxes
