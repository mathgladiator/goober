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
  std::string type;  // int32, int64, double, float, boolean, or string
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
  const Table* table(std::string_view name) const;
};

// Names of tables, columns, indexes, and schemas: [a-z][a-z0-9_]*, at most 63, and not
// goober_ or sqlite_.
bool is_valid_sql_name(std::string_view name);

// Parses and checks a schema. A table without a primary key gets an int64 "id".
std::optional<Schema> parse_schema(std::string_view text, std::string* error);

// Schemas by name, stored once under storage/schemas/$name.schema.json through the host WAL.
class SchemaStore {
 public:
  explicit SchemaStore(wal::HostWal& wal) : wal_(wal) {}
  // Stores a schema. The same name with different content is refused; the same content is a no-op.
  std::optional<Schema> put(std::string_view text, std::string* error);
  std::optional<Schema> get(std::string_view name) const;
  std::vector<std::string> list() const;

 private:
  wal::HostWal& wal_;
};

}  // namespace goober::db
