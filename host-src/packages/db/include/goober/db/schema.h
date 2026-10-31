#pragma once

#include <cstdint>
#include <optional>
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
  std::optional<bj::value> default_value;
  std::optional<int64_t> max_length;
  // The SQL that declares the column inside CREATE TABLE or ALTER TABLE ADD COLUMN.
  std::string sql() const;
  // A stable description, for comparing what a schema declared with what is there.
  std::string signature() const;
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
  std::vector<Table> tables;
  std::string text;  // the JSON it was parsed from
  // What a person should change, though the schema is accepted: keys and references that
  // aren't host ids (038 § 5).
  std::vector<std::string> warnings;
  const Table* table(std::string_view name) const;
  // The table's primary key when the host stamps it (type id), or nullptr.
  static const Column* stamped_key(const Table& t);
};

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

 private:
  wal::HostWal& wal_;
};

}  // namespace goober::db
