#include "common.h"

namespace goober::libraries::detail {

std::string text(const bj::object& o, std::string_view k, bool required) {
  auto* v = o.if_contains(k);
  if (!v || v->is_null()) {
    if (required) bad(std::string(k) + " is required");
    return "";
  }
  if (!v->is_string()) bad(std::string(k) + " is text");
  return std::string(v->as_string());
}

std::optional<int64_t> integer(const bj::object& o, std::string_view k) {
  auto* v = o.if_contains(k);
  if (!v || v->is_null()) return std::nullopt;
  if (v->is_int64()) return v->as_int64();
  if (v->is_uint64() && v->as_uint64() <= uint64_t(INT64_MAX)) return int64_t(v->as_uint64());
  if (v->is_double() && v->as_double() == double(int64_t(v->as_double()))) return int64_t(v->as_double());
  bad(std::string(k) + " is a whole number");
}

const bj::object* object(const bj::object& o, std::string_view k) {
  auto* v = o.if_contains(k);
  if (!v || v->is_null()) return nullptr;
  if (!v->is_object()) bad(std::string(k) + " is an object");
  return &v->as_object();
}

std::shared_ptr<const calendar::Zone> zone_of(const std::string& name, Context& ctx) {
  ctx.used_tzdata = true;
  auto z = calendar::iana(name);
  if (!z) {
    if (auto off = calendar::parse_offset(name)) return calendar::fixed(*off);
    bad("unknown zone " + name + ": an IANA name like America/Chicago (time.zones lists them)");
  }
  return z;
}

std::string utc_text(int64_t ms) {
  int64_t s = ms >= 0 ? ms / 1000 : -((-ms + 999) / 1000);
  int frac = int(ms - s * 1000);
  auto c = calendar::civil_from_seconds(s);
  char buf[48];
  if (frac) {
    std::snprintf(buf, sizeof buf, "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", (long long)c.y, c.mo, c.d, c.h, c.mi, c.s, frac);
  } else {
    std::snprintf(buf, sizeof buf, "%04lld-%02d-%02dT%02d:%02d:%02dZ", (long long)c.y, c.mo, c.d, c.h, c.mi, c.s);
  }
  return buf;
}

std::optional<int64_t> instant(const bj::value& v) {
  if (v.is_int64()) return v.as_int64();
  if (v.is_uint64() && v.as_uint64() <= uint64_t(INT64_MAX)) return int64_t(v.as_uint64());
  if (!v.is_string()) return std::nullopt;
  std::string t(v.as_string());
  // RFC 3339: a local part, then Z or an offset.
  int32_t offset = 0;
  std::string local;
  if (!t.empty() && (t.back() == 'Z' || t.back() == 'z')) {
    local = t.substr(0, t.size() - 1);
  } else if (t.size() > 6 && (t[t.size() - 6] == '+' || t[t.size() - 6] == '-') && t[t.size() - 3] == ':') {
    auto o = calendar::parse_offset(t.substr(t.size() - 6));
    if (!o) return std::nullopt;
    offset = *o;
    local = t.substr(0, t.size() - 6);
  } else {
    return std::nullopt;
  }
  int64_t ms = 0;
  if (auto dot = local.find('.'); dot != std::string::npos) {
    std::string frac = local.substr(dot + 1);
    local = local.substr(0, dot);
    frac = (frac + "000").substr(0, 3);
    for (char c : frac) {
      if (c < '0' || c > '9') return std::nullopt;
    }
    ms = std::stoi(frac);
  }
  bool date_only = false;
  auto c = calendar::parse_civil_text(local, &date_only);
  if (!c || date_only) return std::nullopt;
  return (calendar::civil_seconds(*c) - offset) * 1000 + ms;
}

int64_t instant_field(const bj::object& o, std::string_view k) {
  auto* v = o.if_contains(k);
  if (!v) bad(std::string(k) + " is required: unix milliseconds, or RFC 3339 text with Z or an offset");
  auto ms = instant(*v);
  if (!ms) bad(std::string(k) + " is unix milliseconds, or RFC 3339 text with Z or an offset");
  return *ms;
}

std::optional<calendar::Civil> local_of(const bj::value& v, bool* date_only) {
  *date_only = false;
  if (v.is_string()) return calendar::parse_civil_text(v.as_string(), date_only);
  if (!v.is_object()) return std::nullopt;
  const auto& o = v.as_object();
  auto* d = o.if_contains("date");
  if (!d || !d->is_string()) return std::nullopt;
  std::string t(d->as_string());
  if (auto* tm = o.if_contains("time"); tm && tm->is_string()) t += "T" + std::string(tm->as_string());
  return calendar::parse_civil_text(t, date_only);
}

bj::object at_zone(int64_t ms, const calendar::Zone& z) {
  auto info = z.at(ms);
  auto local = z.to_local(ms);
  return bj::object{{"ms", ms},
                    {"utc", utc_text(ms)},
                    {"local", calendar::civil_text(local)},
                    {"zone", z.name()},
                    {"offset", calendar::offset_text(info.offset_s)}};
}

bj::object schema(std::initializer_list<std::pair<const char*, bj::value>> props, std::initializer_list<const char*> required) {
  bj::object p;
  for (const auto& [k, v] : props) p[k] = v;
  bj::array r;
  for (const char* k : required) r.push_back(bj::value(k));
  return bj::object{{"type", "object"}, {"properties", std::move(p)}, {"required", std::move(r)}};
}

bj::value str(const char* d) { return bj::object{{"type", "string"}, {"description", d}}; }
bj::value num(const char* d) { return bj::object{{"type", "integer"}, {"description", d}}; }
bj::value any(const char* d) { return bj::object{{"description", d}}; }

}  // namespace goober::libraries::detail
