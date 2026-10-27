#include "goober/types/types.h"

#include <cmath>
#include <functional>
#include <limits>

namespace goober::types {

namespace {

// Keywords that only describe, and change nothing a validator does.
const std::set<std::string> kAnnotations = {"description", "title", "example", "examples", "deprecated",
                                            "readOnly", "writeOnly"};

// Keywords outside the subset, refused by name so the message says why (036 § A strict subset).
const std::set<std::string> kRefused = {"oneOf", "anyOf", "allOf", "not", "patternProperties", "if", "then",
                                        "else", "nullable", "const", "dependentRequired", "dependentSchemas",
                                        "unevaluatedProperties", "propertyNames", "uniqueItems", "prefixItems",
                                        "contains", "pattern", "multipleOf", "exclusiveMinimum", "exclusiveMaximum"};

constexpr size_t kMaxDepth = 32;

bool fail(std::string* error, const std::string& message) {
  *error = message;
  return false;
}

std::optional<int64_t> non_negative(const bj::value& v) {
  if (v.is_int64() && v.as_int64() >= 0) return v.as_int64();
  if (v.is_uint64() && v.as_uint64() <= uint64_t(std::numeric_limits<int64_t>::max())) return int64_t(v.as_uint64());
  return std::nullopt;
}

std::optional<double> number(const bj::value& v) {
  if (v.is_int64()) return double(v.as_int64());
  if (v.is_uint64()) return double(v.as_uint64());
  if (v.is_double()) return v.as_double();
  return std::nullopt;
}

bool digits(std::string_view s, size_t from, size_t n) {
  if (s.size() < from + n) return false;
  for (size_t i = from; i < from + n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

// YYYY-MM-DD, with a month and day in range.
bool is_date(std::string_view s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-' || !digits(s, 0, 4) || !digits(s, 5, 2) || !digits(s, 8, 2)) return false;
  int m = (s[5] - '0') * 10 + (s[6] - '0');
  int d = (s[8] - '0') * 10 + (s[9] - '0');
  return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// YYYY-MM-DDTHH:MM:SS[.fraction]Z: an instant in UTC, as 999 § Types on the wire wants.
bool is_date_time(std::string_view s) {
  if (s.size() < 20 || !is_date(s.substr(0, 10)) || s[10] != 'T' || s[13] != ':' || s[16] != ':' ||
      !digits(s, 11, 2) || !digits(s, 14, 2) || !digits(s, 17, 2) || s.back() != 'Z') {
    return false;
  }
  int h = (s[11] - '0') * 10 + (s[12] - '0');
  int mi = (s[14] - '0') * 10 + (s[15] - '0');
  int se = (s[17] - '0') * 10 + (s[18] - '0');
  if (h > 23 || mi > 59 || se > 60) return false;
  if (s.size() == 20) return true;
  return s[19] == '.' && s.size() >= 22 && s.size() <= 30 && digits(s, 20, s.size() - 21);
}

// A bound as JSON: an integer when it is one, so messages and schemas read "100", not "1E2".
bj::value bound(double x) {
  if (std::floor(x) == x && std::fabs(x) < 9e15) return bj::value(int64_t(x));
  return bj::value(x);
}

size_t utf8_length(std::string_view s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

const char* kind_name(Schema::Kind k) {
  switch (k) {
    case Schema::Kind::kObject: return "an object";
    case Schema::Kind::kArray: return "an array";
    case Schema::Kind::kString: return "a string";
    case Schema::Kind::kInt32: return "an int32";
    case Schema::Kind::kInt64: return "an int64";
    case Schema::Kind::kFloat: return "a float";
    case Schema::Kind::kDouble: return "a double";
    case Schema::Kind::kBoolean: return "a boolean";
    case Schema::Kind::kRef: return "a reference";
  }
  return "a value";
}

bool parse_into(const bj::value& v, const std::string& where, bool scalars_only, size_t depth, Schema* out,
                std::string* error);

std::optional<SchemaPtr> parse_node(const bj::value& v, const std::string& where, bool scalars_only, size_t depth,
                                    std::string* error) {
  auto s = std::make_shared<Schema>();
  if (!parse_into(v, where, scalars_only, depth, s.get(), error)) return std::nullopt;
  return s;
}

bool parse_into(const bj::value& v, const std::string& where, bool scalars_only, size_t depth, Schema* out,
                std::string* error) {
  if (depth > kMaxDepth) return fail(error, where + ": nested too deeply");
  if (!v.is_object()) return fail(error, where + ": a schema is an object");
  const auto& o = v.as_object();
  for (const auto& kv : o) {
    std::string k(kv.key());
    if (kRefused.count(k)) return fail(error, where + ": \"" + k + "\" is outside goober's subset (036.Json.md)");
  }
  if (auto* d = o.if_contains("description"); d && d->is_string()) out->description = std::string(d->as_string());

  if (auto* ref = o.if_contains("$ref")) {
    if (scalars_only) return fail(error, where + ": a single value can't be a $ref");
    constexpr std::string_view kPrefix = "#/components/schemas/";
    if (!ref->is_string() || !std::string_view(ref->as_string()).starts_with(kPrefix)) {
      return fail(error, where + ": a $ref must point at #/components/schemas/ in the same document");
    }
    for (const auto& kv : o) {
      std::string k(kv.key());
      if (k != "$ref" && !kAnnotations.count(k)) return fail(error, where + ": a $ref stands alone (\"" + k + "\" beside it)");
    }
    out->kind = Schema::Kind::kRef;
    out->ref = std::string(std::string_view(ref->as_string()).substr(kPrefix.size()));
    return true;
  }

  auto* type = o.if_contains("type");
  if (!type || !type->is_string()) return fail(error, where + ": needs \"type\" (one type; null isn't offered)");
  std::string t(type->as_string());
  std::string format;
  if (auto* f = o.if_contains("format")) {
    if (!f->is_string()) return fail(error, where + ": format must be a string");
    format = std::string(f->as_string());
  }
  std::set<std::string> allowed = {"type", "default"};
  if (t == "integer") {
    if (format == "int32") out->kind = Schema::Kind::kInt32;
    else if (format == "int64") out->kind = Schema::Kind::kInt64;
    else return fail(error, where + ": an integer needs format int32 or int64, since integer alone has no width");
    allowed.insert({"format", "minimum", "maximum"});
  } else if (t == "number") {
    if (format == "float") out->kind = Schema::Kind::kFloat;
    else if (format == "double") out->kind = Schema::Kind::kDouble;
    else return fail(error, where + ": a number needs format float or double");
    allowed.insert({"format", "minimum", "maximum"});
  } else if (t == "boolean") {
    out->kind = Schema::Kind::kBoolean;
  } else if (t == "string") {
    out->kind = Schema::Kind::kString;
    if (format == "token") return fail(error, where + ": format token needs host-signed tokens, which aren't built yet");
    if (!format.empty() && format != "date-time" && format != "date") {
      return fail(error, where + ": string formats are date-time and date (token comes later)");
    }
    out->format = format;
    allowed.insert({"format", "maxLength", "minLength", "enum"});
  } else if (t == "object" || t == "array") {
    if (scalars_only) return fail(error, where + ": a single value can't be " + t + " (query parameters and headers are scalars)");
    if (t == "object") {
      out->kind = Schema::Kind::kObject;
      allowed.insert({"properties", "required", "additionalProperties"});
    } else {
      out->kind = Schema::Kind::kArray;
      allowed.insert({"items", "maxItems", "minItems"});
    }
  } else {
    return fail(error, where + ": unknown type \"" + t + "\"");
  }
  for (const auto& kv : o) {
    std::string k(kv.key());
    if (!allowed.count(k) && !kAnnotations.count(k)) {
      return fail(error, where + ": \"" + k + "\" doesn't apply to " + kind_name(out->kind));
    }
  }

  if (out->kind == Schema::Kind::kInt32 || out->kind == Schema::Kind::kInt64 || out->kind == Schema::Kind::kFloat ||
      out->kind == Schema::Kind::kDouble) {
    for (const char* k : {"minimum", "maximum"}) {
      if (auto* m = o.if_contains(k)) {
        auto n = number(*m);
        if (!n) return fail(error, where + ": " + k + " must be a number");
        (std::string_view(k) == "minimum" ? out->minimum : out->maximum) = *n;
      }
    }
  }
  if (out->kind == Schema::Kind::kString) {
    for (const char* k : {"maxLength", "minLength"}) {
      if (auto* m = o.if_contains(k)) {
        auto n = non_negative(*m);
        if (!n) return fail(error, where + ": " + k + " must be a non-negative integer");
        (std::string_view(k) == "maxLength" ? out->max_length : out->min_length) = *n;
      }
    }
    if (auto* e = o.if_contains("enum")) {
      if (!e->is_array() || e->as_array().empty()) return fail(error, where + ": enum is a non-empty list of strings");
      for (const auto& x : e->as_array()) {
        if (!x.is_string()) return fail(error, where + ": enum values are strings");
        out->enum_values.emplace_back(x.as_string());
      }
    }
  }
  if (out->kind == Schema::Kind::kArray) {
    auto* items = o.if_contains("items");
    if (!items) return fail(error, where + ": an array needs items");
    auto max = o.if_contains("maxItems");
    auto n = max ? non_negative(*max) : std::nullopt;
    if (!n || *n == 0) return fail(error, where + ": an array needs maxItems, so it has a ceiling");
    out->max_items = *n;
    if (auto* m = o.if_contains("minItems")) {
      auto mn = non_negative(*m);
      if (!mn || *mn > out->max_items) return fail(error, where + ": minItems must be from 0 to maxItems");
      out->min_items = *mn;
    }
    auto item = parse_node(*items, where + ".items", false, depth + 1, error);
    if (!item) return false;
    out->items = *item;
  }
  if (out->kind == Schema::Kind::kObject) {
    auto* ap = o.if_contains("additionalProperties");
    if (!ap || !ap->is_bool() || ap->as_bool()) {
      return fail(error, where + ": an object needs \"additionalProperties\": false, so unknown fields are refused");
    }
    if (auto* props = o.if_contains("properties")) {
      if (!props->is_object()) return fail(error, where + ": properties must be an object");
      for (const auto& kv : props->as_object()) {
        std::string name(kv.key());
        auto p = parse_node(kv.value(), where + "." + name, false, depth + 1, error);
        if (!p) return false;
        out->properties.emplace_back(name, *p);
      }
    }
    if (auto* req = o.if_contains("required")) {
      if (!req->is_array()) return fail(error, where + ": required is a list of field names");
      for (const auto& r : req->as_array()) {
        if (!r.is_string()) return fail(error, where + ": required is a list of field names");
        std::string name(r.as_string());
        bool known = false;
        for (const auto& [n, _] : out->properties) known |= n == name;
        if (!known) return fail(error, where + ": required names \"" + name + "\", which isn't a property");
        out->required.insert(name);
      }
    }
  }
  if (auto* d = o.if_contains("default")) {
    // A default must itself be valid. Objects and arrays have no defaults in the subset.
    if (out->kind == Schema::Kind::kObject || out->kind == Schema::Kind::kArray) {
      return fail(error, where + ": only single values have defaults");
    }
    std::string why;
    auto ok = check(nullptr, *out, *d, &why);
    if (!ok) return fail(error, where + ": the default isn't valid: " + why);
    out->default_value = *d;
  }
  return true;
}

std::optional<bj::value> check_at(const Types* types, const Schema& s, const bj::value& v, const std::string& path,
                                  size_t depth, std::string* error) {
  auto bad = [&](const std::string& why) -> std::optional<bj::value> {
    *error = path + ": " + why;
    return std::nullopt;
  };
  if (depth > kMaxDepth) return bad("nested too deeply");
  auto range = [&](double x) -> std::optional<std::string> {
    if (s.minimum && x < *s.minimum) return "must be at least " + json::dump(bound(*s.minimum));
    if (s.maximum && x > *s.maximum) return "must be at most " + json::dump(bound(*s.maximum));
    return std::nullopt;
  };
  switch (s.kind) {
    case Schema::Kind::kRef: {
      const Schema* target = types ? types->find(s.ref) : nullptr;
      if (!target) return bad("unknown type " + s.ref);
      return check_at(types, *target, v, path, depth + 1, error);
    }
    case Schema::Kind::kBoolean:
      if (!v.is_bool()) return bad("must be true or false");
      return v;
    case Schema::Kind::kInt32: {
      int64_t n;
      if (v.is_int64()) n = v.as_int64();
      else if (v.is_uint64() && v.as_uint64() <= uint64_t(INT32_MAX)) n = int64_t(v.as_uint64());
      else return bad("must be an int32");
      if (n < INT32_MIN || n > INT32_MAX) return bad("must be an int32");
      if (auto why = range(double(n))) return bad(*why);
      return bj::value(n);
    }
    case Schema::Kind::kInt64: {
      // Ids cross JSON as strings (021); a number that fits is accepted, and passed on as a string.
      int64_t n;
      if (v.is_int64()) {
        n = v.as_int64();
      } else if (v.is_uint64()) {
        if (v.as_uint64() > uint64_t(std::numeric_limits<int64_t>::max())) return bad("must be an int64");
        n = int64_t(v.as_uint64());
      } else if (v.is_string()) {
        std::string_view t = v.as_string();
        if (t.empty() || t.size() > 20) return bad("must be an int64, as a string of digits");
        size_t i = t[0] == '-' ? 1 : 0;
        if (i == t.size()) return bad("must be an int64, as a string of digits");
        for (size_t j = i; j < t.size(); ++j) {
          if (t[j] < '0' || t[j] > '9') return bad("must be an int64, as a string of digits");
        }
        try {
          n = std::stoll(std::string(t));
        } catch (...) {
          return bad("must fit in an int64");
        }
      } else {
        return bad("must be an int64");
      }
      if (auto why = range(double(n))) return bad(*why);
      return bj::value(std::to_string(n));
    }
    case Schema::Kind::kFloat:
    case Schema::Kind::kDouble: {
      auto x = number(v);
      if (!x || !std::isfinite(*x)) return bad(std::string("must be ") + kind_name(s.kind));
      if (s.kind == Schema::Kind::kFloat && std::fabs(*x) > double(std::numeric_limits<float>::max())) {
        return bad("must fit in a float");
      }
      if (auto why = range(*x)) return bad(*why);
      return v;
    }
    case Schema::Kind::kString: {
      if (!v.is_string()) return bad("must be a string");
      std::string_view t = v.as_string();
      size_t n = utf8_length(t);
      if (s.max_length && int64_t(n) > *s.max_length) return bad("is longer than " + std::to_string(*s.max_length) + " characters");
      if (s.min_length && int64_t(n) < *s.min_length) return bad("is shorter than " + std::to_string(*s.min_length) + " characters");
      if (s.format == "date-time" && !is_date_time(t)) return bad("must be an RFC 3339 instant in UTC, like 2026-10-27T13:00:00Z");
      if (s.format == "date" && !is_date(t)) return bad("must be a date, like 2026-10-27");
      if (!s.enum_values.empty()) {
        bool ok = false;
        for (const auto& e : s.enum_values) ok |= e == t;
        if (!ok) {
          std::string list;
          for (const auto& e : s.enum_values) list += (list.empty() ? "" : ", ") + e;
          return bad("must be one of " + list);
        }
      }
      return v;
    }
    case Schema::Kind::kArray: {
      if (!v.is_array()) return bad("must be an array");
      const auto& a = v.as_array();
      if (int64_t(a.size()) > s.max_items) return bad("holds more than " + std::to_string(s.max_items) + " items");
      if (int64_t(a.size()) < s.min_items) return bad("holds fewer than " + std::to_string(s.min_items) + " items");
      bj::array out;
      for (size_t i = 0; i < a.size(); ++i) {
        auto item = check_at(types, *s.items, a[i], path + "[" + std::to_string(i) + "]", depth + 1, error);
        if (!item) return std::nullopt;
        out.push_back(std::move(*item));
      }
      return out;
    }
    case Schema::Kind::kObject: {
      if (!v.is_object()) return bad("must be an object");
      const auto& o = v.as_object();
      for (const auto& kv : o) {
        bool known = false;
        for (const auto& [n, _] : s.properties) known |= n == kv.key();
        if (!known) return bad("unknown field \"" + std::string(kv.key()) + "\" (objects are closed)");
      }
      bj::object out;
      for (const auto& [name, field] : s.properties) {
        auto* got = o.if_contains(name);
        std::string at = path + "." + name;
        if (!got) {
          if (s.required.count(name)) {
            *error = at + ": is required";
            return std::nullopt;
          }
          // The host fills defaults in (036 § A strict subset); a ref's default lives on its target.
          const Schema* target = field.get();
          if (target->kind == Schema::Kind::kRef && types) target = types->find(target->ref);
          if (target && target->default_value) out[name] = *target->default_value;
          continue;
        }
        if (got->is_null()) {
          *error = at + ": is null; leave the field out instead";
          return std::nullopt;
        }
        auto checked = check_at(types, *field, *got, at, depth + 1, error);
        if (!checked) return std::nullopt;
        out[name] = std::move(*checked);
      }
      return out;
    }
  }
  return bad("unknown schema");
}

}  // namespace

std::optional<SchemaPtr> parse_schema(const bj::value& v, const std::string& where, bool scalars_only,
                                      std::string* error) {
  return parse_node(v, where, scalars_only, 0, error);
}

std::optional<bj::value> check(const Types* types, const Schema& schema, const bj::value& value, std::string* error) {
  return check_at(types, schema, value, "$", 0, error);
}

std::optional<bj::value> check_text(const Schema& s, std::string_view text, std::string* error) {
  bj::value v;
  switch (s.kind) {
    case Schema::Kind::kString:
      v = bj::string(text);
      break;
    case Schema::Kind::kBoolean:
      if (text == "true") v = true;
      else if (text == "false") v = false;
      else return *error = "must be true or false", std::nullopt;
      break;
    case Schema::Kind::kInt64:
      v = bj::string(text);
      break;
    case Schema::Kind::kInt32:
    case Schema::Kind::kFloat:
    case Schema::Kind::kDouble: {
      std::string err;
      auto parsed = json::parse(text, &err);
      if (!parsed || !(parsed->is_number())) return *error = std::string("must be ") + kind_name(s.kind), std::nullopt;
      v = *parsed;
      break;
    }
    default:
      return *error = "only single values come from text", std::nullopt;
  }
  std::string why;
  auto out = check(nullptr, s, v, &why);
  if (!out) {
    *error = why.starts_with("$: ") ? why.substr(3) : why;
    return std::nullopt;
  }
  return out;
}

std::optional<Types> Types::parse(std::string_view text, std::string* error) {
  std::string err;
  auto doc = json::parse(text, &err);
  if (!doc) return *error = "the types document isn't JSON: " + err, std::nullopt;
  if (!doc->is_object()) return *error = "the types document must be an OpenAPI object", std::nullopt;
  const auto& root = doc->as_object();
  auto* version = root.if_contains("openapi");
  if (!version || !version->is_string() || !std::string_view(version->as_string()).starts_with("3.")) {
    return *error = "the types document needs \"openapi\": \"3.x\"", std::nullopt;
  }
  Types out;
  const bj::value* schemas = nullptr;
  if (auto* c = root.if_contains("components"); c && c->is_object()) schemas = c->as_object().if_contains("schemas");
  if (!schemas) return out;  // a document with no types is allowed, if useless
  if (!schemas->is_object()) return *error = "components.schemas must be an object", std::nullopt;
  for (const auto& kv : schemas->as_object()) {
    std::string name(kv.key());
    bool ok = !name.empty() && name.size() <= 64 && std::isalpha(static_cast<unsigned char>(name[0]));
    for (char c : name) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
    if (!ok) return *error = "type name \"" + name + "\" must be a letter, then letters, digits, or _", std::nullopt;
    auto s = parse_schema(kv.value(), name, false, error);
    if (!s) return std::nullopt;
    out.schemas_[name] = *s;
  }
  // Every $ref resolves, and no type reaches itself (036: recursive types are refused).
  std::function<bool(const Schema&, std::vector<std::string>&)> walk = [&](const Schema& s,
                                                                         std::vector<std::string>& stack) {
    if (s.kind == Schema::Kind::kRef) {
      if (!out.schemas_.count(s.ref)) return fail(error, stack.back() + ": $ref to unknown type " + s.ref);
      for (const auto& n : stack) {
        if (n == s.ref) return fail(error, stack.front() + ": recursive types are refused (" + s.ref + " reaches itself)");
      }
      stack.push_back(s.ref);
      bool ok = walk(*out.schemas_.at(s.ref), stack);
      stack.pop_back();
      return ok;
    }
    if (s.items && !walk(*s.items, stack)) return false;
    for (const auto& [_, p] : s.properties) {
      if (!walk(*p, stack)) return false;
    }
    return true;
  };
  for (const auto& [name, s] : out.schemas_) {
    std::vector<std::string> stack{name};
    if (!walk(*s, stack)) return std::nullopt;
  }
  return out;
}

const Schema* Types::find(const std::string& name) const {
  auto it = schemas_.find(name);
  return it == schemas_.end() ? nullptr : it->second.get();
}

std::vector<std::string> Types::names() const {
  std::vector<std::string> out;
  for (const auto& [n, _] : schemas_) out.push_back(n);
  return out;
}

std::optional<bj::value> Types::check(const std::string& name, const bj::value& value, std::string* error) const {
  const Schema* s = find(name);
  if (!s) return *error = "unknown type " + name, std::nullopt;
  return types::check(this, *s, value, error);
}

bj::value Types::json_schema(const std::string& name) const {
  const Schema* s = find(name);
  return s ? to_json_schema(this, *s) : bj::value(bj::object{{"type", "object"}});
}

std::string json_type_name(std::string_view ref) {
  constexpr std::string_view kPrefix = "json:";
  return ref.starts_with(kPrefix) && ref.size() > kPrefix.size() ? std::string(ref.substr(kPrefix.size())) : "";
}

bj::value to_json_schema(const Types* types, const Schema& s) {
  bj::object o;
  switch (s.kind) {
    case Schema::Kind::kRef: {
      const Schema* target = types ? types->find(s.ref) : nullptr;
      return target ? to_json_schema(types, *target) : bj::value(bj::object{});
    }
    case Schema::Kind::kBoolean: o["type"] = "boolean"; break;
    case Schema::Kind::kInt32: o["type"] = "integer"; o["format"] = "int32"; break;
    // An int64 crosses as a string of digits (021), so that is what a caller is told to send.
    case Schema::Kind::kInt64: o["type"] = "string"; o["pattern"] = "^-?[0-9]{1,19}$"; o["format"] = "int64"; break;
    case Schema::Kind::kFloat: o["type"] = "number"; o["format"] = "float"; break;
    case Schema::Kind::kDouble: o["type"] = "number"; o["format"] = "double"; break;
    case Schema::Kind::kString: {
      o["type"] = "string";
      if (!s.format.empty()) o["format"] = s.format;
      if (s.max_length) o["maxLength"] = *s.max_length;
      if (s.min_length) o["minLength"] = *s.min_length;
      if (!s.enum_values.empty()) {
        bj::array e;
        for (const auto& v : s.enum_values) e.push_back(bj::value(v));
        o["enum"] = std::move(e);
      }
      break;
    }
    case Schema::Kind::kArray:
      o["type"] = "array";
      o["items"] = to_json_schema(types, *s.items);
      o["maxItems"] = s.max_items;
      if (s.min_items) o["minItems"] = s.min_items;
      break;
    case Schema::Kind::kObject: {
      o["type"] = "object";
      bj::object props;
      for (const auto& [n, p] : s.properties) props[n] = to_json_schema(types, *p);
      o["properties"] = std::move(props);
      if (!s.required.empty()) {
        bj::array req;
        for (const auto& [n, _] : s.properties) {
          if (s.required.count(n)) req.push_back(bj::value(n));
        }
        o["required"] = std::move(req);
      }
      o["additionalProperties"] = false;
      break;
    }
  }
  if (s.minimum && s.kind != Schema::Kind::kInt64) o["minimum"] = bound(*s.minimum);
  if (s.maximum && s.kind != Schema::Kind::kInt64) o["maximum"] = bound(*s.maximum);
  if (!s.description.empty()) o["description"] = s.description;
  if (s.default_value) o["default"] = *s.default_value;
  return o;
}

}  // namespace goober::types
