#include "goober/db/database.h"

#include <chrono>
#include <sqlite3.h>

#include <algorithm>
#include <functional>
#include <limits>

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

struct Auth {
  const manifest::DataStrategy* strategy = nullptr;  // null for an operator
  bool write = false;                                // an operator's db_exec
  std::set<std::string> tables;                      // the database's real tables; anything else read is a CTE
  std::string denied;
};

std::set<std::string> real_tables(sqlite3* db) {
  std::set<std::string> out;
  sqlite3_stmt* q = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type IN ('table', 'view')", -1, &q, nullptr) == SQLITE_OK) {
    while (sqlite3_step(q) == SQLITE_ROW) out.insert(reinterpret_cast<const char*>(sqlite3_column_text(q, 0)));
  }
  sqlite3_finalize(q);
  return out;
}

int authorize(void* p, int code, const char* a, const char* b, const char*, const char*) {
  auto& auth = *static_cast<Auth*>(p);
  std::string t = a ? a : "", c = b ? b : "";
  auto deny = [&](std::string why) {
    if (auth.denied.empty()) auth.denied = std::move(why);
    return SQLITE_DENY;
  };
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
      // A common table expression or subquery: its own reads are checked one by one.
      if (!g && !auth.tables.count(t) && !host_table(t)) return SQLITE_OK;
      if (!g) return deny("strategy " + auth.strategy->name + " can't read table " + t);
      if (!g->read_all && !g->read.count(c)) return deny("strategy " + auth.strategy->name + " can't read " + t + "." + c);
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
      return SQLITE_OK;
    }
    case SQLITE_FUNCTION:
      if (!auth.strategy) return SQLITE_OK;
      if (kBoxFunctions.count(base::to_lower(c))) return SQLITE_OK;
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
  if (sql.size() > 100000) return fail("the statement is longer than 100000 bytes");
  if (auth.strategy && uses_replace_conflict(sql)) {
    return fail("REPLACE conflict resolution deletes rows behind the grants; use INSERT ... ON CONFLICT DO UPDATE");
  }
  auth.tables = real_tables(db);
  sqlite3_limit(db, SQLITE_LIMIT_LENGTH, kMaxValueBytes);
  sqlite3_limit(db, SQLITE_LIMIT_LIKE_PATTERN_LENGTH, kMaxLikePattern);
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
      result_bytes += size_t(sqlite3_column_bytes(st.s, i)) + 16;
      row.push_back(column_value(st.s, i));
    }
    if (result_bytes > kMaxResultBytes) {
      sqlite3_progress_handler(db, 0, nullptr, nullptr);
      return fail("the result is larger than " + std::to_string(kMaxResultBytes) + " bytes; select less");
    }
    rows.push_back(std::move(row));
  }
  sqlite3_progress_handler(db, 0, nullptr, nullptr);
  if (rc == SQLITE_INTERRUPT && progress.late) {
    return fail("the statement ran longer than " + std::to_string(kStatementMs) + " ms");
  }
  if (rc == SQLITE_INTERRUPT && progress.steps > progress.budget) {
    return fail("the statement used more than its step budget of " + std::to_string(step_budget));
  }
  if (rc != SQLITE_DONE) return fail(sqlite3_errmsg(db));
  return bj::object{{"columns", std::move(columns)},
                    {"rows", std::move(rows)},
                    {"changes", readonly ? 0 : sqlite3_changes(db)},
                    {"last_insert_id", readonly ? 0 : sqlite3_last_insert_rowid(db)}};
}

