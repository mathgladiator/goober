#include "goober/db/database.h"

#include <chrono>
#include <sqlite3.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/names.h"
#include "goober/base/time.h"

namespace goober::db {
namespace {

constexpr int kProgressSteps = 1000;
constexpr int64_t kMaxSafeInteger = (int64_t(1) << 53) - 1;

constexpr const char* kHostTables = R"sql(
CREATE TABLE IF NOT EXISTS goober_schemas (name TEXT PRIMARY KEY, applied_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS goober_columns (
  table_name TEXT NOT NULL, column_name TEXT NOT NULL, signature TEXT NOT NULL, schema TEXT NOT NULL,
  PRIMARY KEY (table_name, column_name));
CREATE TABLE IF NOT EXISTS goober_indexes (
  index_name TEXT PRIMARY KEY, table_name TEXT NOT NULL, signature TEXT NOT NULL, schema TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS goober_dropped (
  table_name TEXT NOT NULL, column_name TEXT NOT NULL, schema TEXT NOT NULL, dropped_ms INTEGER NOT NULL,
  PRIMARY KEY (table_name, column_name));
)sql";

// The pure functions a box's SQL may call (008 §5).
const std::set<std::string> kBoxFunctions = {
    "abs",        "avg",         "ceil",        "ceiling",       "char",         "coalesce",       "concat",
    "concat_ws",  "count",       "date",        "datetime",      "floor",        "format",         "glob",
    "group_concat", "hex",       "ifnull",      "iif",           "instr",        "julianday",      "length",
    "like",       "likely",      "lower",       "ltrim",         "max",          "min",            "mod",
    "nullif",     "octet_length", "pow",        "power",         "printf",       "quote",          "replace",
    "round",      "rtrim",       "sign",        "sqrt",          "strftime",     "string_agg",     "substr",
    "substring",  "sum",         "time",        "timediff",      "total",        "trim",           "trunc",
    "typeof",     "unicode",     "unixepoch",   "unlikely",      "upper",        "json",           "json_array",
    "json_array_length", "json_extract", "json_group_array", "json_group_object", "json_insert", "json_object",
    "json_patch", "json_quote",  "json_remove", "json_replace",  "json_set",     "json_type",      "json_valid",
    "->",         "->>",         "row_number",  "rank",          "dense_rank",   "lag",            "lead",
    "first_value", "last_value", "ntile"};

bool host_table(std::string_view t) { return t.starts_with("goober_") || t.starts_with("sqlite_"); }

// ---- No clock for boxes ----
//
// A box's only clock is the call's now_ms (its input, logged for replays and caching, 037).
// SQLite's date functions read the time through the VFS when given 'now' (date('now'),
// CURRENT_TIMESTAMP, date() with no value), so the package's connections go through a VFS
// that forwards everything to the default one, except that while a box's statement runs, a
// request for the time interrupts the statement instead: SQLite undoes it whole, and the box
// is told why. Operators' statements and the host's own keep the clock.
thread_local sqlite3* t_clockless = nullptr;  // the box statement's connection, while it runs
thread_local bool t_clock_asked = false;
sqlite3_vfs* g_root_vfs = nullptr;
sqlite3_vfs g_vfs;

int vfs_time_int64(sqlite3_vfs*, sqlite3_int64* out) {
  if (t_clockless) {
    t_clock_asked = true;
    sqlite3_interrupt(t_clockless);
    *out = 0;
    return SQLITE_ERROR;
  }
  return g_root_vfs->xCurrentTimeInt64(g_root_vfs, out);
}
int vfs_time(sqlite3_vfs*, double* out) {
  if (t_clockless) {
    t_clock_asked = true;
    sqlite3_interrupt(t_clockless);
    *out = 0;
    return SQLITE_ERROR;
  }
  return g_root_vfs->xCurrentTime(g_root_vfs, out);
}
int vfs_open(sqlite3_vfs*, const char* name, sqlite3_file* f, int flags, int* out) {
  return g_root_vfs->xOpen(g_root_vfs, name, f, flags, out);
}
int vfs_delete(sqlite3_vfs*, const char* name, int sync) { return g_root_vfs->xDelete(g_root_vfs, name, sync); }
int vfs_access(sqlite3_vfs*, const char* name, int flags, int* out) {
  return g_root_vfs->xAccess(g_root_vfs, name, flags, out);
}
int vfs_full(sqlite3_vfs*, const char* name, int n, char* out) { return g_root_vfs->xFullPathname(g_root_vfs, name, n, out); }
void* vfs_dlopen(sqlite3_vfs*, const char* name) { return g_root_vfs->xDlOpen(g_root_vfs, name); }
void vfs_dlerror(sqlite3_vfs*, int n, char* out) { g_root_vfs->xDlError(g_root_vfs, n, out); }
void (*vfs_dlsym(sqlite3_vfs*, void* h, const char* sym))(void) { return g_root_vfs->xDlSym(g_root_vfs, h, sym); }
void vfs_dlclose(sqlite3_vfs*, void* h) { g_root_vfs->xDlClose(g_root_vfs, h); }
int vfs_random(sqlite3_vfs*, int n, char* out) { return g_root_vfs->xRandomness(g_root_vfs, n, out); }
int vfs_sleep(sqlite3_vfs*, int us) { return g_root_vfs->xSleep(g_root_vfs, us); }
int vfs_last_error(sqlite3_vfs*, int n, char* out) {
  return g_root_vfs->xGetLastError ? g_root_vfs->xGetLastError(g_root_vfs, n, out) : 0;
}
int vfs_set_call(sqlite3_vfs*, const char* name, sqlite3_syscall_ptr p) {
  return g_root_vfs->xSetSystemCall(g_root_vfs, name, p);
}
sqlite3_syscall_ptr vfs_get_call(sqlite3_vfs*, const char* name) { return g_root_vfs->xGetSystemCall(g_root_vfs, name); }
const char* vfs_next_call(sqlite3_vfs*, const char* name) { return g_root_vfs->xNextSystemCall(g_root_vfs, name); }

// Made the default once, before the package opens a connection.
void install_clockless_vfs() {
  static const bool installed = [] {
    g_root_vfs = sqlite3_vfs_find(nullptr);
    if (!g_root_vfs || g_root_vfs->iVersion < 3) return false;
    g_vfs = *g_root_vfs;
    g_vfs.zName = "goober";
    g_vfs.pNext = nullptr;
    g_vfs.xOpen = vfs_open;
    g_vfs.xDelete = vfs_delete;
    g_vfs.xAccess = vfs_access;
    g_vfs.xFullPathname = vfs_full;
    g_vfs.xDlOpen = vfs_dlopen;
    g_vfs.xDlError = vfs_dlerror;
    g_vfs.xDlSym = vfs_dlsym;
    g_vfs.xDlClose = vfs_dlclose;
    g_vfs.xRandomness = vfs_random;
    g_vfs.xSleep = vfs_sleep;
    g_vfs.xCurrentTime = vfs_time;
    g_vfs.xGetLastError = vfs_last_error;
    g_vfs.xCurrentTimeInt64 = vfs_time_int64;
    g_vfs.xSetSystemCall = vfs_set_call;
    g_vfs.xGetSystemCall = vfs_get_call;
    g_vfs.xNextSystemCall = vfs_next_call;
    return sqlite3_vfs_register(&g_vfs, 1) == SQLITE_OK;
  }();
  (void)installed;
}

// ---- Host ids (design/machine/038.Ids.md § 3) ----
//
// goober_id() is a stamped key's default: the next id from the host's service. The table's
// BEFORE INSERT trigger accepts a row only when its key is one goober_id() just minted (so a
// box never names a key), and its AFTER INSERT trigger records the row's id for the answer.
// Statements run start to finish on the calling thread, so the tally is per thread, reset
// before each statement.
struct IdTally {
  std::set<int64_t> pending;     // minted by goober_id(), not yet taken by a row
  std::vector<int64_t> inserted;  // rows inserted with a stamped key, in order
};
thread_local IdTally t_ids;

void sql_goober_id(sqlite3_context* ctx, int, sqlite3_value**) {
  const auto* ids = static_cast<const std::function<int64_t()>*>(sqlite3_user_data(ctx));
  if (!ids || !*ids) return sqlite3_result_error(ctx, "the host's id service isn't available here", -1);
  try {
    int64_t id = (*ids)();
    t_ids.pending.insert(id);
    sqlite3_result_int64(ctx, id);
  } catch (const std::exception& e) {
    sqlite3_result_error(ctx, (std::string("the host's id service failed: ") + e.what()).c_str(), -1);
  }
}

void sql_goober_stamped(sqlite3_context* ctx, int, sqlite3_value** argv) {
  bool ok = sqlite3_value_type(argv[0]) == SQLITE_INTEGER && t_ids.pending.erase(sqlite3_value_int64(argv[0])) > 0;
  sqlite3_result_int(ctx, ok ? 1 : 0);
}

void sql_goober_inserted(sqlite3_context* ctx, int, sqlite3_value** argv) {
  if (sqlite3_value_type(argv[0]) == SQLITE_INTEGER) t_ids.inserted.push_back(sqlite3_value_int64(argv[0]));
  sqlite3_result_null(ctx);
}

// Every connection the package opens gets these; `ids` may be null (nothing is minted then).
void install_id_functions(sqlite3* db, const std::function<int64_t()>* ids) {
  sqlite3_create_function(db, "goober_id", 0, SQLITE_UTF8, const_cast<std::function<int64_t()>*>(ids), sql_goober_id,
                          nullptr, nullptr);
  sqlite3_create_function(db, "goober_stamped", 1, SQLITE_UTF8, nullptr, sql_goober_stamped, nullptr, nullptr);
  sqlite3_create_function(db, "goober_inserted", 1, SQLITE_UTF8, nullptr, sql_goober_inserted, nullptr, nullptr);
}

// The triggers that keep a stamped key the host's, made with its table.
std::string id_triggers(const Table& table, const Column& key) {
  std::string t = "\"" + table.name + "\"", k = "NEW.\"" + key.name + "\"";
  return "CREATE TRIGGER \"goober_ids_" + table.name + "_insert\" BEFORE INSERT ON " + t + " WHEN NOT goober_stamped(" + k +
         ") BEGIN SELECT RAISE(ABORT, 'a box never chooses a primary key: leave " + key.name +
         " out of the insert, and the host stamps one'); END;\n"
         "CREATE TRIGGER \"goober_ids_" + table.name + "_inserted\" AFTER INSERT ON " + t +
         " BEGIN SELECT goober_inserted(" + k + "); END;\n"
         "CREATE TRIGGER \"goober_ids_" + table.name + "_update\" BEFORE UPDATE OF \"" + key.name + "\" ON " + t +
         " WHEN " + k + " IS NOT OLD.\"" + key.name + "\" BEGIN SELECT RAISE(ABORT, 'a primary key never changes: " +
         key.name + " is the host\'\'s'); END;\n";
}

struct Auth {
  const manifest::DataStrategy* strategy = nullptr;  // null for an operator
  bool write = false;                                // an operator's db_exec
  std::set<std::string> tables;                      // the database's real tables; anything else read is a CTE
  std::string denied;
  std::map<std::string, TableUse> uses;              // what the statement was allowed to do, by table
  // What the statement cost, once it ran to its end (037 § 7.1).
  struct Cost {
    bool ran = false;
    int64_t us = 0, rows = 0, vm_steps = 0, fullscan_steps = 0, sorts = 0, autoindexes = 0;
  } cost;
};

// A second check, independent of the authorizer: the compiled program itself. SQLite's
// transfer optimization ("INSERT INTO a SELECT * FROM b") copies raw rows without asking
// the authorizer about the columns it reads, so a box could copy a table it has no grant
// on into one it can read. Every table or index the program opens (by root page) must
// belong to a table the strategy grants something on, and a program that copies raw rows
// (RowData) may only read tables the strategy reads in full. Found by the SQL property
// test (013.SecurityReview.md).
// Root page -> the table it belongs to; read before the box's authorizer is set.
std::map<int64_t, std::string> root_pages(sqlite3* db) {
  std::map<int64_t, std::string> owner;
  sqlite3_stmt* q = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT rootpage, tbl_name FROM sqlite_master WHERE rootpage > 0", -1, &q, nullptr) ==
      SQLITE_OK) {
    while (sqlite3_step(q) == SQLITE_ROW) {
      owner[sqlite3_column_int64(q, 0)] = reinterpret_cast<const char*>(sqlite3_column_text(q, 1));
    }
  }
  sqlite3_finalize(q);
  return owner;
}

std::string check_program(sqlite3_stmt* st, const manifest::DataStrategy& strategy,
                          const std::map<int64_t, std::string>& owner) {
  if (sqlite3_stmt_explain(st, 1) != SQLITE_OK) return "the statement can't be inspected";
  std::set<std::string> read_tables;
  bool raw_copy = false;
  std::string why;
  while (sqlite3_step(st) == SQLITE_ROW) {
    std::string op = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    int64_t p2 = sqlite3_column_int64(st, 3);
    int64_t p3 = sqlite3_column_int64(st, 4);
    if (op == "RowData") raw_copy = true;
    if (op != "OpenRead" && op != "OpenWrite" && op != "ReopenIdx") continue;
    if (p3 != 0) {
      why = "statements may only use the main database";
      break;
    }
    auto it = owner.find(p2);
    std::string table = it == owner.end() ? "" : it->second;
    if (table.empty() || host_table(table) || !strategy.tables.count(table)) {
      why = "strategy " + strategy.name + " can't touch table " + (table.empty() ? std::string("?") : table);
      break;
    }
    if (op != "OpenWrite") read_tables.insert(table);
  }
  sqlite3_reset(st);
  sqlite3_stmt_explain(st, 0);
  if (!why.empty()) return why;
  if (raw_copy) {
    for (const auto& t : read_tables) {
      if (!strategy.tables.at(t).read_all) {
        return "copying whole rows needs read on every column of " + t + "; name the columns";
      }
    }
  }
  return "";
}

std::set<std::string> real_tables(sqlite3* db) {
  std::set<std::string> out;
  sqlite3_stmt* q = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type IN ('table', 'view')", -1, &q, nullptr) == SQLITE_OK) {
    while (sqlite3_step(q) == SQLITE_ROW) out.insert(reinterpret_cast<const char*>(sqlite3_column_text(q, 0)));
  }
  sqlite3_finalize(q);
  return out;
}

int authorize(void* p, int code, const char* a, const char* b, const char*, const char* inner) {
  auto& auth = *static_cast<Auth*>(p);
  std::string t = a ? a : "", c = b ? b : "";
  auto deny = [&](std::string why) {
    if (auth.denied.empty()) auth.denied = std::move(why);
    return SQLITE_DENY;
  };
  // The host's tombstone triggers (041 § 5) keep a dropped column at its old value and a dropped
  // table empty, whatever the box may do: they are the host's writes, not the box's.
  if (auth.strategy && inner && std::string_view(inner).starts_with("goober_dropped_") &&
      (code == SQLITE_READ || code == SQLITE_UPDATE || code == SQLITE_DELETE)) {
    return SQLITE_OK;
  }
  const manifest::TableGrant* g = nullptr;
  if (auth.strategy) {
    auto it = auth.strategy->tables.find(t);
    if (it != auth.strategy->tables.end()) g = &it->second;
  }
  switch (code) {
    case SQLITE_SELECT:
    case SQLITE_RECURSIVE:
      return SQLITE_OK;
    case SQLITE_READ:
      if (!auth.strategy) return SQLITE_OK;
      // The host's id triggers read the new row's key; nothing they read reaches the box.
      if (inner && std::string_view(inner).starts_with("goober_ids_")) return SQLITE_OK;
      // A common table expression or subquery: its own reads are checked one by one.
      if (!g && !auth.tables.count(t) && !host_table(t)) return SQLITE_OK;
      if (!g) return deny("strategy " + auth.strategy->name + " can't read table " + t);
      if (!g->read_all && !g->read.count(c)) return deny("strategy " + auth.strategy->name + " can't read " + t + "." + c);
      if (!c.empty()) auth.uses[t].read.insert(c);
      return SQLITE_OK;
    case SQLITE_INSERT:
    case SQLITE_DELETE:
    case SQLITE_UPDATE: {
      const char* op = code == SQLITE_INSERT ? "insert into" : code == SQLITE_DELETE ? "delete from" : "update";
      // A change to sqlite_master is SQLite carrying out DDL, which the next check would refuse anyway.
      if (t.starts_with("sqlite_")) return deny("schema changes go through schemas (schema_put and a box's manifest)");
      if (!auth.strategy) {
        if (!auth.write) return deny("db_query is read-only; use db_exec to change rows");
        if (host_table(t)) return deny(t + " belongs to the host");
        return SQLITE_OK;
      }
      if (!g) return deny("strategy " + auth.strategy->name + " can't " + op + " " + t);
      if (code == SQLITE_INSERT && !g->insert) return deny("strategy " + auth.strategy->name + " can't insert into " + t);
      if (code == SQLITE_DELETE && !g->remove) return deny("strategy " + auth.strategy->name + " can't delete from " + t);
      if (code == SQLITE_UPDATE && !g->update_all && !g->update.count(c)) {
        return deny("strategy " + auth.strategy->name + " can't update " + t + "." + c);
      }
      auto& use = auth.uses[t];
      if (code == SQLITE_INSERT) use.insert = true;
      if (code == SQLITE_DELETE) use.remove = true;
      if (code == SQLITE_UPDATE && !c.empty()) use.update.insert(c);
      return SQLITE_OK;
    }
    case SQLITE_FUNCTION:
      if (!auth.strategy) return SQLITE_OK;
      if (kBoxFunctions.count(base::to_lower(c))) return SQLITE_OK;
      if (std::string f = base::to_lower(c); f == "current_timestamp" || f == "current_date" || f == "current_time") {
        return deny("box SQL has no clock: " + f + " is refused. Pass the call's now_ms as a parameter instead, like "
                    "datetime(:now_ms / 1000, 'unixepoch')");
      }
      // The host's id functions: goober_id() anywhere (it only ever hands out a fresh id); the
      // others only inside the host's own id triggers.
      if (base::to_lower(c) == "goober_id") return SQLITE_OK;
      if (inner && std::string_view(inner).starts_with("goober_ids_") &&
          (base::to_lower(c) == "goober_stamped" || base::to_lower(c) == "goober_inserted")) {
        return SQLITE_OK;
      }
      return deny("function " + c + "() isn't on the host's list of pure functions");
    case SQLITE_PRAGMA:
      return deny("PRAGMA statements are refused");
    case SQLITE_ATTACH:
    case SQLITE_DETACH:
      return deny("ATTACH and DETACH are refused: a statement stays in its database");
    case SQLITE_TRANSACTION:
    case SQLITE_SAVEPOINT:
      return deny("transactions belong to the host: one per request");
    default:
      return deny("schema changes go through schemas (schema_put and a box's manifest)");
  }
}

// Limits on one statement that no manifest can raise: its wall time, the size of any one
// value it builds, the size of its whole result, and LIKE patterns. Each keeps one box from
// pinning a thread or the host's memory.
constexpr int64_t kStatementMs = 2000;
constexpr int kMaxValueBytes = 4 << 20;
constexpr size_t kMaxResultBytes = 8 << 20;
constexpr int kMaxColumns = 128;
constexpr int kMaxLikePattern = 1000;

struct Progress {
  int64_t steps = 0;
  int64_t budget = 0;
  std::chrono::steady_clock::time_point deadline;
  bool late = false;
};

int on_progress(void* p) {
  auto& pr = *static_cast<Progress*>(p);
  pr.steps += kProgressSteps;
  if (std::chrono::steady_clock::now() > pr.deadline) pr.late = true;
  return pr.steps > pr.budget || pr.late ? 1 : 0;
}

// Only spaces and semicolons in [tail, end): the text after the one statement. The SQL
// is a view into box memory with no terminator, so the scan must stop at `end`.
bool only_space(const char* tail, const char* end) {
  for (; tail && tail < end; ++tail) {
    if (*tail != ' ' && *tail != '\n' && *tail != '\t' && *tail != '\r' && *tail != ';') return false;
  }
  return true;
}

// REPLACE conflict resolution deletes the row it collides with, and SQLite's authorizer
// reports only an insert or an update, so a box could overwrite or delete rows it was
// never granted. "REPLACE INTO" and "OR REPLACE" are refused; replace(...) the function
// is fine. Strings, quoted names, and comments are skipped.
bool uses_replace_conflict(std::string_view sql) {
  std::vector<std::string> words;
  size_t i = 0;
  while (i < sql.size()) {
    char c = sql[i];
    if (c == '\'' || c == '"' || c == '`' || c == '[') {
      char close = c == '[' ? ']' : c;
      for (++i; i < sql.size() && sql[i] != close; ++i) {
      }
      ++i;
      words.emplace_back("\x01");  // a literal or a quoted name
    } else if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      while (i < sql.size() && sql[i] != '\n') ++i;
    } else if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      auto end = sql.find("*/", i + 2);
      i = end == std::string_view::npos ? sql.size() : end + 2;
    } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t start = i;
      while (i < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) ++i;
      std::string w(sql.substr(start, i - start));
      for (auto& ch : w) ch = char(std::toupper(static_cast<unsigned char>(ch)));
      words.push_back(std::move(w));
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
    } else {
      words.emplace_back(1, c);
      ++i;
    }
  }
  for (size_t k = 0; k < words.size(); ++k) {
    // OR ROLLBACK would end the host's own transaction for the whole request.
    if (words[k] == "ROLLBACK" && k > 0 && words[k - 1] == "OR") return true;
    if (words[k] != "REPLACE") continue;
    bool call = k + 1 < words.size() && words[k + 1] == "(";
    bool after_or = k > 0 && words[k - 1] == "OR";
    bool into = k + 1 < words.size() && words[k + 1] == "INTO";
    if (into || (after_or && !call)) return true;
  }
  return false;
}

