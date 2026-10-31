#include "goober/db/schema.h"

#include <set>

#include "goober/base/files.h"
#include "goober/base/names.h"

namespace goober::db {
namespace {

const std::set<std::string> kTypes = {"id", "int32", "int64", "double", "float", "boolean", "string"};

std::string sql_type(const std::string& t) {
  if (t == "string") return "TEXT";
  if (t == "double" || t == "float") return "REAL";
  // INT, not INTEGER: an id primary key stays an ordinary column, not SQLite's rowid, so its
  // default (the host's next id) applies; and "INT" is how a read knows to send it as a string.
  if (t == "id") return "INT";
  return "INTEGER";
}

// A column that refers to a user or a row, by its name (038 § 5).
bool looks_like_reference(const std::string& name) {
  return name == "user_id" || name.ends_with("_id");
}

std::string quote_literal(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    out += c;
    if (c == '\'') out += '\'';
  }
  return out + "'";
}

bool fail(std::string* error, std::string why) {
  *error = std::move(why);
  return false;
}

bool parse_column(const std::string& table, const std::string& name, const bj::value& v, Column* c,
                  std::string* error) {
  std::string where = table + "." + name;
  if (!is_valid_sql_name(name)) return fail(error, where + ": a column name must match [a-z][a-z0-9_]*, not goober_ or sqlite_");
  if (!v.is_object()) return fail(error, where + " must be an object");
  const auto& o = v.get_object();
  c->name = name;
  for (const auto& f : o) {
    std::string k(f.key());
    if (k != "type" && k != "primary-key" && k != "nullable" && k != "default" && k != "max-length") {
      return fail(error, where + ": unknown field \"" + k + "\"");
    }
  }
  std::string err;
  auto type = json::get_string(o, "type", &err);
  if (!type || !kTypes.count(*type)) {
    return fail(error, where + ": type must be id, int32, int64, double, float, boolean, or string");
  }
  c->type = *type;
  c->primary_key = json::get_bool(o, "primary-key", &err).value_or(false);
  c->nullable = json::get_bool(o, "nullable", &err).value_or(false);
  c->max_length = json::get_int(o, "max-length", &err);
  if (!err.empty()) return fail(error, where + ": " + err);
  if (c->primary_key && ((c->type != "id" && c->type != "int64") || c->nullable)) {
    return fail(error, where + ": a primary key is a non-nullable id, which the host stamps (or, as tables were keyed "
                              "before, int64). A natural key, like an address and a day, is a unique index on its own "
                              "columns: \"indexes\": {\"t_by_addr_day\": {\"columns\": [\"addr\", \"day\"], \"unique\": true}}");
  }
  if (c->max_length && (c->type != "string" || *c->max_length < 1)) {
    return fail(error, where + ": max-length is a positive length for a string");
  }
  if (auto it = o.find("default"); it != o.end()) {
    const auto& d = it->value();
    bool ok = (c->type == "string" && d.is_string()) || (c->type == "boolean" && d.is_bool()) ||
              ((c->type == "int32" || c->type == "int64" || c->type == "id") && d.is_int64()) ||
              ((c->type == "double" || c->type == "float") && (d.is_double() || d.is_int64())) ||
              (c->nullable && d.is_null());
    if (!ok) return fail(error, where + ": the default must be a " + c->type);
    if (c->primary_key) return fail(error, where + ": a primary key has no default");
    c->default_value = d;
  }
  return true;
}

}  // namespace

bool is_valid_sql_name(std::string_view name) {
  return base::is_valid_name(name) && !name.starts_with("sqlite_");
}

