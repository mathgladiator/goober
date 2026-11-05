#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "goober/boxes/review.h"
#include "goober/manifest/manifest.h"
#include "goober/router/table.h"
#include "goober/runtime_wasm/engine.h"
#include "goober/types/types.h"
#include "goober/wal/wal.h"

namespace goober::boxes {

// One deployed version of a box, ready to run.
struct ActiveBox {
  std::string version;
  manifest::Manifest manifest;
  std::shared_ptr<const runtime::wasm::CompiledBox> compiled;
  std::shared_ptr<const types::Types> types;  // the version's types document (036), or null
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

  // A/B splits (design/human/310.Deployments.md § Splitting traffic; design/machine/052.Splits.md):
  // a candidate version of a live box serving `share_bp` (basis points, 0 to 10000) of the
  // requests whose bucket falls under it. The route tables hold the live version; the host
  // swaps a request's RouteRef for the candidate's route of the same verb and name.
  struct Split {
    std::shared_ptr<const ActiveBox> candidate;
    int64_t share_bp = 0;
    std::string salt;  // the split's own, so each experiment assigns people afresh
    std::map<std::string, const manifest::HttpRoute*> routes;  // "verb name" -> the candidate's route
  };
  std::map<std::string, Split> splits;  // by box

  // Beta (design/machine/053.Beta.md § 3): each box's beta version, and the beta domains whose
  // tables are built from them. On a beta domain a box answers with its beta version, else its
  // live one; a box only in beta answers only there.
  std::map<std::string, std::shared_ptr<const ActiveBox>> beta;
  std::set<std::string> beta_domains;
  // The version that answers for box `name`: its beta version when `on_beta` and it has one,
  // else its live version; null when neither.
  std::shared_ptr<const ActiveBox> box_for(const std::string& name, bool on_beta) const;
  // Every box that answers on production (the live versions), or on beta (each box's beta
  // version, else its live one).
  std::map<std::string, std::shared_ptr<const ActiveBox>> versions(bool on_beta) const;
  // The candidate's route for `ref` when its box is split and `bucket` (0 to 9999) falls under
  // the share, else nullopt (the live version answers).
  std::optional<RouteRef> candidate_for(const RouteRef& ref, int64_t bucket) const;

  // The route table for a domain: its own table when some manifest names it,
  // or else the table of routes on "*".
  const router::RouteTable<RouteRef>& table_for(const std::string& domain) const;
  // The MCP tools on a domain, by name (050): its own when some manifest names it, else "*".
  const std::map<std::string, RouteRef>& tools_for(const std::string& domain) const;

  // Builds the tables. Returns false with `error` set on a conflict.
  bool build(std::string* error);

 private:
  std::map<std::string, router::RouteTable<RouteRef>> tables_;  // "*" plus every named domain
  std::map<std::string, std::map<std::string, RouteRef>> tools_;
};

struct UploadResult {
  std::string version;
  std::vector<std::string> warnings;
};

struct VersionInfo {
  std::string version;
  bool active = false;
  int64_t stored_ms = 0;  // when it was stored (its manifest's time on disk); versions are named by hash
  // Its life (design/machine/045.Limits.md § 3.1): "live", "stored", "archived", or "deleted".
  std::string state;
  int64_t inactive_since_ms = 0;  // when it stopped being live, or was stored dark; 0 while live
  int64_t archived_ms = 0, deleted_ms = 0;
  int64_t archive_at_ms = 0, delete_at_ms = 0;  // when the host will; 0 while live or kept
  std::string kept;                             // why it is kept from both, or empty
  boost::json::array live;                      // when it was live: [{from_ms?, until_ms?}]
  std::string module_sha256;                    // a deleted version's, from its tombstone
  std::string stored_by;                        // who stored it, when known
};

// How long a version no longer live is kept (045 § 3.1).
struct Retention {
  int64_t archive_after_ms = 30LL * 86400000;
  int64_t delete_after_ms = 400LL * 86400000;
  int64_t keep = 3;  // the newest versions that were live before, kept for rollback
};

struct PruneReport {
  std::vector<std::pair<std::string, std::string>> archived;  // box, version
  std::vector<boost::json::object> deleted;                   // each tombstone
};

// Why two versions of a box can't split traffic (052 § 3), or nothing: both must answer the same
// routes (kind, verb, pattern, and name) and bind the same data (strategy, schema, database,
// tenant, pattern), so a person lands on either and both read and write the same rows.
std::vector<std::string> split_refusals(const manifest::Manifest& live, const manifest::Manifest& candidate);

// Box versions on disk (storage/boxes/$box/...), and which one is active.
// All writes go through the host WAL.
class BoxRegistry {
 public:
  BoxRegistry(wal::HostWal& wal, runtime::wasm::Engine& engine);