bj::value column_value(sqlite3_stmt* s, int i) {
  switch (sqlite3_column_type(s, i)) {
    case SQLITE_INTEGER: {
      int64_t v = sqlite3_column_int64(s, i);
      // An id column (declared INT, 038 § 4) crosses JSON as a string, so no client rounds it.
      if (const char* decl = sqlite3_column_decltype(s, i); decl && std::string_view(decl) == "INT") {
        return bj::value(std::to_string(v));
      }
      if (v > kMaxSafeInteger || v < -kMaxSafeInteger) return bj::value(std::to_string(v));
      return bj::value(v);
    }
    case SQLITE_FLOAT:
      return bj::value(sqlite3_column_double(s, i));
    case SQLITE_TEXT:
      return bj::value(std::string(reinterpret_cast<const char*>(sqlite3_column_text(s, i)),
                                   size_t(sqlite3_column_bytes(s, i))));
    case SQLITE_BLOB: {
      std::string blob(static_cast<const char*>(sqlite3_column_blob(s, i)), size_t(sqlite3_column_bytes(s, i)));
      return bj::object{{"base64", base::base64_encode(blob)}};
    }
    default:
      return nullptr;
  }
}

bool bind_value(sqlite3_stmt* s, int i, const bj::value& v, std::string* error) {
  switch (v.kind()) {
    case bj::kind::null: sqlite3_bind_null(s, i); return true;
    case bj::kind::bool_: sqlite3_bind_int64(s, i, v.get_bool() ? 1 : 0); return true;
    case bj::kind::int64: sqlite3_bind_int64(s, i, v.get_int64()); return true;
    case bj::kind::uint64:
      if (v.get_uint64() > uint64_t(std::numeric_limits<int64_t>::max())) return *error = "an integer is too large", false;
      sqlite3_bind_int64(s, i, int64_t(v.get_uint64()));
      return true;
    case bj::kind::double_: sqlite3_bind_double(s, i, v.get_double()); return true;
    case bj::kind::string:
      sqlite3_bind_text(s, i, v.get_string().data(), int(v.get_string().size()), SQLITE_TRANSIENT);
      return true;
    default:
      return *error = "a parameter is null, a boolean, a number, or a string; pass JSON as a string", false;
  }
}