std::string Column::sql() const {
  std::string s = "\"" + name + "\" " + sql_type(type);
  // The host stamps it: the next id from its service (038 § 3).
  if (primary_key && type == "id") return s + " PRIMARY KEY NOT NULL DEFAULT (goober_id())";
  if (primary_key) return s + " PRIMARY KEY";
  if (!nullable) s += " NOT NULL";
  if (default_value) {
    const auto& d = *default_value;
    if (d.is_string()) {
      s += " DEFAULT " + quote_literal(std::string(d.get_string()));
    } else if (d.is_bool()) {
      s += std::string(" DEFAULT ") + (d.get_bool() ? "1" : "0");
    } else if (d.is_null()) {
      s += " DEFAULT NULL";
    } else {
      s += " DEFAULT " + json::dump(d);
    }
  }
  if (type == "boolean") s += " CHECK (\"" + name + "\" IN (0, 1))";
  if (type == "int32") s += " CHECK (\"" + name + "\" BETWEEN -2147483648 AND 2147483647)";
  if (max_length) s += " CHECK (length(\"" + name + "\") <= " + std::to_string(*max_length) + ")";
  return s;
}

std::string Column::signature() const {
  return type + (primary_key ? " pk" : "") + (nullable ? " null" : "") +
         (max_length ? " max " + std::to_string(*max_length) : "");
}

std::string Index::signature() const {
  std::string s = unique ? "unique" : "index";
  for (const auto& c : columns) s += " " + c;
  return s;
}

const Column* Table::column(std::string_view n) const {
  for (const auto& c : columns) {
    if (c.name == n) return &c;
  }
  return nullptr;
}

const Column* Schema::stamped_key(const Table& t) {
  for (const auto& c : t.columns) {
    if (c.primary_key && c.type == "id") return &c;
  }
  return nullptr;
}

const Table* Schema::table(std::string_view n) const {
  for (const auto& t : tables) {
    if (t.name == n) return &t;
  }
  return nullptr;
}

std::optional<Schema> parse_schema(std::string_view text, std::string* error) {
  std::string err;
  auto v = json::parse(text, &err);
  if (!v) return *error = err, std::nullopt;
  if (!v->is_object()) return *error = "a schema is a JSON object", std::nullopt;
  const auto& root = v->get_object();
  for (const auto& f : root) {
    if (f.key() != "schema" && f.key() != "tables") return *error = "unknown field \"" + std::string(f.key()) + "\"", std::nullopt;
  }
  Schema s;
  auto name = json::get_string(root, "schema", &err);
  if (!name || !is_valid_sql_name(*name)) return *error = "\"schema\" is the schema's name with its version, like guestbook1", std::nullopt;
  s.name = *name;
  const auto* tables = json::get_object(root, "tables", &err);
  if (!tables || tables->empty()) return *error = "\"tables\" is required", std::nullopt;
  std::set<std::string> index_names;
  for (const auto& t : *tables) {
    Table table;
    table.name = std::string(t.key());
    if (!is_valid_sql_name(table.name)) return *error = table.name + ": a table name must match [a-z][a-z0-9_]*, not goober_ or sqlite_", std::nullopt;
    if (!t.value().is_object()) return *error = table.name + " must be an object", std::nullopt;
    const auto& to = t.value().get_object();
    for (const auto& f : to) {
      if (f.key() != "columns" && f.key() != "indexes") return *error = table.name + ": unknown field \"" + std::string(f.key()) + "\"", std::nullopt;
    }
    const auto* cols = json::get_object(to, "columns", &err);
    if (!cols || cols->empty()) return *error = table.name + ": \"columns\" is required", std::nullopt;
    int keys = 0;
    for (const auto& c : *cols) {
      Column col;
      if (!parse_column(table.name, std::string(c.key()), c.value(), &col, error)) return std::nullopt;
      keys += col.primary_key ? 1 : 0;
      std::string where = table.name + "." + col.name;
      if (col.primary_key && col.type == "int64") {
        s.warnings.push_back(where + " is an int64 primary key: new rows get this database's rowids, not host ids; "
                             "declare {\"type\": \"id\", \"primary-key\": true} (design/machine/038.Ids.md)");
      } else if (!col.primary_key && looks_like_reference(col.name) &&
                 (col.type == "string" || col.type == "int32" || col.type == "int64")) {
        s.warnings.push_back(where + " looks like a reference to a user or a row, but is " + col.type +
                             ": a reference is {\"type\": \"id\"}; ids are never text and never emails");
      }
      table.columns.push_back(std::move(col));
    }
    if (keys > 1) return *error = table.name + ": only one column can be the primary key", std::nullopt;
    if (keys == 0) {
      if (table.column("id")) return *error = table.name + ": id must be the primary key, or another column must be", std::nullopt;
      // As before: the table is keyed by its rowid. Kept so stored schemas mean what they meant.
      Column id;
      id.name = "id";
      id.type = "int64";
      id.primary_key = true;
      table.columns.insert(table.columns.begin(), std::move(id));
      s.warnings.push_back(table.name + " names no primary key, so it gets an int64 id: new rows get this database's "
                           "rowids, not host ids; declare \"id\": {\"type\": \"id\", \"primary-key\": true}");
    }
    if (const auto* idx = json::get_object(to, "indexes", &err)) {
      for (const auto& i : *idx) {
        Index index;
        index.name = std::string(i.key());
        std::string where = table.name + ".indexes." + index.name;
        if (!is_valid_sql_name(index.name)) return *error = where + ": an index name must match [a-z][a-z0-9_]*", std::nullopt;
        if (!index_names.insert(index.name).second) return *error = where + ": index names are unique in a schema", std::nullopt;
        if (!i.value().is_object()) return *error = where + " must be an object", std::nullopt;
        const auto& io = i.value().get_object();
        const auto* ic = json::get_array(io, "columns", &err);
        if (!ic || ic->empty()) return *error = where + ": \"columns\" is a list of columns", std::nullopt;
        for (const auto& c : *ic) {
          if (!c.is_string() || !table.column(c.get_string())) return *error = where + ": every column must be in the table", std::nullopt;
          index.columns.push_back(std::string(c.get_string()));
        }
        index.unique = json::get_bool(io, "unique", &err).value_or(false);
        table.indexes.push_back(std::move(index));
      }
    }
    if (!err.empty()) return *error = table.name + ": " + err, std::nullopt;
    s.tables.push_back(std::move(table));
  }
  s.text = std::string(text);
  return s;
}

