#pragma once

// A box's types (036.Json.md): the components.schemas of an OpenAPI document, held to
// a strict subset so every type maps onto a C structure and every rule is checkable.
// The host uses it to validate request bodies, query parameters, and tool inputs before
// a box wakes up, to fill in defaults, and to describe tool inputs to agents.
// See design/machine/011.Types.md.

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"

namespace goober::types {

namespace bj = boost::json;

struct Schema;
using SchemaPtr = std::shared_ptr<const Schema>;

// One node of the subset.
struct Schema {
  enum class Kind { kObject, kArray, kString, kInt32, kInt64, kFloat, kDouble, kBoolean, kRef };
  Kind kind = Kind::kString;
  std::string ref;  // kRef: the name of a schema in the same document
  std::string description;
  std::optional<bj::value> default_value;
  // kObject: fields in declaration order; additionalProperties is always false.
  std::vector<std::pair<std::string, SchemaPtr>> properties;
  std::set<std::string> required;
  // kArray
  SchemaPtr items;
  int64_t max_items = 0;
  int64_t min_items = 0;
  // kString
  std::optional<int64_t> max_length;
  std::optional<int64_t> min_length;
  std::vector<std::string> enum_values;
  std::string format;  // "", "date-time" (RFC 3339 in UTC, ending in Z), or "date" (YYYY-MM-DD)
  // kInt32, kInt64, kFloat, kDouble
  std::optional<double> minimum;
  std::optional<double> maximum;
  // const: the one value allowed, for a string, boolean, integer, or number. An int64's is
  // kept as its string of digits, as int64 values cross JSON (021).
  std::optional<bj::value> const_value;
};

class Types;

// Parses one schema in the subset. `scalars_only` is for query parameters and headers,
// which hold a single value. $refs are recorded, and resolved by Types::parse.
std::optional<SchemaPtr> parse_schema(const bj::value& v, const std::string& where, bool scalars_only, std::string* error);

// Checks a JSON value against a schema. On success returns the value the box should see:
// defaults filled in, and int64 values as strings (021.PrimaryKeys.md § Ids in JSON).
// The error names the path, like "$.items[2].quantity: must be at most 100".
std::optional<bj::value> check(const Types* types, const Schema& schema, const bj::value& value, std::string* error);

// Checks text from a URL or a header, like "42" or "true", against a scalar schema.
std::optional<bj::value> check_text(const Schema& schema, std::string_view text, std::string* error);

// A box's types document.
class Types {
 public:
  // Parses an OpenAPI document and its components.schemas. Refuses anything outside the
  // subset, $refs outside the document, and recursive types.
  static std::optional<Types> parse(std::string_view text, std::string* error);

  bool has(const std::string& name) const { return schemas_.count(name) > 0; }
  const Schema* find(const std::string& name) const;
  std::vector<std::string> names() const;

  // Checks a value against a named type; see check() above.
  std::optional<bj::value> check(const std::string& name, const bj::value& value, std::string* error) const;

  // A self-contained JSON Schema for a named type, with $refs inlined, as MCP's
  // tools/list wants for inputSchema.
  bj::value json_schema(const std::string& name) const;

 private:
  std::map<std::string, SchemaPtr> schemas_;
};

// "json:Order" -> "Order"; anything else -> "".
std::string json_type_name(std::string_view ref);

// A schema as plain JSON Schema, with $refs resolved through `types` when given.
bj::value to_json_schema(const Types* types, const Schema& schema);

}  // namespace goober::types