struct Stmt {
  sqlite3_stmt* s = nullptr;
  ~Stmt() { sqlite3_finalize(s); }
};

// Prepares, checks, binds, and runs one statement. `before_step` runs after the checks
// and before the first step, given whether the statement only reads; returning an
// error string stops it. Errors come back as {"error": ...}.
bj::object run_statement(sqlite3* db, std::string_view sql, const bj::value& params, Auth& auth, int64_t row_limit,
                         int64_t step_budget, const std::function<std::string(bool readonly)>& before_step) {
  auto fail = [](std::string why) { return bj::object{{"error", std::move(why)}}; };
  t_ids = {};
  if (sql.size() > 100000) return fail("the statement is longer than 100000 bytes");
  if (auth.strategy && uses_replace_conflict(sql)) {
    return fail("REPLACE and ROLLBACK conflict resolution are refused (they delete rows behind the grants, or end the "
                "request's transaction); use INSERT ... ON CONFLICT DO UPDATE or DO NOTHING");
  }
  auth.tables = real_tables(db);
  auto owner = auth.strategy ? root_pages(db) : std::map<int64_t, std::string>{};
  sqlite3_limit(db, SQLITE_LIMIT_LENGTH, kMaxValueBytes);
  sqlite3_limit(db, SQLITE_LIMIT_LIKE_PATTERN_LENGTH, kMaxLikePattern);
  // At most 128 result columns: 2000 columns of a 4 MiB value would be gigabytes in one row.
  sqlite3_limit(db, SQLITE_LIMIT_COLUMN, kMaxColumns);
  // A backstop for all of SQLite in this process: past it, statements fail out of memory
  // instead of the kernel ending the host.
  static const bool heap_limited = [] {
    sqlite3_hard_heap_limit64(int64_t(1536) << 20);
    return true;
  }();
  (void)heap_limited;
  // The authorizer stays on until the statement is finished: SQLite re-prepares a statement
  // whose schema changed underneath it, and that must be checked too.
  sqlite3_set_authorizer(db, authorize, &auth);
  struct AuthorizerOff {
    sqlite3* db;
    ~AuthorizerOff() { sqlite3_set_authorizer(db, nullptr, nullptr); }
  } authorizer_off{db};
  Stmt st;
  const char* tail = nullptr;
  int rc = sqlite3_prepare_v2(db, sql.data(), int(sql.size()), &st.s, &tail);
  if (rc != SQLITE_OK) return fail(!auth.denied.empty() ? auth.denied : std::string(sqlite3_errmsg(db)));
  if (!st.s) return fail("the statement is empty");
  if (auth.strategy) {
    if (auto why = check_program(st.s, *auth.strategy, owner); !why.empty()) return fail(why);
  }
  if (tail && !only_space(tail, sql.data() + sql.size())) return fail("one statement at a time");
  int count = sqlite3_bind_parameter_count(st.s);
  std::string err;
  if (params.is_array()) {
    const auto& arr = params.get_array();
    if (int(arr.size()) != count) {
      return fail("the statement takes " + std::to_string(count) + " parameters, and " + std::to_string(arr.size()) +
                  " were given");
    }
    for (int i = 0; i < count; ++i) {
      if (!bind_value(st.s, i + 1, arr[size_t(i)], &err)) return fail(err);
    }
  } else if (params.is_object()) {
    for (int i = 1; i <= count; ++i) {
      const char* name = sqlite3_bind_parameter_name(st.s, i);
      if (!name || !*name || *name == '?') return fail("mix of ? and named parameters; give an array for ?");
      auto it = params.get_object().find(name + 1);
      if (it == params.get_object().end()) return fail(std::string("no value for parameter ") + name);
      if (!bind_value(st.s, i, it->value(), &err)) return fail(err);
    }
  } else if (!params.is_null() || count > 0) {
    if (count > 0) return fail("the statement takes " + std::to_string(count) + " parameters");
  }
  bool readonly = sqlite3_stmt_readonly(st.s) != 0;
  if (before_step) {
    // The host's own BEGIN runs here, outside the box's authorizer.
    sqlite3_set_authorizer(db, nullptr, nullptr);
    auto why = before_step(readonly);
    sqlite3_set_authorizer(db, authorize, &auth);
    if (!why.empty()) return fail(why);
  }
  Progress progress{0, step_budget, std::chrono::steady_clock::now() + std::chrono::milliseconds(kStatementMs), false};
  sqlite3_progress_handler(db, kProgressSteps, on_progress, &progress);
  // A box's statement has no clock: asking for the time interrupts it (see install_clockless_vfs).
  t_clock_asked = false;
  struct Clockless {
    explicit Clockless(sqlite3* db) { t_clockless = db; }
    ~Clockless() { t_clockless = nullptr; }
  };
  std::optional<Clockless> clockless;
  if (auth.strategy) clockless.emplace(db);
  auto step_started = std::chrono::steady_clock::now();
  bj::array columns;
  int ncol = sqlite3_column_count(st.s);
  for (int i = 0; i < ncol; ++i) columns.push_back(bj::value(sqlite3_column_name(st.s, i)));
  bj::array rows;
  int64_t n = 0;
  size_t result_bytes = 0;
  while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {
    if (++n > row_limit) {
      sqlite3_progress_handler(db, 0, nullptr, nullptr);
      return fail("more than " + std::to_string(row_limit) + " rows; add a LIMIT, or raise row-limit");
    }
    bj::array row;
    for (int i = 0; i < ncol; ++i) {
      // Checked before each column is copied, not after the row: a row is the size of all of it.
      result_bytes += size_t(sqlite3_column_bytes(st.s, i)) + 16;
      if (result_bytes > kMaxResultBytes) {
        sqlite3_progress_handler(db, 0, nullptr, nullptr);
        return fail("the result is larger than " + std::to_string(kMaxResultBytes) + " bytes; select less");
      }
      row.push_back(column_value(st.s, i));
    }
    rows.push_back(std::move(row));
  }
  // SQLite's own counters say what the statement cost: a full scan or an automatic index in
  // them is an index the schema lacks (037 § 7.1).
  auth.cost.ran = true;
  auth.cost.us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - step_started).count();
  auth.cost.rows = n;
  auth.cost.vm_steps = sqlite3_stmt_status(st.s, SQLITE_STMTSTATUS_VM_STEP, 0);
  auth.cost.fullscan_steps = sqlite3_stmt_status(st.s, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0);
  auth.cost.sorts = sqlite3_stmt_status(st.s, SQLITE_STMTSTATUS_SORT, 0);
  auth.cost.autoindexes = sqlite3_stmt_status(st.s, SQLITE_STMTSTATUS_AUTOINDEX, 0);
  sqlite3_progress_handler(db, 0, nullptr, nullptr);
  clockless.reset();
  if (rc == SQLITE_INTERRUPT && t_clock_asked) {
    return fail("box SQL has no clock: date('now'), CURRENT_TIMESTAMP, CURRENT_DATE, and date or time functions with no "
                "value are refused. Pass the call's now_ms as a parameter instead, like datetime(:now_ms / 1000, 'unixepoch')");
  }
  if (rc == SQLITE_INTERRUPT && progress.late) {
    return fail("the statement ran longer than " + std::to_string(kStatementMs) + " ms");
  }
  if (rc == SQLITE_INTERRUPT && progress.steps > progress.budget) {
    return fail("the statement used more than its step budget of " + std::to_string(step_budget));
  }
  if (rc != SQLITE_DONE) return fail(sqlite3_errmsg(db));
  bj::object out{{"columns", std::move(columns)},
                 {"rows", std::move(rows)},
                 {"changes", readonly ? 0 : sqlite3_changes(db)},
                 {"last_insert_id", readonly ? 0 : sqlite3_last_insert_rowid(db)}};
  // The ids the host stamped on inserted rows (038 § 3), as strings.
  if (!t_ids.inserted.empty()) {
    bj::array ids;
    for (int64_t id : t_ids.inserted) ids.push_back(bj::value(std::to_string(id)));
    out["id"] = ids.back();
    out["ids"] = std::move(ids);
  }
  return out;
}