// Plans (and, unless dry_run, applies) a schema to an open database, expand only.
bool apply_schema(sqlite::Db& db, const Schema& schema, bool dry_run, std::string* error) {
  db.exec(kHostTables);
  {
    sqlite::Stmt q(db, "SELECT 1 FROM goober_schemas WHERE name = ?");
    q.bind(1, schema.name);
    if (q.step()) return true;
  }
  db.exec("BEGIN IMMEDIATE");
  try {
    for (const auto& table : schema.tables) {
      bool exists = false;
      {
        sqlite::Stmt q(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
        q.bind(1, table.name);
        exists = q.step();
      }
      std::map<std::string, std::string> recorded;
      {
        sqlite::Stmt q(db, "SELECT column_name, signature FROM goober_columns WHERE table_name = ?");
        q.bind(1, table.name);
        while (q.step()) recorded[q.text(0)] = q.text(1);
      }
      if (!exists) {
        std::string sql = "CREATE TABLE \"" + table.name + "\" (";
        for (size_t i = 0; i < table.columns.size(); ++i) sql += (i ? ", " : "") + table.columns[i].sql();
        db.exec(sql + ") STRICT");
      }
      std::set<std::string> present;
      {
        sqlite::Stmt q(db, "SELECT name FROM pragma_table_info(?)");
        q.bind(1, table.name);
        while (q.step()) present.insert(q.text(0));
      }
      for (const auto& col : table.columns) {
        auto rec = recorded.find(col.name);
        if (rec != recorded.end()) {
          if (rec->second != col.signature()) {
            throw std::runtime_error("schema " + schema.name + " changes " + table.name + "." + col.name + " from " +
                                     rec->second + " to " + col.signature() +
                                     "; a column only ever keeps its type, nullability, and length (420)");
          }
        } else if (exists && present.count(col.name)) {
          throw std::runtime_error(table.name + "." + col.name + " exists but no schema declared it");
        } else if (exists) {
          if (col.primary_key) throw std::runtime_error("schema " + schema.name + " can't add a primary key to " + table.name);
          if (!col.nullable && !col.default_value) {
            throw std::runtime_error("schema " + schema.name + " adds " + table.name + "." + col.name +
                                     ", which existing rows can't fill: make it nullable or give it a default");
          }
          db.exec("ALTER TABLE \"" + table.name + "\" ADD COLUMN " + col.sql());
        }
        if (rec == recorded.end()) {
          sqlite::Stmt ins(db, "INSERT INTO goober_columns (table_name, column_name, signature, schema) VALUES (?, ?, ?, ?)");
          ins.bind(1, table.name).bind(2, col.name).bind(3, col.signature()).bind(4, schema.name).run();
        }
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
        std::string sql = std::string(idx.unique ? "CREATE UNIQUE INDEX \"" : "CREATE INDEX \"") + idx.name + "\" ON \"" +
                          table.name + "\" (";
        for (size_t i = 0; i < idx.columns.size(); ++i) sql += (i ? ", \"" : "\"") + idx.columns[i] + "\"";
        db.exec(sql + ")");
        sqlite::Stmt ins(db, "INSERT INTO goober_indexes (index_name, table_name, signature, schema) VALUES (?, ?, ?, ?)");
        ins.bind(1, idx.name).bind(2, table.name).bind(3, idx.signature()).bind(4, schema.name).run();
      }
    }
    sqlite::Stmt mark(db, "INSERT INTO goober_schemas (name, applied_ms) VALUES (?, ?)");
    mark.bind(1, schema.name).bind(2, base::now_unix_ms()).run();
  } catch (const std::exception& e) {
    db.exec("ROLLBACK");
    *error = e.what();
    return false;
  }
  db.exec(dry_run ? "ROLLBACK" : "COMMIT");
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
  auto db = std::make_unique<sqlite::Db>(path);
  if (schema) {
    std::string key = path.string() + "|" + schema->name;
    std::lock_guard lock(mu_);
    if (!applied_.count(key)) {
      if (!apply_schema(*db, *schema, false, error)) return nullptr;
      applied_.insert(key);
    }
  }
  return db;
}

bool Databases::check(std::string_view tenant, std::string_view database, const Schema& schema,
                      std::string* error) const {
  if (!exists(tenant, database)) return true;
  sqlite::Db db(file(tenant, database));
  std::string why;
  if (apply_schema(db, schema, true, &why)) return true;
  *error = "tenant " + std::string(tenant) + ", database " + std::string(database) + ": " + why;
  return false;
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
  return out;
}

std::optional<bj::object> Databases::operate(std::string_view tenant, std::string_view database, std::string_view sql,
                                             const bj::value& params, int64_t row_limit, bool write,
                                             std::string* error) const {
  if (!exists(tenant, database)) {
    return *error = "tenant " + std::string(tenant) + " has no database " + std::string(database), std::nullopt;
  }
  sqlite::Db db(file(tenant, database));
  Auth auth;
  auth.write = write;
  auto result = run_statement(db.raw(), sql, params, auth, row_limit, int64_t(1) << 40, [&](bool readonly) {
    if (write && readonly) return std::string("db_exec changes rows; use db_query to read");
    return std::string();
  });
  if (auto* e = result.if_contains("error")) return *error = std::string(e->get_string()), std::nullopt;
  return result;
}

Session::~Session() { rollback(); }

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
  auto& open = open_[database];
  if (!open.db) {
    // On first use, every schema that any strategy brings to this database is applied,
    // before the request's transaction begins.
    std::string err;
    try {
      for (const auto& other : strategies_) {
        if (!other.runnable() || other.database_for(tenant_) != database) continue;
        auto schema = schemas_.get(other.schema);
        if (!schema) {
          err = "schema " + other.schema + " isn't stored on this host";
          break;
        }
        if (!dbs_.open(tenant_, database, &*schema, &err)) break;
      }
      if (err.empty()) open.db = dbs_.open(tenant_, database, nullptr, &err);
    } catch (const std::exception& e) {
      err = e.what();
    }
    if (!open.db) {
      open_.erase(database);
      return fail("opening database " + database + ": " + err);
    }
  }
  Auth auth;
  auth.strategy = st;
  sqlite3* raw = open.db->raw();
  auto result = run_statement(raw, sql, params, auth, st->row_limit, st->step_budget, [&](bool readonly) {
    if (open.in_transaction) return std::string();
    char* msg = nullptr;
    if (sqlite3_exec(raw, readonly ? "BEGIN" : "BEGIN IMMEDIATE", nullptr, nullptr, &msg) != SQLITE_OK) {
      std::string why = msg ? msg : "begin failed";
      sqlite3_free(msg);
      return "database " + database + " is busy: " + why;
    }
    open.in_transaction = true;
    return std::string();
  });
  return json::dump(result);
}

bool Session::commit(std::string* error) {
  bool ok = true;
  for (auto& [name, open] : open_) {
    if (!open.in_transaction) continue;
    char* msg = nullptr;
    if (ok && sqlite3_exec(open.db->raw(), "COMMIT", nullptr, nullptr, &msg) == SQLITE_OK) {
      open.in_transaction = false;
      continue;
    }
    if (ok) *error = "committing database " + name + ": " + (msg ? msg : "failed");
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
