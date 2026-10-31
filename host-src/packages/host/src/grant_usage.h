#pragma once

// What boxes actually do with their grants (design/machine/008.Databases.md § 4), private to
// the host package. Every call's statements report the tables and columns the authorizer let
// them use; the host sums them per box version in memory, since the host started, and
// grant_usage compares them with what each strategy grants: tables never touched, writes
// never made, and `true` grants a column list would cover.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "goober/db/database.h"
#include "goober/db/schema.h"
#include "goober/json/json.h"
#include "goober/manifest/manifest.h"

namespace goober::host {

class GrantUsage {
 public:
  explicit GrantUsage(int64_t since_ms) : since_ms_(since_ms) {}

  struct Seen {
    db::TableUse use;
    int64_t calls = 0;    // calls whose statements used the table
    int64_t last_ms = 0;  // the latest of them
  };
  // Strategy -> table -> what was seen.
  using Seen2 = std::map<std::string, std::map<std::string, Seen>>;

  // One call's use, merged under a lock: a few small sets, once per call that ran SQL.
  void record(const std::string& box, const std::string& version, const db::Usage& used, int64_t now_ms);
  Seen2 of(const std::string& box, const std::string& version) const;
  int64_t since_ms() const { return since_ms_; }

 private:
  int64_t since_ms_;
  mutable std::mutex mu_;
  std::map<std::pair<std::string, std::string>, Seen2> seen_;  // (box, version)
};

// One box version's grants against its use: {box, version, since, strategies: [{strategy,
// database, tables: [{table, granted, used, calls, last_at, findings: [...]}]}], findings}.
// `schemas` gives each table's columns, so a `true` grant can be weighed. Findings, by kind:
//   unused_table      granted something, and no statement touched it
//   unused_write      insert, update, or delete granted and never done
//   unused_columns    named columns never read or updated
//   narrow_read       read: true where the columns read would do (suggested)
//   narrow_update     update: true where the columns updated would do (suggested)
boost::json::object grant_report(const manifest::Manifest& m, const std::string& version, const GrantUsage::Seen2& seen,
                                 int64_t since_ms, const db::SchemaStore* schemas);

// Upload advice (never a refusal): an `update: true` grant on a table wider than kWideTable
// columns lets the box change columns it may never need, and any added later; name them. A wide
// `read: true` is often a whole-row listing, so reads are left to grant_usage, which has evidence.
inline constexpr size_t kWideTable = 8;
std::vector<std::string> grant_advice(const manifest::Manifest& m, const db::SchemaStore& schemas);

}  // namespace goober::host