// How a schema is applied to an open database (008 § 3; design/machine/041.SchemaEvolution.md § 4).
struct ApplyContext {
  bool dry_run = false;
  const SchemaStore* store = nullptr;  // where a `follows` is looked up, and approvals read
  bool require_approval = true;        // a step that drops or rewrites data waits for a person (041 § 6)
  std::function<bool(const std::string& note, std::string* error)> before_change;  // the snapshot
  const std::map<std::string, RowTransform>* rows = nullptr;  // forced tables' handlers (041 § 7)
  bool keep_going = false;
  UpgradeReport* report = nullptr;
  std::vector<std::string>* pending = nullptr;  // a dry run's forced tables that still hold rows
};

std::string ident(const std::string& name) { return "\"" + name + "\""; }

bool is_applied(sqlite::Db& db, const std::string& name) {
  sqlite::Stmt q(db, "SELECT 1 FROM goober_schemas WHERE name = ?");
  q.bind(1, name);
  return q.step();
}

bool table_exists(sqlite::Db& db, const std::string& table) {
  sqlite::Stmt q(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
  q.bind(1, table);
  return q.step();
}

std::set<std::string> columns_present(sqlite::Db& db, const std::string& table) {
  std::set<std::string> out;
  sqlite::Stmt q(db, "SELECT name FROM pragma_table_info(?)");
  q.bind(1, table);
  while (q.step()) out.insert(q.text(0));
  return out;
}

int64_t count_rows(sqlite::Db& db, const std::string& table, const std::string& where = "") {
  sqlite::Stmt q(db, "SELECT count(*) FROM " + ident(table) + (where.empty() ? "" : " WHERE " + where));
  return q.step() ? q.int64(0) : 0;
}

// The schema that dropped a table (column "") or a column, if one did: its name is never used again.
std::optional<std::string> dropped_by(sqlite::Db& db, const std::string& table, const std::string& column) {
  sqlite::Stmt q(db, "SELECT schema FROM goober_dropped WHERE table_name = ? AND column_name = ?");
  q.bind(1, table).bind(2, column);
  if (q.step()) return q.text(0);
  return std::nullopt;
}

void record_dropped(sqlite::Db& db, const std::string& table, const std::string& column, const std::string& schema) {
  sqlite::Stmt ins(db, "INSERT OR IGNORE INTO goober_dropped (table_name, column_name, schema, dropped_ms) VALUES (?, ?, ?, ?)");
  ins.bind(1, table).bind(2, column).bind(3, schema).bind(4, base::now_unix_ms()).run();
}

void record_column(sqlite::Db& db, const std::string& table, const Column& c, const std::string& schema) {
  sqlite::Stmt ins(db, "INSERT OR REPLACE INTO goober_columns (table_name, column_name, signature, schema) VALUES (?, ?, ?, ?)");
  ins.bind(1, table).bind(2, c.name).bind(3, c.signature()).bind(4, schema).run();
}

void create_index(sqlite::Db& db, const std::string& table, const Index& idx, const std::string& schema) {
  {
    sqlite::Stmt q(db, "SELECT table_name FROM goober_indexes WHERE index_name = ?");
    q.bind(1, idx.name);
    if (q.step()) throw std::runtime_error("index " + idx.name + " is already on table " + q.text(0));
  }
  std::string sql = std::string(idx.unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ") + ident(idx.name) + " ON " +
                    ident(table) + " (";
  for (size_t i = 0; i < idx.columns.size(); ++i) sql += (i ? ", " : "") + ident(idx.columns[i]);
  try {
    db.exec(sql + ")");
  } catch (const std::exception& e) {
    throw std::runtime_error("index " + idx.name + " on " + table + ": " + e.what());
  }
  sqlite::Stmt ins(db, "INSERT INTO goober_indexes (index_name, table_name, signature, schema) VALUES (?, ?, ?, ?)");
  ins.bind(1, idx.name).bind(2, table).bind(3, idx.signature()).bind(4, schema).run();
}

void drop_index(sqlite::Db& db, const std::string& name) {
  db.exec("DROP INDEX IF EXISTS " + ident(name));
  sqlite::Stmt d(db, "DELETE FROM goober_indexes WHERE index_name = ?");
  d.bind(1, name).run();
}

void create_table(sqlite::Db& db, const Table& table, const std::string& name) {
  std::string sql = "CREATE TABLE " + ident(name) + " (";
  for (size_t i = 0; i < table.columns.size(); ++i) sql += (i ? ", " : "") + table.columns[i].sql();
  db.exec(sql + ") STRICT");
}

// The column definitions of a table, from the CREATE TABLE text SQLite keeps (ALTER TABLE ADD
// COLUMN appends to it), split at the commas outside quotes and parentheses; and whether it is STRICT.
std::vector<std::string> column_defs(sqlite::Db& db, const std::string& table, bool* strict) {
  std::string sql;
  {
    sqlite::Stmt q(db, "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = ?");
    q.bind(1, table);
    if (!q.step()) throw std::runtime_error("no table " + table);
    sql = q.text(0);
  }
  size_t open = std::string::npos;
  char quote = 0;
  for (size_t i = 0; i < sql.size() && open == std::string::npos; ++i) {
    char c = sql[i];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'' || c == '`') {
      quote = c;
    } else if (c == '(') {
      open = i;
    }
  }
  size_t close = sql.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) throw std::runtime_error("can't read table " + table);
  *strict = sql.substr(close).find("STRICT") != std::string::npos;
  std::vector<std::string> defs;
  std::string cur;
  int depth = 0;
  quote = 0;
  for (size_t i = open + 1; i < close; ++i) {
    char c = sql[i];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'' || c == '`') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
    } else if (c == ',' && depth == 0) {
      defs.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  defs.push_back(cur);
  for (auto& d : defs) {
    size_t a = d.find_first_not_of(" \t\n"), b = d.find_last_not_of(" \t\n");
    d = a == std::string::npos ? "" : d.substr(a, b - a + 1);
  }
  return defs;
}

// A column definition's name: its first token, unquoted.
std::string def_name(const std::string& def) {
  if (!def.empty() && def[0] == '"') {
    std::string out;
    for (size_t i = 1; i < def.size(); ++i) {
      if (def[i] == '"') {
        if (i + 1 < def.size() && def[i + 1] == '"') {
          out += '"';
          ++i;
          continue;
        }
        break;
      }
      out += def[i];
    }
    return out;
  }
  return def.substr(0, def.find_first_of(" \t"));
}

// Rebuilds a table with some columns' definitions replaced (041 § 4): a new table, every row
// copied, the old one dropped, the new one renamed, and its indexes and triggers made again.
void rebuild_table(sqlite::Db& db, const std::string& table, const std::map<std::string, std::string>& defs) {
  bool strict = false;
  auto cols = column_defs(db, table, &strict);
  std::string body, list;
  for (const auto& d : cols) {
    std::string name = def_name(d);
    auto it = defs.find(name);
    body += (body.empty() ? "" : ", ") + (it == defs.end() ? d : it->second);
    list += (list.empty() ? "" : ", ") + ident(name);
  }
  std::vector<std::string> again;
  {
    sqlite::Stmt q(db, "SELECT sql FROM sqlite_master WHERE tbl_name = ? AND type IN ('index', 'trigger') AND sql IS NOT NULL");
    q.bind(1, table);
    while (q.step()) again.push_back(q.text(0));
  }
  db.exec("CREATE TABLE \"goober_rebuild\" (" + body + ")" + (strict ? " STRICT" : ""));
  db.exec("INSERT INTO \"goober_rebuild\" (" + list + ") SELECT " + list + " FROM " + ident(table));
  db.exec("DROP TABLE " + ident(table));
  db.exec("ALTER TABLE \"goober_rebuild\" RENAME TO " + ident(table));
  for (const auto& sql : again) db.exec(sql);
}

std::string trigger_base(const std::string& table) {
  return "goober_dropped_" + std::to_string(table.size()) + "_" + table;
}

// A dropped column (041 § 5): cleared to what older boxes read, and kept there by two triggers.
void tombstone_column(sqlite::Db& db, const std::string& table, const Column& c) {
  std::string v = c.dropped_value_sql(), col = ident(c.name), t = ident(table), base = trigger_base(table) + "_" + c.name;
  db.exec("UPDATE " + t + " SET " + col + " = " + v + " WHERE " + col + " IS NOT " + v);
  db.exec("CREATE TRIGGER " + ident(base + "_insert") + " AFTER INSERT ON " + t + " WHEN NEW." + col + " IS NOT " + v +
          " BEGIN UPDATE " + t + " SET " + col + " = " + v + " WHERE rowid = NEW.rowid; END");
  db.exec("CREATE TRIGGER " + ident(base + "_update") + " AFTER UPDATE OF " + col + " ON " + t + " WHEN NEW." + col +
          " IS NOT " + v + " BEGIN UPDATE " + t + " SET " + col + " = " + v + " WHERE rowid = NEW.rowid; END");
}

