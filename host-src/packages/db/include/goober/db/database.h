#pragma once

#include <filesystem>
#include <functional>
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
  ~Databases();
  Databases(const Databases&) = delete;
  Databases& operator=(const Databases&) = delete;

  // Versions (design/machine/037.Caching.md § 3), by database file. A number in memory,
  // seeded with the time in microseconds the first time it is asked for (so it never goes
  // backwards across a restart), and moved on every commit the host makes to the file: a
  // request that wrote, an operator's db_exec, a schema applied, a drop.
  int64_t version(const std::filesystem::path& file) const;
  void bump(const std::filesystem::path& file) const;
  // PRAGMA data_version on a connection the host keeps open on the file: it moves when any
  // connection, the host's or a shell's, commits a change. nullopt when the file doesn't exist
  // (or can't be opened). At most 512 files are watched; the least recently asked closes first.
  std::optional<int64_t> data_version(const std::filesystem::path& file) const;
  // Where stamped keys come from: the host's id service (038 § 2). Set before the first open;
  // without it, an insert into a table keyed by id fails.
  void set_id_source(std::function<int64_t()> ids) { ids_ = std::move(ids); }

  std::filesystem::path file(std::string_view tenant, std::string_view database) const;
  bool exists(std::string_view tenant, std::string_view database) const;

  // Opens a database, creating it if needed, and applies `schema` (when given) once.
  // The schema applied to the database, opening it only the first time in this process.
  bool ensure_schema(std::string_view tenant, std::string_view database, const Schema& schema, std::string* error);
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

  // Forgets which schemas this process applied to a database, so the next open applies them
  // again: after its file was replaced, restored, or dropped.
  void forget(std::string_view tenant, std::string_view database);
  // Removes a database's file (and its -wal and -shm). The caller has checked no box binds it
  // and taken a snapshot (008 § 11).
  bool drop_database(std::string_view tenant, std::string_view database, std::string* error);
  // Drops one table, its indexes and the host's records of them; the schemas that declared it
  // are marked unapplied, so a box binding one later makes the table again, empty.
  bool drop_table(std::string_view tenant, std::string_view database, std::string_view table, std::string* error);

  // Runs one operator statement: read-only unless `write`, when it may be an INSERT,
  // UPDATE, or DELETE outside the host's tables. Returns the result object, or nullopt
  // with `error` set.
  std::optional<bj::object> operate(std::string_view tenant, std::string_view database, std::string_view sql,
                                    const bj::value& params, int64_t row_limit, bool write, std::string* error) const;

 private:
  std::filesystem::path root_;
  std::function<int64_t()> ids_;
  struct Watcher {
    sqlite3* db = nullptr;  // closed by the destructor, or when it is dropped from watchers_
    int64_t used = 0;
  };
  mutable std::mutex versions_mu_;
  mutable std::map<std::string, int64_t> versions_;
  mutable std::map<std::string, Watcher> watchers_;
  mutable int64_t watcher_clock_ = 0;
  mutable std::mutex mu_;
  std::set<std::string> applied_;  // "$file|$schema", applied in this process
};

// What statements actually did with one table, as the authorizer saw them: the columns read
// and updated, and whether rows were inserted or deleted (grant_usage, 008 § 4).
struct TableUse {
  std::set<std::string> read, update;
  bool insert = false, remove = false;
  void merge(const TableUse& o) {
    read.insert(o.read.begin(), o.read.end());
    update.insert(o.update.begin(), o.update.end());
    insert |= o.insert;
    remove |= o.remove;
  }
};
// Strategy -> table -> use.
using Usage = std::map<std::string, std::map<std::string, TableUse>>;

// One statement a box ran to its end, and what it cost (design/machine/037.Caching.md § 7.1):
// its time stepping, rows returned, and SQLite's own counters (sqlite3_stmt_status).
struct StatementRecord {
  std::string strategy, tenant, database, sql;
  int64_t us = 0, rows = 0, vm_steps = 0, fullscan_steps = 0, sorts = 0, autoindexes = 0;
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
  // A GET, a HEAD, or a subscription's evaluation (design/machine/037.Caching.md § 2): any
  // statement that writes is refused before it runs, and reads begin deferred.
  void set_read_only(bool on) { read_only_ = on; }
  // The strategies a statement that writes ran through, for the topics they publish.
  const std::set<std::string>& wrote() const { return wrote_; }
  // What the statements that ran without error used of their grants.
  const Usage& used() const { return used_; }
  // Where a request's SQL time went (037 § 7.1): waiting for a database's write lock (BEGIN),
  // stepping statements, and committing; and statements refused because the request is a read.
  struct Timing {
    int64_t lock_us = 0, statement_us = 0, commit_us = 0, refused_writes = 0;
  };
  const Timing& timing() const { return timing_; }
  const std::vector<StatementRecord>& statement_records() const { return statement_records_; }
  // The databases a statement ran in, and those a statement wrote, as "tenant/database".
  const std::set<std::string>& databases_read() const { return read_; }
  const std::set<std::string>& databases_written() const { return written_; }

 private:
  Usage used_;
  Timing timing_;
  std::vector<StatementRecord> statement_records_;
  std::set<std::string> read_, written_;
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