  // A check the host adds for what this package can't see, like a manifest's schemas
  // and databases. It runs on every upload and promotion, before anything is stored.
  using ManifestCheck = std::function<bool(const manifest::Manifest&, std::string* error)>;
  void set_manifest_check(ManifestCheck check) { check_ = std::move(check); }
  // Advice the host adds to an upload's warnings, like grants wider than a box needs. It
  // never refuses anything.
  using ManifestAdvice = std::function<std::vector<std::string>(const manifest::Manifest&)>;
  void set_manifest_advice(ManifestAdvice advice) { advice_ = std::move(advice); }

  // Loads every box's active version. Problems are returned as warnings.
  std::vector<std::string> load_all();
  // Boxes with a version set active that aren't being served (a load failure, a conflict).
  std::vector<std::string> unloaded() const;

  // Checks, stores, and activates a version. `module` is binary WASM or WAT text, and
  // `types_text` is the types document the manifest's "types" names, or empty.
  std::optional<UploadResult> upload(std::string_view manifest_text, std::string_view module, std::string* error) {
    return upload(manifest_text, module, "", error);
  }
  std::optional<UploadResult> upload(std::string_view manifest_text, std::string_view module,
                                     std::string_view types_text, std::string* error);
  // Everything upload checks (the manifest, types, the host's check, the module, and conflicts
  // with the active boxes), without storing or activating anything.
  bool check(std::string_view manifest_text, std::string_view module, std::string_view types_text, std::string* error);

  // Every upload check (conflicts with the active boxes included), then stores the version
  // without making it active: promote makes it live later.
  std::optional<UploadResult> store(std::string_view manifest_text, std::string_view module,
                                    std::string_view types_text, std::string* error);

  // Makes an existing version active (and a disabled box enabled again).
  bool promote(const std::string& box, const std::string& version, std::string* error);

  // Takes a box out of the active set: none of its routes, tools, timers, mail routes, or jobs
  // answer. Its versions stay, and boxes/$box/disabled remembers the version and why, for
  // enabling it again. Returns the version that was active.
  std::optional<std::string> disable(const std::string& box, const std::string& why, std::string* error);
  struct Disabled {
    std::string version, why;
  };
  // A disabled box's last active version and why it was disabled, or nullopt.
  std::optional<Disabled> disabled(const std::string& box) const;

  // Drops a box for good (design/machine/051.Cleanup.md § 3): out of the active set if it is
  // live, its disabled record gone, every stored version deleted leaving its tombstone (with
  // "dropped": true), and boxes/$box/dropped holding `record`. The name stays usable: uploading
  // the box again starts it afresh. Answers the versions deleted.
  std::optional<std::vector<std::string>> drop(const std::string& box, const boost::json::object& record, std::string* error);
  // Splits (052): starts or changes the split of a live box to `version` at `share_bp`, keeping
  // `record` (the controller's state) in boxes/$box/split. Refused when the box isn't live, the
  // version is the live one or can't be loaded, or the two aren't compatible
  // (split_refusals). A new live version, a disable, or a drop ends the box's split.
  bool split_set(const std::string& box, const std::string& version, int64_t share_bp, const boost::json::object& record,
                 std::string* error);
  // Ends a box's split: the live version answers everything again.
  bool split_end(const std::string& box, std::string* error);
  // A box's split record, or nullopt.
  std::optional<boost::json::object> split_record(const std::string& box) const;

  // Beta (053 § 3): makes a stored version the box's beta version (a box with no live version
  // too), kept in boxes/$box/beta; refused when it conflicts on a beta domain. beta_clear makes
  // beta follow the live version again. set_beta_domains says which domains are beta.
  bool beta_set(const std::string& box, const std::string& version, std::string* error);
  bool beta_clear(const std::string& box, std::string* error);
  std::optional<std::string> beta_version(const std::string& box) const;
  void set_beta_domains(std::set<std::string> domains);