// A dropped table (041 § 5): emptied, and kept empty by a trigger; its indexes go.
void tombstone_table(sqlite::Db& db, const std::string& table, const std::string& schema) {
  if (!table_exists(db, table)) return;
  db.exec("DELETE FROM " + ident(table));
  db.exec("CREATE TRIGGER " + ident(trigger_base(table) + "_rows") + " AFTER INSERT ON " + ident(table) +
          " BEGIN DELETE FROM " + ident(table) + " WHERE rowid = NEW.rowid; END");
  std::vector<std::string> indexes;
  {
    sqlite::Stmt q(db, "SELECT index_name FROM goober_indexes WHERE table_name = ?");
    q.bind(1, table);
    while (q.step()) indexes.push_back(q.text(0));
  }
  for (const auto& i : indexes) drop_index(db, i);
  sqlite::Stmt d(db, "DELETE FROM goober_columns WHERE table_name = ?");
  d.bind(1, table).run();
  record_dropped(db, table, "", schema);
}

std::string text_of(const bj::value& v) {
  if (v.is_string()) return std::string(v.get_string());
  return json::dump(v);
}

// A forced table (041 § 7): built in the new shape, every row through the upgrade box's handler,
// checked by the new shape's constraints, then swapped in.
void rebuild_forced(sqlite::Db& db, const Table& from_t, const Table& to_t, const std::string& schema, ApplyContext& ctx) {
  const std::string& t = to_t.name;
  const RowTransform* handler = nullptr;
  if (ctx.rows) {
    if (auto it = ctx.rows->find(t); it != ctx.rows->end()) handler = &it->second;
  }
  auto present = columns_present(db, t);
  std::vector<std::string> from_cols;
  for (const auto& c : from_t.columns) {
    if (present.count(c.name)) from_cols.push_back(c.name);
  }
  std::string key;
  for (const auto& c : from_t.columns) {
    if (c.primary_key) key = c.name;
  }
  create_table(db, to_t, "goober_rebuild");
  std::string list;
  for (const auto& c : from_cols) list += (list.empty() ? "" : ", ") + ident(c);
  sqlite3* raw = db.raw();
  Stmt sel;
  std::string sql = "SELECT " + list + " FROM " + ident(t) + " ORDER BY rowid";
  if (sqlite3_prepare_v2(raw, sql.c_str(), -1, &sel.s, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(raw));
  int64_t failed = 0;
  std::string first;
  while (sqlite3_step(sel.s) == SQLITE_ROW) {
    bj::object row;
    for (int i = 0; i < int(from_cols.size()); ++i) row[from_cols[size_t(i)]] = column_value(sel.s, i);
    if (ctx.report) ++ctx.report->rows;
    std::string why;
    std::optional<bj::object> out;
    if (!handler) {
      why = "no upgrade handler for " + t;
    } else {
      out = (*handler)(row, &why);
      if (!out && why.empty()) why = "the handler answered nothing";
    }
    if (out) {
      const auto* k = out->if_contains(key);
      if (!k || text_of(*k) != text_of(row.at(key))) why = "the handler changed the primary key " + key + "; it never changes";
      for (const auto& kv : *out) {
        if (why.empty() && !to_t.column(kv.key())) why = "the handler answered " + std::string(kv.key()) + ", which " + schema + "'s " + t + " doesn't have";
      }
    }
    if (out && why.empty()) {
      std::string cols, marks;
      for (const auto& kv : *out) {
        cols += (cols.empty() ? "" : ", ") + ident(std::string(kv.key()));
        marks += marks.empty() ? "?" : ", ?";
      }
      Stmt ins;
      std::string isql = "INSERT INTO \"goober_rebuild\" (" + cols + ") VALUES (" + marks + ")";
      if (sqlite3_prepare_v2(raw, isql.c_str(), -1, &ins.s, nullptr) != SQLITE_OK) {
        why = sqlite3_errmsg(raw);
      } else {
        int i = 1;
        for (const auto& kv : *out) {
          if (!why.empty()) break;
          bind_value(ins.s, i++, kv.value(), &why);
        }
        if (why.empty() && sqlite3_step(ins.s) != SQLITE_DONE) why = sqlite3_errmsg(raw);
      }
    }
    if (why.empty()) continue;
    ++failed;
    std::string k = text_of(row.at(key));
    if (first.empty()) first = "row " + k + ": " + why;
    if (ctx.report && ctx.report->failures.size() < 20) {
      ctx.report->failures.push_back(bj::object{{"table", t}, {"key", k}, {"error", why}});
    }
    if (!ctx.keep_going) break;
  }
  if (failed) {
    throw std::runtime_error(std::to_string(failed) + (failed == 1 ? " row" : " rows") + " of " + t +
                             " can't become " + schema + "'s shape, so nothing changed; the first: " + first);
  }
  db.exec("DROP TABLE " + ident(t));
  db.exec("ALTER TABLE \"goober_rebuild\" RENAME TO " + ident(t));
  for (const char* q : {"DELETE FROM goober_columns WHERE table_name = ?", "DELETE FROM goober_indexes WHERE table_name = ?"}) {
    sqlite::Stmt d(db, q);
    d.bind(1, t).run();
  }
  for (const auto& c : to_t.columns) record_column(db, t, c, schema);
  for (const auto& idx : to_t.indexes) create_index(db, t, idx, schema);
  if (const auto* stamped = Schema::stamped_key(to_t)) db.exec(id_triggers(to_t, *stamped));
  for (const auto& c : from_t.columns) {
    if (!to_t.column(c.name)) record_dropped(db, t, c.name, schema);
  }
}

void apply_in(sqlite::Db& db, const Schema& schema, ApplyContext& ctx);

// The step from `from` to `to`, on a database that holds `from` (041 § 4): expansions, then
// emulated changes and contractions, then indexing; forced tables first, through their handlers.
void apply_step(sqlite::Db& db, const Schema& from, const Schema& to, ApplyContext& ctx, bool existed) {
  Step step = plan_step(from, to);
  if (step.refused()) throw std::runtime_error("schema " + to.name + " changes a primary key, which never changes");
  if (ctx.require_approval && step.needs_person() && (!ctx.store || !ctx.store->approval(to.name))) {
    throw std::runtime_error("schema " + to.name + " drops or rewrites data, so it waits for a person to approve it in "
                             "the panel's Database view (041 § 6)");
  }
  // A table or column another schema also declares, outside this line of versions, stays as it is.
  if (ctx.store) {
    std::set<std::string> line{to.name};
    for (std::optional<Schema> s = from; s; s = s->follows.empty() ? std::nullopt : ctx.store->get(s->follows)) {
      if (!line.insert(s->name).second) break;
    }
    std::vector<std::string> applied;
    {
      sqlite::Stmt q(db, "SELECT name FROM goober_schemas");
      while (q.step()) applied.push_back(q.text(0));
    }
    for (const auto& name : applied) {
      if (line.count(name)) continue;
      auto other = ctx.store->get(name);
      if (!other) continue;
      for (const auto& c : step.changes) {
        if (c.kind != "contraction" && c.kind != "emulated" && c.kind != "forced") continue;
        const Table* ot = other->table(c.table);
        if (!ot || (!c.column.empty() && !ot->column(c.column))) continue;
        throw std::runtime_error(c.table + (c.column.empty() ? "" : "." + c.column) + " is also declared by schema " + name +
                                 ", which isn't a version of " + to.name + ", so " + to.name + " can't change it here");
      }
    }
  }
  std::set<std::string> skip;
  for (const auto& t : step.forced_tables) {
    bool handled = ctx.rows && ctx.rows->count(t);
    if (handled || !table_exists(db, t)) continue;
    int64_t rows = count_rows(db, t);
    if (rows == 0) continue;
    if (ctx.dry_run && ctx.pending) {
      ctx.pending->push_back(t);
      skip.insert(t);
      continue;
    }
    throw std::runtime_error("table " + t + " holds " + std::to_string(rows) + " rows that only an upgrade box can carry "
                             "over to " + to.name + ", so this database waits for its upgrade (upgrade_dry_run, then "
                             "upgrade_start; 041 § 7)");
  }
  bool rewrites = step.contracts() || !step.forced_tables.empty();
  for (const auto& c : step.changes) rewrites |= c.kind == "emulated";
  if (existed && rewrites && ctx.before_change) {
    std::string err;
    if (!ctx.before_change("before schema " + to.name, &err)) {
      throw std::runtime_error("the snapshot before schema " + to.name + " failed, so nothing changed: " + err);
    }
  }
  // Forced tables, through their handlers.
  for (const auto& t : step.forced_tables) {
    if (skip.count(t)) continue;
    if (!table_exists(db, t)) {
      create_table(db, *to.table(t), t);
      for (const auto& c : to.table(t)->columns) record_column(db, t, c, to.name);
      for (const auto& idx : to.table(t)->indexes) create_index(db, t, idx, to.name);
      if (const auto* k = Schema::stamped_key(*to.table(t))) db.exec(id_triggers(*to.table(t), *k));
      continue;
    }
    rebuild_forced(db, *from.table(t), *to.table(t), to.name, ctx);
  }
  // Indexes that go or change: dropped before any is made, so a name can move between tables.
  for (const auto& c : step.changes) {
    if (c.index.empty() || c.kind != "indexing" || step.forced_tables.count(c.table)) continue;
    const Table* ft = from.table(c.table);
    if (!ft) continue;
    for (const auto& i : ft->indexes) {
      if (i.name == c.index) drop_index(db, i.name);
    }
  }
  // New tables.
  for (const auto& tt : to.tables) {
    if (from.table(tt.name)) continue;
    if (auto by = dropped_by(db, tt.name, "")) {
      throw std::runtime_error("table " + tt.name + " was dropped by schema " + *by + "; a dropped name is never used again");
    }
    if (table_exists(db, tt.name)) throw std::runtime_error("table " + tt.name + " already exists here, made by another schema");
    create_table(db, tt, tt.name);
    if (const auto* key = Schema::stamped_key(tt)) db.exec(id_triggers(tt, *key));
    for (const auto& c : tt.columns) record_column(db, tt.name, c, to.name);
    for (const auto& idx : tt.indexes) create_index(db, tt.name, idx, to.name);
  }
  // Tables both declare.
  for (const auto& tt : to.tables) {
    const Table* ft = from.table(tt.name);
    if (!ft || step.forced_tables.count(tt.name)) continue;
    auto present = columns_present(db, tt.name);
    for (const auto& tc : tt.columns) {
      if (ft->column(tc.name)) continue;
      if (auto by = dropped_by(db, tt.name, tc.name)) {
        throw std::runtime_error(tt.name + "." + tc.name + " was dropped by schema " + *by + "; a dropped name is never used again");
      }
      if (present.count(tc.name)) throw std::runtime_error(tt.name + "." + tc.name + " exists but no schema declared it");
      db.exec("ALTER TABLE " + ident(tt.name) + " ADD COLUMN " + tc.sql());
      record_column(db, tt.name, tc, to.name);
    }
    std::map<std::string, std::string> defs;
    std::vector<const Column*> dropped;
    for (const auto& c : step.changes) {
      if (c.table != tt.name || c.column.empty()) continue;
      if (c.kind == "emulated") defs[c.column] = tt.column(c.column)->sql();
      if (c.kind == "contraction") {
        const Column* fc = ft->column(c.column);
        if (!present.count(fc->name)) continue;
        dropped.push_back(fc);
        if (!fc->default_value && !fc->nullable) {
          // Newer boxes' inserts don't name it, so it needs a default: what older boxes read.
          Column held = *fc;
          if (fc->type == "string") held.default_value = bj::value("");
          else if (fc->type == "boolean") held.default_value = bj::value(false);
          else if (fc->type == "double" || fc->type == "float") held.default_value = bj::value(0.0);
          else held.default_value = bj::value(int64_t(0));
          defs[fc->name] = held.sql();
        }
      }
    }
    if (!defs.empty()) rebuild_table(db, tt.name, defs);
    for (const auto* fc : dropped) {
      tombstone_column(db, tt.name, *fc);
      sqlite::Stmt d(db, "DELETE FROM goober_columns WHERE table_name = ? AND column_name = ?");
      d.bind(1, tt.name).bind(2, fc->name).run();
      record_dropped(db, tt.name, fc->name, to.name);
    }
    for (const auto& [name, def] : defs) {
      if (const Column* tc = tt.column(name)) record_column(db, tt.name, *tc, to.name);
    }
  }
  // Dropped tables.
  for (const auto& ft : from.tables) {
    if (!to.table(ft.name)) tombstone_table(db, ft.name, to.name);
  }
  // New and changed indexes on tables both declare.
  for (const auto& c : step.changes) {
    if (c.index.empty() || c.kind != "indexing" || !from.table(c.table) || step.forced_tables.count(c.table)) continue;
    const Table* tt = to.table(c.table);
    if (!tt) continue;
    for (const auto& i : tt->indexes) {
      if (i.name == c.index) create_index(db, tt->name, i, to.name);
    }
  }
}

// A schema with no `follows`: expand only, as v1 always did (008 § 3).
void expand(sqlite::Db& db, const Schema& schema) {
  for (const auto& table : schema.tables) {
    if (auto by = dropped_by(db, table.name, "")) {
      throw std::runtime_error("table " + table.name + " was dropped by schema " + *by + "; a dropped name is never used again");
    }
    bool exists = table_exists(db, table.name);
    std::map<std::string, std::string> recorded;
    {
      sqlite::Stmt q(db, "SELECT column_name, signature FROM goober_columns WHERE table_name = ?");
      q.bind(1, table.name);
      while (q.step()) recorded[q.text(0)] = q.text(1);
    }
    if (!exists) {
      create_table(db, table, table.name);
      if (const auto* key = Schema::stamped_key(table)) db.exec(id_triggers(table, *key));
    }
    auto present = columns_present(db, table.name);
    for (const auto& col : table.columns) {
      auto rec = recorded.find(col.name);
      if (rec != recorded.end()) {
        if (rec->second != col.signature()) {
          throw std::runtime_error("schema " + schema.name + " changes " + table.name + "." + col.name + " from " +
                                   rec->second + " to " + col.signature() +
                                   "; a column changes only in a schema that follows the one before it (\"follows\", 041)");
        }
      } else if (auto by = dropped_by(db, table.name, col.name)) {
        throw std::runtime_error(table.name + "." + col.name + " was dropped by schema " + *by + "; a dropped name is never used again");
      } else if (exists && present.count(col.name)) {
        throw std::runtime_error(table.name + "." + col.name + " exists but no schema declared it");
      } else if (exists) {
        if (col.primary_key) throw std::runtime_error("schema " + schema.name + " can't add a primary key to " + table.name);
        if (!col.nullable && !col.default_value) {
          throw std::runtime_error("schema " + schema.name + " adds " + table.name + "." + col.name +
                                   ", which existing rows can't fill: make it nullable or give it a default");
        }
        db.exec("ALTER TABLE " + ident(table.name) + " ADD COLUMN " + col.sql());
      }
      if (rec == recorded.end()) record_column(db, table.name, col, schema.name);
    }
    for (const auto& idx : table.indexes) {
      sqlite::Stmt q(db, "SELECT table_name, signature FROM goober_indexes WHERE index_name = ?");
      q.bind(1, idx.name);
      if (q.step()) {
        if (q.text(0) != table.name || q.text(1) != idx.signature()) {
          throw std::runtime_error("schema " + schema.name + " changes index " + idx.name);
        }
        continue;
      }
      create_index(db, table.name, idx, schema.name);
    }
  }
}

void apply_in(sqlite::Db& db, const Schema& schema, ApplyContext& ctx) {
  if (is_applied(db, schema.name)) return;
  if (!schema.follows.empty()) {
    if (!ctx.store) throw std::runtime_error("schema " + schema.name + " follows " + schema.follows + ", which can't be looked up here");
    auto from = ctx.store->get(schema.follows);
    if (!from) throw std::runtime_error("schema " + schema.name + " follows " + schema.follows + ", which isn't stored");
    // Every database goes through the same steps in order: the one before first.
    bool existed = is_applied(db, from->name);
    if (!existed) apply_in(db, *from, ctx);
    apply_step(db, *from, schema, ctx, existed);
  } else {
    expand(db, schema);
  }
  sqlite::Stmt mark(db, "INSERT INTO goober_schemas (name, applied_ms) VALUES (?, ?)");
  mark.bind(1, schema.name).bind(2, base::now_unix_ms()).run();
}

// Plans (and, unless dry_run, applies) a schema to an open database, in one transaction.
bool apply_schema(sqlite::Db& db, const Schema& schema, ApplyContext ctx, std::string* error) {
  db.exec(kHostTables);
  if (is_applied(db, schema.name)) return true;
  db.exec("BEGIN IMMEDIATE");
  try {
    apply_in(db, schema, ctx);
  } catch (const std::exception& e) {
    db.exec("ROLLBACK");
    *error = e.what();
    return false;
  }
  db.exec(ctx.dry_run ? "ROLLBACK" : "COMMIT");
  return true;
}

}  // namespace

