#include "goober/json/json.h"

namespace goober::json {

std::optional<bj::value> parse(std::string_view text, std::string* error) {
  boost::system::error_code ec;
  bj::value v = bj::parse(text, ec);
  if (ec) {
    if (error) *error = "invalid JSON: " + ec.message();
    return std::nullopt;
  }
  return v;
}

std::string dump(const bj::value& value) { return bj::serialize(value); }

namespace {
void pretty_into(std::string& out, const bj::value& v, int indent) {
  auto pad = [&](int n) { out.append(size_t(n) * 2, ' '); };
  switch (v.kind()) {
    case bj::kind::object: {
      const auto& obj = v.get_object();
      if (obj.empty()) {
        out += "{}";
        return;
      }
      out += "{\n";
      size_t i = 0;
      for (const auto& kv : obj) {
        pad(indent + 1);
        out += bj::serialize(bj::value(kv.key()));
        out += ": ";
        pretty_into(out, kv.value(), indent + 1);
        if (++i < obj.size()) out += ",";
        out += "\n";
      }
      pad(indent);
      out += "}";
      return;
    }
    case bj::kind::array: {
      const auto& arr = v.get_array();
      if (arr.empty()) {
        out += "[]";
        return;
      }
      out += "[\n";
      for (size_t i = 0; i < arr.size(); ++i) {
        pad(indent + 1);
        pretty_into(out, arr[i], indent + 1);
        if (i + 1 < arr.size()) out += ",";
        out += "\n";
      }
      pad(indent);
      out += "]";
      return;
    }
    default:
      out += bj::serialize(v);
  }
}

template <typename T>
void wrong_type(std::string* error, std::string_view key, const char* want) {
  if (error) *error = "field \"" + std::string(key) + "\" must be " + want;
}
}  // namespace

std::string pretty(const bj::value& value) {
  std::string out;
  pretty_into(out, value, 0);
  out += "\n";
  return out;
}

std::optional<std::string> get_string(const bj::object& obj, std::string_view key, std::string* error) {
  auto it = obj.find(key);
  if (it == obj.end()) return std::nullopt;
  if (!it->value().is_string()) {
    wrong_type<std::string>(error, key, "a string");
    return std::nullopt;
  }
  return std::string(it->value().get_string());
}

std::optional<int64_t> get_int(const bj::object& obj, std::string_view key, std::string* error) {
  auto it = obj.find(key);
  if (it == obj.end()) return std::nullopt;
  const auto& v = it->value();
  if (v.is_int64()) return v.get_int64();
  if (v.is_uint64() && v.get_uint64() <= uint64_t(INT64_MAX)) return int64_t(v.get_uint64());
  wrong_type<int64_t>(error, key, "an integer");
  return std::nullopt;
}

std::optional<bool> get_bool(const bj::object& obj, std::string_view key, std::string* error) {
  auto it = obj.find(key);
  if (it == obj.end()) return std::nullopt;
  if (!it->value().is_bool()) {
    wrong_type<bool>(error, key, "true or false");
    return std::nullopt;
  }
  return it->value().get_bool();
}

const bj::object* get_object(const bj::object& obj, std::string_view key, std::string* error) {
  auto it = obj.find(key);
  if (it == obj.end()) return nullptr;
  if (!it->value().is_object()) {
    wrong_type<bj::object>(error, key, "an object");
    return nullptr;
  }
  return &it->value().get_object();
}

const bj::array* get_array(const bj::object& obj, std::string_view key, std::string* error) {
  auto it = obj.find(key);
  if (it == obj.end()) return nullptr;
  if (!it->value().is_array()) {
    wrong_type<bj::array>(error, key, "an array");
    return nullptr;
  }
  return &it->value().get_array();
}

}  // namespace goober::json