  // A dropped box's record, or nullopt (also once it is uploaded again).
  std::optional<boost::json::object> dropped(const std::string& box) const;

  std::shared_ptr<const ActiveSet> active() const;
  std::vector<std::string> box_names() const;
  std::vector<VersionInfo> versions(const std::string& box) const;
  std::optional<std::string> manifest_text(const std::string& box, const std::string& version) const;
  std::optional<std::string> types_text(const std::string& box, const std::string& version) const;
  // Any stored version, compiled and ready to call, live or not (route_try runs a dark one).
  std::shared_ptr<const ActiveBox> load(const std::string& box, const std::string& version, std::string* error) const;
  // A stored version's files, for reviewing it against another (review.h). An archived
  // version's too: archiving keeps them for the record.
  std::optional<VersionFiles> files(const std::string& box, const std::string& version) const;

  // ---- Keeping versions (045 § 3.1) ----
  //
  // A version is inactive from when it stops being live (promoted over, disabled) or, never
  // live, from when it was stored. After retention.archive_after_ms inactive it is archived: its
  // files stay and version_get and version_diff read them, but nothing tries, dry-runs, or
  // promotes it until restore(). After retention.delete_after_ms it is deleted, leaving a
  // tombstone at boxes/$box/deleted/$version.json. Never either: the live version, a disabled
  // box's version, the newest retention.keep versions that were live before, and what the
  // host's protector names (versions a kept change set may roll back to).
  void set_retention(const Retention& r);
  // Versions kept for the host's reasons, each with why, by box: every box's at once, so a
  // pruning reads the host's reasons (every change set) once, not once per box.
  using Protected = std::map<std::string, std::map<std::string, std::string>>;  // box -> version -> why
  using Protector = std::function<Protected()>;
  void set_protector(Protector p);
  // Who is storing a version now (the operator), kept in its life record as stored_by.
  void set_actor(std::function<std::string()> actor);
  // Fields the host adds to a tombstone, like who uploaded the version.
  using Annotate = std::function<boost::json::object(const std::string& box, const std::string& version)>;
  // Archives and deletes what is due. A version with no record yet (stored before records were
  // kept) starts its inactive time now.
  PruneReport prune(int64_t now_ms, const Annotate& annotate = {});
  // An archived version back to stored, its inactive time started again.
  bool restore(const std::string& box, const std::string& version, std::string* error);
  // A deleted version's tombstone, or nullopt.
  std::optional<boost::json::object> tombstone(const std::string& box, const std::string& version) const;

 private:
  // The version's life record (life.json beside its files), or an empty object.
  boost::json::object life(const std::string& box, const std::string& version) const;
  std::string life_path(const std::string& box, const std::string& version) const;
  // Adds to the bundle what a change of live version means for both lives: the old one's live
  // time ends and its inactive time starts; the new one's (in `next`) starts. False when the
  // version is live already, and nothing moves.
  bool live_moves(wal::Bundle& bundle, const std::string& box, const std::string& version, boost::json::object* next,
                  int64_t now_ms) const;
  // Why each version is kept from archiving and deleting.
  std::map<std::string, std::string> kept(const std::string& box, const std::map<std::string, boost::json::object>& lives,
                                          const Protected& protected_) const;
  Protected protected_versions() const;
  Retention retention() const;
  bool archived(const std::string& box, const std::string& version, std::string* error) const;

  std::optional<ActiveBox> load_version(const std::string& box, const std::string& version, std::string* error) const;
  // Checks the candidate against every other active box, then swaps it in.
  bool activate(std::shared_ptr<const ActiveBox> candidate, const wal::Bundle& bundle, std::string* error,
                bool dry_run = false);
  enum class Mode { kActivate, kCheck, kStore };
  std::optional<UploadResult> upload_or_check(std::string_view manifest_text, std::string_view module,
                                              std::string_view types_text, Mode mode, std::string* error);

  wal::HostWal& wal_;
  runtime::wasm::Engine& engine_;
  ManifestCheck check_;
  ManifestAdvice advice_;
  mutable std::mutex mu_;  // serializes uploads and promotions
  mutable std::mutex snapshot_mu_;
  std::shared_ptr<const ActiveSet> active_;
  mutable std::mutex policy_mu_;
  Retention retention_;
  Protector protector_;
  std::function<std::string()> actor_;
  std::set<std::string> beta_domains_;  // under mu_
};

}  // namespace goober::boxes