std::filesystem::path Databases::file(std::string_view tenant, std::string_view database) const {
  return root_ / "sqlite3" / std::string(tenant) / "databases" / (std::string(database) + ".db");
}

bool Databases::exists(std::string_view tenant, std::string_view database) const {
  return base::is_valid_name(tenant) && is_valid_sql_name(database) && std::filesystem::exists(file(tenant, database));
}

std::unique_ptr<sqlite::Db> Databases::open(std::string_view tenant, std::string_view database, const Schema* schema,
                                            std::string* error) {
  if (!base::is_valid_name(tenant)) return *error = "bad tenant", nullptr;
  if (!is_valid_sql_name(database)) return *error = "bad database name: " + std::string(database), nullptr;
  auto path = file(tenant, database);
  std::filesystem::create_directories(path.parent_path());
  install_clockless_vfs();
  auto db = std::make_unique<sqlite::Db>(path);
  install_id_functions(db->raw(), &ids_);
  if (schema) {
    std::string key = path.string() + "|" + schema->name;
    std::lock_guard lock(mu_);
    if (!applied_.count(key)) {
      ApplyContext ctx;
      ctx.store = store_;
      if (before_change_) {
        ctx.before_change = [this, t = std::string(tenant), d = std::string(database)](const std::string& note, std::string* err) {
          return before_change_(t, d, note, err);
        };
      }
      if (!apply_schema(*db, *schema, ctx, error)) return nullptr;
      bump(path);
      applied_.insert(key);
    }
  }
  return db;
}

bool Databases::ensure_schema(std::string_view tenant, std::string_view database, const Schema& schema,
                              std::string* error) {
  if (!base::is_valid_name(tenant)) return *error = "bad tenant", false;
  if (!is_valid_sql_name(database)) return *error = "bad database name: " + std::string(database), false;
  {
    std::lock_guard lock(mu_);
    if (applied_.count(file(tenant, database).string() + "|" + schema.name)) return true;
  }
  return open(tenant, database, &schema, error) != nullptr;
}

bool Databases::check(std::string_view tenant, std::string_view database, const Schema& schema,
                      std::string* error) const {
  if (!exists(tenant, database)) return true;
  sqlite::Db db(file(tenant, database));
  install_id_functions(db.raw(), &ids_);
  std::string why;
  // A forced table still holding rows waits for its upgrade (041 § 7); the box may be stored.
  std::vector<std::string> pending;
  ApplyContext ctx;
  ctx.dry_run = true;
  ctx.store = store_;
  ctx.pending = &pending;
  if (apply_schema(db, schema, ctx, &why)) return true;
  *error = "tenant " + std::string(tenant) + ", database " + std::string(database) + ": " + why;
  return false;
}

bool Databases::compatible(const std::vector<const Schema*>& schemas, std::string* error, const SchemaStore* store) {
  sqlite::Db db(":memory:");
  install_id_functions(db.raw(), nullptr);
  ApplyContext ctx;
  ctx.store = store;
  ctx.require_approval = false;  // an empty database: nothing to drop
  for (const auto* schema : schemas) {
    std::string why;
    if (!apply_schema(db, *schema, ctx, &why)) {
      *error = "schema " + schema->name + " doesn't fit with the others on the same database: " + why;
      return false;
    }
  }
  return true;
}

std::vector<std::string> Databases::applied_schemas(std::string_view tenant, std::string_view database) const {
  std::vector<std::string> out;
  if (!exists(tenant, database)) return out;
  sqlite::Db db(file(tenant, database));
  db.exec(kHostTables);
  sqlite::Stmt q(db, "SELECT name FROM goober_schemas ORDER BY applied_ms, name");
  while (q.step()) out.push_back(q.text(0));
  return out;
}

