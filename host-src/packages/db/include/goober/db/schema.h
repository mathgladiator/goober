#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"
#include "goober/wal/wal.h"

namespace goober::db {

namespace bj = boost::json;

// One column of a .schema (design/machine/008.Databases.md §2).
struct Column {
  std::string name;
  std::string type;  // id, int32, int64, double, float, boolean, or string
  bool primary_key = false;
  bool nullable = false;
  bool not_a_reference = false;  // "not-a-reference": silences the reference warning (038 § 5)
  std::optional<bj::value> default_value;
  std::optional<int64_t> max_length;
  // The SQL that declares the column inside CREATE TABLE or ALTER TABLE ADD COLUMN.
  std::string sql() const;
  // A stable description, for comparing what a schema declared with what is there.
  std::string signature() const;
  // What an older box reads once the column is dropped (041 § 5), as a SQL literal: its
  // default, or NULL when nullable, or else 0, '' or false.
  std::string dropped_value_sql() const;
};

struct Index {
  std::string name;
  std::vector<std::string> columns;
  bool unique = false;
  std::string signature() const;
};

struct Table {
  std::string name;
  std::vector<Column> columns;
  std::vector<Index> indexes;
  const Column* column(std::string_view name) const;
};

struct Schema {
  std::string name;
  // The version this one replaces (design/machine/041.SchemaEvolution.md § 2): its tables replace
  // that schema's tables, so what they leave out is dropped. Empty: partial and expand only (008 § 3).
  std::string follows;
  std::vector<Table> tables;
  std::string text;  // the JSON it was parsed from
  // What a person should change, though the schema is accepted: keys and references that
  // aren't host ids (038 § 5).
  std::vector<std::string> warnings;
  const Table* table(std::string_view name) const;
  // The table's primary key when the host stamps it (type id), or nullptr.
  static const Column* stamped_key(const Table& t);
};

// One change between a schema and the one it follows (041 § 3).
struct Change {
  std::string kind;  // expansion, contraction, indexing, emulated, forced, or refused
  std::string table, column, index;  // column and index empty when the change is the table's
  std::string says;  // the sentence a person reads
};

// What a schema's step from the version it follows does (041 § 3).
struct Step {
  std::string from, to;
  std::vector<Change> changes;
  std::vector<std::string> consequences;  // 420's warnings that apply, in its words
  std::set<std::string> forced_tables;    // tables only an upgrade box can carry over
  bool contracts() const;
  bool refused() const;
  // A contraction or a forced change: a person approves the schema in the panel first (041 § 6).
  bool needs_person() const { return contracts() || !forced_tables.empty(); }
  bj::object to_json() const;
};

// Compares `to` with `from`, the schema it follows, table by table.
Step plan_step(const Schema& from, const Schema& to);

// Names of tables, columns, indexes, and schemas: [a-z][a-z0-9_]*, at most 63, and not
// goober_ or sqlite_.
bool is_valid_sql_name(std::string_view name);

// Parses and checks a schema. A table without a primary key gets an int64 "id" (a rowid);
// a key of type id is the host's (038 § 3).
std::optional<Schema> parse_schema(std::string_view text, std::string* error);

// Schemas by name, stored once under storage/schemas/$name.schema.json through the host WAL.
class SchemaStore {
 public:
  explicit SchemaStore(wal::HostWal& wal) : wal_(wal) {}
  // Stores a schema. The same name with different content is refused; the same content is a no-op.
  std::optional<Schema> put(std::string_view text, std::string* error);
  // What put would answer, without storing anything: the schema, or why it would be refused.
  std::optional<Schema> check(std::string_view text, std::string* error) const;
  std::optional<Schema> get(std::string_view name) const;
  std::vector<std::string> list() const;
  // Retiring (008 § 11): a stored schema no new upload or database may use; its file stays for
  // history. `record` is JSON saying who and why, kept beside it as $name.retired.
  bool retire(std::string_view name, const std::string& record, std::string* error);
  // The retirement record, or nullopt while the schema is in service.
  std::optional<std::string> retired(std::string_view name) const;
  // The step from the schema `s` follows to `s`, or nullopt when it follows none (or what it
  // follows isn't stored).
  std::optional<Step> step(const Schema& s) const;
  // The stored schema that follows `name`, if any.
  std::optional<std::string> follower(std::string_view name) const;
  // A person's approval of a held schema (041 § 6), kept as $name.approved: JSON saying who and why.
  bool approve(std::string_view name, const std::string& record, std::string* error);
  std::optional<std::string> approval(std::string_view name) const;
  // Stored, its step drops or rewrites data, and no person has approved it yet.
  bool held(std::string_view name) const;

 private:
  wal::HostWal& wal_;
};

}  // namespace goober::db
