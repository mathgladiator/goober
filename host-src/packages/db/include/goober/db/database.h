#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "goober/db/schema.h"
#include "goober/manifest/manifest.h"
#include "goober/sqlite/sqlite.h"

namespace goober::db {

// Tenant databases: storage/sqlite3/$tenant/databases/$database.db, created on first use
// and grown only by expanding schemas (design/machine/008.Databases.md §3). Thread-safe.
class Databases {
 public:
  explicit Databases(std::filesystem::path storage_root) : root_(std::move(storage_root)) {}

  std::filesystem::path file(std::string_view tenant, std::string_view database) const;
  bool exists(std::string_view tenant, std::string_view database) const;

  // Opens a database, creating it if needed, and applies `schema` (when given) once.
  std::unique_ptr<sqlite::Db> open(std::string_view tenant, std::string_view database, const Schema* schema,
                                   std::string* error);

  // Checks that `schema` would apply to an existing database without changing it.
  // A database that doesn't exist yet always passes.
  bool check(std::string_view tenant, std::string_view database, const Schema& schema, std::string* error) const;

  // Whether `schemas` can all apply to one database, as when boxes share it: each is
  // applied in turn to an empty one, and the first that would change another is named.
  static bool compatible(const std::vector<const Schema*>& schemas, std::string* error);

  // The tenants that have a databases directory, and the databases each one holds.
  std::vector<std::string> tenants() const;
  std::vector<std::string> databases(std::string_view tenant) const;

  // Tables (with columns, indexes, and row counts) and the schemas applied.
  bj::object describe(std::string_view tenant, std::string_view database) const;

  // Runs one operator statement: read-only unless `write`, when it may be an INSERT,
  // UPDATE, or DELETE outside the host's tables. Returns the result object, or nullopt
  // with `error` set.
  std::optional<bj::object> operate(std::string_view tenant, std::string_view database, std::string_view sql,
                                    const bj::value& params, int64_t row_limit, bool write, std::string* error) const;

 private:
  std::filesystem::path root_;
  mutable std::mutex mu_;
  std::set<std::string> applied_;  // "$file|$schema", applied in this process
};

// One request's use of its box's databases: one transaction per database touched,
// committed or rolled back as a whole (008 §6). Not thread-safe; one per call.
class Session {
 public:
  Session(Databases& dbs, std::string tenant, const std::vector<manifest::DataStrategy>& strategies,
          const SchemaStore& schemas)
      : dbs_(dbs), tenant_(std::move(tenant)), strategies_(strategies), schemas_(schemas) {}
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Runs one statement through a strategy and returns the result as JSON text:
  // {"columns","rows","changes","last_insert_id"} or {"error"}.
  std::string run(std::string_view strategy, std::string_view sql, std::string_view params);

  // Runs a statement for the host itself (like turning off a one-shot timer's row) in
  // the same transaction as the box's writes to that strategy's database, with no grants.
  bool host_exec(const manifest::DataStrategy& strategy, std::string_view sql, const bj::value& params,
                 std::string* error);
  // The host will write to this strategy's database too (the outbox, a timer turning its row
  // off): its transaction takes the write lock from the start, like a strategy that grants writes.
  void expect_writes(const manifest::DataStrategy& strategy);
  // Commits this strategy's database after every other, so it commits only if they did (the
  // outbox lives there: enqueued only if the request's writes landed).
  void commit_last(const manifest::DataStrategy& strategy);
  bool commit(std::string* error);
  void rollback();
  // How many statements ran, for the access log.
  int statements() const { return statements_; }
  // A subscription's evaluation (design/machine/036.Bus.md § 5): any statement that writes is
  // refused before it runs.
  void set_read_only(bool on) { read_only_ = on; }
  // The strategies a statement that writes ran through, for the topics they publish.
  const std::set<std::string>& wrote() const { return wrote_; }

 private:
  struct Open {
    std::unique_ptr<sqlite::Db> db;
    bool in_transaction = false;
  };
  Databases& dbs_;
  std::string tenant_;
  std::set<std::string> expect_writes_;  // strategy names
  std::set<std::string> wrote_;          // strategy names
  bool read_only_ = false;
  std::string commit_last_;              // an open_ key, "tenant/database"
  const std::vector<manifest::DataStrategy>& strategies_;
  const SchemaStore& schemas_;
  std::map<std::string, Open> open_;  // by "$tenant/$database"
  // Opens a strategy's database on first use, applying every schema bound to it.
  Open* open_database(const std::string& tenant, const std::string& database, std::string* error);
  int statements_ = 0;
  std::string broken_;  // why the request's transaction can't commit, once it can't
};

}  // namespace goober::db