bj::object Databases::step_impact(std::string_view tenant, std::string_view database, const Step& step,
                                  const Schema& from) const {
  bj::object out{{"tenant", std::string(tenant)}, {"database", std::string(database)}};
  sqlite::Db db(file(tenant, database));
  db.exec(kHostTables);
  out["applied"] = is_applied(db, step.to);
  bj::array drops, forced;
  for (const auto& c : step.changes) {
    if (c.kind != "contraction" || !table_exists(db, c.table)) continue;
    if (c.column.empty()) {
      drops.push_back(bj::object{{"table", c.table}, {"rows", count_rows(db, c.table)}});
      continue;
    }
    const Table* ft = from.table(c.table);
    const Column* fc = ft ? ft->column(c.column) : nullptr;
    if (!fc || !columns_present(db, c.table).count(c.column)) continue;
    // Rows whose value differs from what older boxes will read: the data that goes.
    drops.push_back(bj::object{{"table", c.table},
                               {"column", c.column},
                               {"rows", count_rows(db, c.table, ident(c.column) + " IS NOT " + fc->dropped_value_sql())}});
  }
  for (const auto& t : step.forced_tables) {
    if (table_exists(db, t)) forced.push_back(bj::object{{"table", t}, {"rows", count_rows(db, t)}});
  }
  out["drops"] = std::move(drops);
  out["forced"] = std::move(forced);
  return out;
}

bool Databases::upgrade(std::string_view tenant, std::string_view database, const Schema& to,
                        const std::map<std::string, RowTransform>& rows, bool keep_going, UpgradeReport* report,
                        std::string* error, bool require_approval) {
  if (!exists(tenant, database)) return *error = "tenant " + std::string(tenant) + " has no database " + std::string(database), false;
  auto path = file(tenant, database);
  install_clockless_vfs();
  sqlite::Db db(path);
  install_id_functions(db.raw(), &ids_);
  ApplyContext ctx;
  ctx.store = store_;
  ctx.rows = &rows;
  ctx.require_approval = require_approval;
  ctx.keep_going = keep_going;
  ctx.report = report;
  if (before_change_) {
    ctx.before_change = [this, t = std::string(tenant), d = std::string(database)](const std::string& note, std::string* err) {
      return before_change_(t, d, note, err);
    };
  }
  std::lock_guard lock(mu_);
  if (!apply_schema(db, to, ctx, error)) return false;
  bump(path);
  applied_.insert(path.string() + "|" + to.name);
  return true;
}