std::optional<Schema> SchemaStore::check(std::string_view text, std::string* error) const {
  auto s = parse_schema(text, error);
  if (!s) return std::nullopt;
  if (auto existing = get(s->name)) {
    std::string err;
    auto a = json::parse(existing->text, &err);
    auto b = json::parse(text, &err);
    if (a && b && *a == *b) return existing;
    return *error = "schema " + s->name + " is already stored with different content; a schema is a version, so "
                    "give the new one a new name, like " + s->name + "_2",
           std::nullopt;
  }
  return s;
}

std::optional<Schema> SchemaStore::put(std::string_view text, std::string* error) {
  auto s = parse_schema(text, error);
  if (!s) return std::nullopt;
  if (auto existing = get(s->name)) {
    std::string err;
    auto a = json::parse(existing->text, &err);
    auto b = json::parse(text, &err);
    if (a && b && *a == *b) return existing;
    return *error = "schema " + s->name + " is already stored with different content; a schema is a version, so "
                    "give the new one a new name, like " + s->name + "_2",
           std::nullopt;
  }
  std::string err;
  auto parsed = json::parse(text, &err);
  if (!wal_.commit({{wal::FileChange::Kind::kWrite, "schemas/" + s->name + ".schema.json", json::pretty(*parsed)}}, error)) {
    return std::nullopt;
  }
  return s;
}

std::optional<Schema> SchemaStore::get(std::string_view name) const {
  if (!is_valid_sql_name(name)) return std::nullopt;
  auto text = base::read_file(wal_.root() / "schemas" / (std::string(name) + ".schema.json"));
  if (!text) return std::nullopt;
  std::string err;
  return parse_schema(*text, &err);
}

std::vector<std::string> SchemaStore::list() const {
  std::vector<std::string> out;
  for (const auto& f : base::list_files(wal_.root() / "schemas")) {
    if (f.ends_with(".schema.json") && f.find('/') == std::string::npos) out.push_back(f.substr(0, f.size() - 12));
  }
  return out;
}

}  // namespace goober::db