std::vector<std::string> Databases::tenants() const {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(root_ / "sqlite3", ec)) {
    std::string name = e.path().filename().string();
    if (e.is_directory() && base::is_valid_name(name) && std::filesystem::exists(e.path() / "databases")) {
      out.push_back(name);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> Databases::databases(std::string_view tenant) const {
  std::vector<std::string> out;
  if (!base::is_valid_name(tenant)) return out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(root_ / "sqlite3" / std::string(tenant) / "databases", ec)) {
    std::string name = e.path().filename().string();
    if (name.ends_with(".db") && is_valid_sql_name(name.substr(0, name.size() - 3))) out.push_back(name.substr(0, name.size() - 3));
  }
  std::sort(out.begin(), out.end());
  return out;
}

Databases::~Databases() {
  for (auto& [f, w] : watchers_) sqlite3_close(w.db);
}

namespace {
int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

int64_t Databases::version(const std::filesystem::path& f) const {
  std::lock_guard lock(versions_mu_);
  auto [it, fresh] = versions_.try_emplace(f.string(), 0);
  if (fresh) it->second = now_us();
  return it->second;
}

void Databases::bump(const std::filesystem::path& f) const {
  int64_t now = now_us();
  std::lock_guard lock(versions_mu_);
  auto& v = versions_[f.string()];
  v = std::max(v + 1, now);
}

std::optional<int64_t> Databases::data_version(const std::filesystem::path& f) const {
  std::lock_guard lock(versions_mu_);
  std::string key = f.string();
  auto it = watchers_.find(key);
  if (it == watchers_.end()) {
    std::error_code ec;
    if (!std::filesystem::exists(f, ec)) return std::nullopt;
    Watcher w;
    if (sqlite3_open_v2(key.c_str(), &w.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      sqlite3_close(w.db);
      return std::nullopt;
    }
    if (watchers_.size() >= 512) {
      auto oldest = std::min_element(watchers_.begin(), watchers_.end(),
                                     [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
      sqlite3_close(oldest->second.db);
      watchers_.erase(oldest);
    }
    it = watchers_.emplace(key, w).first;
  }
  it->second.used = ++watcher_clock_;
  sqlite3_stmt* s = nullptr;
  std::optional<int64_t> out;
  if (sqlite3_prepare_v2(it->second.db, "PRAGMA data_version", -1, &s, nullptr) == SQLITE_OK &&
      sqlite3_step(s) == SQLITE_ROW) {
    out = sqlite3_column_int64(s, 0);
  }
  sqlite3_finalize(s);
  if (!out) {
    sqlite3_close(it->second.db);
    watchers_.erase(it);
  }
  return out;
}

void Databases::forget(std::string_view tenant, std::string_view database) {
  {
    // Dropped or changed underneath: every cached answer that read it is stale, and its watcher
    // must not hold the old file open.
    auto f = file(tenant, database);
    {
      std::lock_guard lock(versions_mu_);
      if (auto it = watchers_.find(f.string()); it != watchers_.end()) {
        sqlite3_close(it->second.db);
        watchers_.erase(it);
      }
    }
    bump(f);
  }
  std::string prefix = file(tenant, database).string() + "|";
  std::lock_guard lock(mu_);
  for (auto it = applied_.begin(); it != applied_.end();) {
    it = it->starts_with(prefix) ? applied_.erase(it) : std::next(it);
  }
}

bool Databases::drop_database(std::string_view tenant, std::string_view database, std::string* error) {
  if (!exists(tenant, database)) return *error = "tenant " + std::string(tenant) + " has no database " + std::string(database), false;
  auto path = file(tenant, database);
  std::error_code ec;
  for (const char* extra : {"-wal", "-shm", "-journal"}) std::filesystem::remove(path.string() + extra, ec);
  if (!std::filesystem::remove(path, ec) || ec) return *error = "removing " + path.string() + ": " + ec.message(), false;
  forget(tenant, database);
  return true;
}

bool Databases::drop_table(std::string_view tenant, std::string_view database, std::string_view table,
                           std::string* error) {
  if (!exists(tenant, database)) return *error = "tenant " + std::string(tenant) + " has no database " + std::string(database), false;
  if (!is_valid_sql_name(table)) return *error = "not a table name: " + std::string(table), false;
  sqlite::Db db(file(tenant, database));
  db.exec(kHostTables);
  {
    sqlite::Stmt q(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
    q.bind(1, table);
    if (!q.step()) return *error = "database " + std::string(database) + " has no table " + std::string(table), false;
  }
  db.exec("BEGIN IMMEDIATE");
  try {
    // The schemas that declared the table are no longer whole here: applying one again makes it.
    std::set<std::string> schemas;
    {
      sqlite::Stmt q(db, "SELECT DISTINCT schema FROM goober_columns WHERE table_name = ?");
      q.bind(1, table);
      while (q.step()) schemas.insert(q.text(0));
    }
    db.exec("DROP TABLE \"" + std::string(table) + "\"");
    // A person's drop frees a tombstoned table's name and its dropped columns' (041 § 5).
    for (const char* sql : {"DELETE FROM goober_columns WHERE table_name = ?", "DELETE FROM goober_indexes WHERE table_name = ?",
                            "DELETE FROM goober_dropped WHERE table_name = ?"}) {
      sqlite::Stmt d(db, sql);
      d.bind(1, table).run();
    }
    for (const auto& s : schemas) {
      sqlite::Stmt d(db, "DELETE FROM goober_schemas WHERE name = ?");
      d.bind(1, s).run();
    }
    db.exec("COMMIT");
  } catch (const std::exception& e) {
    db.exec("ROLLBACK");
    return *error = e.what(), false;
  }
  forget(tenant, database);
  return true;
}

bj::object Databases::describe(std::string_view tenant, std::string_view database) const {
  bj::object out{{"tenant", std::string(tenant)}, {"database", std::string(database)}};
  if (!exists(tenant, database)) {
    out["exists"] = false;
    return out;
  }
  auto path = file(tenant, database);
  sqlite::Db db(path);
  db.exec(kHostTables);
  out["exists"] = true;
  out["file"] = path.string();
  std::error_code ec;
  out["bytes"] = int64_t(std::filesystem::file_size(path, ec));
  bj::array schemas;
  {
    sqlite::Stmt q(db, "SELECT name, applied_ms FROM goober_schemas ORDER BY applied_ms");
    while (q.step()) schemas.push_back(bj::object{{"schema", q.text(0)}, {"applied", base::format_utc_ms(q.int64(1))}});
  }
  out["schemas"] = std::move(schemas);
  bj::array tables;
  std::vector<std::string> names;
  {
    sqlite::Stmt q(db, "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'goober\\_%' ESCAPE '\\' "
                       "AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY name");
    while (q.step()) names.push_back(q.text(0));
  }
  for (const auto& t : names) {
    bj::array columns;
    sqlite::Stmt c(db, "SELECT c.name, c.type, c.\"notnull\", c.pk, g.signature, g.schema FROM pragma_table_info(?) c "
                       "LEFT JOIN goober_columns g ON g.table_name = ? AND g.column_name = c.name ORDER BY c.cid");
    c.bind(1, t).bind(2, t);
    while (c.step()) {
      columns.push_back(bj::object{{"name", c.text(0)},
                                   {"sql_type", c.text(1)},
                                   {"nullable", c.int64(2) == 0 && c.int64(3) == 0},
                                   {"primary_key", c.int64(3) != 0},
                                   {"declared", c.is_null(4) ? "" : c.text(4)},
                                   {"schema", c.is_null(5) ? "" : c.text(5)}});
    }
    bj::array indexes;
    sqlite::Stmt i(db, "SELECT index_name, signature FROM goober_indexes WHERE table_name = ? ORDER BY index_name");
    i.bind(1, t);
    while (i.step()) indexes.push_back(bj::object{{"name", i.text(0)}, {"definition", i.text(1)}});
    sqlite::Stmt n(db, "SELECT count(*) FROM \"" + t + "\"");
    int64_t rows = n.step() ? n.int64(0) : 0;
    tables.push_back(bj::object{{"table", t}, {"rows", rows}, {"columns", std::move(columns)}, {"indexes", std::move(indexes)}});
  }
  out["tables"] = std::move(tables);
  // Tombstones (041 § 5): tables and columns a schema dropped, kept for older boxes' SQL.
  bj::array dropped;
  {
    sqlite::Stmt q(db, "SELECT table_name, column_name, schema, dropped_ms FROM goober_dropped ORDER BY table_name, column_name");
    while (q.step()) {
      bj::object d{{"table", q.text(0)}, {"schema", q.text(2)}, {"dropped", base::format_utc_ms(q.int64(3))}};
      if (!q.text(1).empty()) d["column"] = q.text(1);
      dropped.push_back(std::move(d));
    }
  }
  out["dropped"] = std::move(dropped);
  return out;
}

std::optional<bj::object> Databases::operate(std::string_view tenant, std::string_view database, std::string_view sql,
                                             const bj::value& params, int64_t row_limit, bool write,
                                             std::string* error) const {
  if (!exists(tenant, database)) {
    return *error = "tenant " + std::string(tenant) + " has no database " + std::string(database), std::nullopt;
  }
  sqlite::Db db(file(tenant, database));
  install_id_functions(db.raw(), &ids_);
  Auth auth;
  auth.write = write;
  // An operator's statement gets far more room than a box's, but not for ever: a recursive CTE
  // that never ends would hold a worker (and, from the panel, an observer could start one).
  constexpr int64_t kOperatorSteps = 500'000'000;
  auto result = run_statement(db.raw(), sql, params, auth, row_limit, kOperatorSteps, [&](bool readonly) {
    if (write && readonly) return std::string("db_exec changes rows; use db_query to read");
    return std::string();
  });
  if (auto* e = result.if_contains("error")) return *error = std::string(e->get_string()), std::nullopt;
  if (write && sqlite3_total_changes64(db.raw()) > 0) bump(file(tenant, database));  // 037 § 3
  return result;
}

Session::~Session() { rollback(); }

Session::Open* Session::open_database(const std::string& tenant, const std::string& database, std::string* error) {
  std::string key = tenant + "/" + database;
  auto& open = open_[key];
  if (open.db) return &open;
  // On first use, every schema that any strategy brings to this database is applied,
  // before the request's transaction begins.
  std::string err;
  try {
    for (const auto& other : strategies_) {
      if (!other.applies_schema() || other.tenant_for(tenant_) != tenant || other.database_for(tenant_) != database) continue;
      auto schema = schemas_.get(other.schema);
      if (!schema) {
        err = "schema " + other.schema + " isn't stored on this host";
        break;
      }
      // Applied once per process: a request after the first opens nothing for it.
      if (!dbs_.ensure_schema(tenant, database, *schema, &err)) break;
    }
    if (err.empty()) open.db = dbs_.open(tenant, database, nullptr, &err);
  } catch (const std::exception& e) {
    err = e.what();
  }
  if (!open.db) {
    open_.erase(key);
    *error = err;
    return nullptr;
  }
  return &open;
}

bool Session::host_exec(const manifest::DataStrategy& strategy, std::string_view sql, const bj::value& params,
                        std::string* error) {
  if (!broken_.empty()) return *error = broken_, false;
  Open* open = open_database(strategy.tenant_for(tenant_), strategy.database_for(tenant_), error);
  if (!open) return false;
  sqlite3* raw = open->db->raw();
  if (!open->in_transaction) {
    if (sqlite3_exec(raw, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
      *error = sqlite3_errmsg(raw);
      return false;
    }
    open->in_transaction = true;
  }
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(raw, sql.data(), int(sql.size()), &st, nullptr) != SQLITE_OK) {
    *error = sqlite3_errmsg(raw);
    return false;
  }
  bool ok = true;
  if (params.is_array()) {
    for (size_t i = 0; i < params.as_array().size() && ok; ++i) ok = bind_value(st, int(i) + 1, params.as_array()[i], error);
  }
  int rc = ok ? sqlite3_step(st) : SQLITE_ERROR;
  if (ok && rc != SQLITE_DONE && rc != SQLITE_ROW) {
    *error = sqlite3_errmsg(raw);
    ok = false;
  }
  sqlite3_finalize(st);
  ++statements_;
  return ok;
}

std::string Session::run(std::string_view strategy, std::string_view sql, std::string_view params_text) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  ++statements_;
  const manifest::DataStrategy* st = nullptr;
  for (const auto& s : strategies_) {
    if (s.name == strategy) st = &s;
  }
  if (!st) return fail("no strategy named " + std::string(strategy) + " in the manifest's data.sqlite3");
  if (!st->runnable()) return fail("strategy " + st->name + " uses pattern " + st->pattern + ", which v1 doesn't run");
  bj::value params = nullptr;
  if (!params_text.empty()) {
    std::string err;
    auto p = json::parse(params_text, &err);
    if (!p) return fail("params must be JSON: " + err);
    params = std::move(*p);
  }
  std::string database = st->database_for(tenant_);
  std::string open_err;
  Open* opened = open_database(st->tenant_for(tenant_), database, &open_err);
  if (!opened) return fail("opening database " + database + ": " + open_err);
  auto& open = *opened;
  Auth auth;
  auth.strategy = st;
  sqlite3* raw = open.db->raw();
  if (!broken_.empty()) return fail(broken_);
  // A strategy that may write takes the write lock with its first statement, read or not: a
  // read transaction that later writes fails at once (SQLITE_BUSY_SNAPSHOT) if anyone else
  // committed in between, and no busy timeout helps.
  // A read-only session (a GET, a HEAD, or a subscription's evaluation) never writes, so it
  // never takes the lock: its reads begin deferred and run beside a writer.
  bool immediate = !read_only_ && (st->may_write() || expect_writes_.count(st->name));
  bool wrote = false;
  auto result = run_statement(raw, sql, params, auth, st->row_limit, st->step_budget, [&](bool readonly) {
    if (!readonly && read_only_) {
      ++timing_.refused_writes;
      return std::string("a read changes nothing (a GET, a HEAD, or a subscription): this statement writes");
    }
    wrote = !readonly;
    if (open.in_transaction) return std::string();
    char* msg = nullptr;
    // Waiting for the write lock is the time BEGIN IMMEDIATE takes (037 § 7.1).
    struct Lock {
      Timing& t;
      std::chrono::steady_clock::time_point at;
      ~Lock() { t.lock_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - at).count(); }
    } lock{timing_, std::chrono::steady_clock::now()};
    if (sqlite3_exec(raw, readonly && !immediate ? "BEGIN" : "BEGIN IMMEDIATE", nullptr, nullptr, &msg) != SQLITE_OK) {
      std::string why = msg ? msg : "begin failed";
      sqlite3_free(msg);
      return "database " + database + " is busy: " + why;
    }
    open.in_transaction = true;
    return std::string();
  });
  // SQLite rolls a transaction back by itself after some failures (an interrupted write, a
  // full disk). Everything after that would commit statement by statement, so the request
  // is spoiled: nothing more runs, and its commit fails.
  if (open.in_transaction && sqlite3_get_autocommit(raw)) {
    open.in_transaction = false;
    broken_ = "the request's transaction on " + database + " was rolled back, so nothing more can be written";
    return fail(broken_);
  }
  if (wrote && !result.contains("error")) {
    wrote_.insert(st->name);
    written_.insert(st->tenant_for(tenant_) + "/" + database);
  }
  read_.insert(st->tenant_for(tenant_) + "/" + database);
  if (auth.cost.ran) {
    timing_.statement_us += auth.cost.us;
    statement_records_.push_back(StatementRecord{st->name, st->tenant_for(tenant_), database, std::string(sql), auth.cost.us,
                                                 auth.cost.rows, auth.cost.vm_steps, auth.cost.fullscan_steps,
                                                 auth.cost.sorts, auth.cost.autoindexes});
  }
  if (!result.contains("error")) {
    auto& used = used_[st->name];
    for (const auto& [table, use] : auth.uses) used[table].merge(use);
  }
  return json::dump(result);
}

void Session::expect_writes(const manifest::DataStrategy& strategy) { expect_writes_.insert(strategy.name); }

void Session::commit_last(const manifest::DataStrategy& strategy) {
  commit_last_ = strategy.tenant_for(tenant_) + "/" + strategy.database_for(tenant_);
}

bool Session::commit(std::string* error) {
  if (!broken_.empty()) {
    *error = broken_;
    rollback();
    return false;
  }
  // Each database commits on its own, so a request writing several isn't atomic across them:
  // a failed commit rolls back the ones not yet committed, not the ones before it. The
  // commit_last database goes last, so it lands only if all the others did.
  std::vector<std::pair<const std::string*, Open*>> order;
  for (auto& [name, open] : open_) {
    if (name != commit_last_) order.emplace_back(&name, &open);
  }
  if (auto it = open_.find(commit_last_); it != open_.end()) order.emplace_back(&it->first, &it->second);
  bool ok = true;
  struct Committing {
    Timing& t;
    std::chrono::steady_clock::time_point at;
    ~Committing() { t.commit_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - at).count(); }
  } committing{timing_, std::chrono::steady_clock::now()};
  for (auto [name, open] : order) {
    if (!open->in_transaction) continue;
    char* msg = nullptr;
    if (ok && sqlite3_exec(open->db->raw(), "COMMIT", nullptr, nullptr, &msg) == SQLITE_OK) {
      open->in_transaction = false;
      // A connection opened for this request changed something: its database's version moves (037 § 3).
      if (sqlite3_total_changes64(open->db->raw()) > 0) {
        auto slash = name->find('/');
        dbs_.bump(dbs_.file(name->substr(0, slash), name->substr(slash + 1)));
      }
      continue;
    }
    if (ok) *error = "committing database " + *name + ": " + (msg ? msg : "failed");
    sqlite3_free(msg);
    ok = false;
  }
  if (!ok) rollback();
  return ok;
}

void Session::rollback() {
  for (auto& [name, open] : open_) {
    if (!open.in_transaction) continue;
    sqlite3_exec(open.db->raw(), "ROLLBACK", nullptr, nullptr, nullptr);
    open.in_transaction = false;
  }
}

}  // namespace goober::db
