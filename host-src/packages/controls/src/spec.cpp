#include "goober/controls/spec.h"

#include <algorithm>
#include <cmath>

namespace goober::controls {

const char* mode_name(Mode m) { return m == Mode::kEnforce ? "enforce" : "observe"; }

const char* by_name(By b) {
  switch (b) {
    case By::kClient: return "client";
    case By::kUser: return "user";
    case By::kAll: return "all";
  }
  return "client";
}

const char* reason_name(Reason r) {
  switch (r) {
    case Reason::kRate: return "rate";
    case Reason::kConcurrent: return "concurrent";
    case Reason::kBody: return "body";
    case Reason::kClosed: return "closed";
    case Reason::kTimeout: return "timeout";
  }
  return "rate";
}

const char* property_name(Property p) {
  switch (p) {
    case Property::kRate: return "rate";
    case Property::kConcurrent: return "concurrent";
    case Property::kBodyMaxBytes: return "body-max-bytes";
    case Property::kTimeoutMs: return "timeout-ms";
    case Property::kClosed: return "closed";
  }
  return "rate";
}

const char* level_name(Level l) {
  switch (l) {
    case Level::kHost: return "host";
    case Level::kDomain: return "domain";
    case Level::kApi: return "api";
    case Level::kEndpoint: return "endpoint";
  }
  return "domain";
}

bool Props::named(Property p) const {
  switch (p) {
    case Property::kRate: return rate.named;
    case Property::kConcurrent: return concurrent.named;
    case Property::kBodyMaxBytes: return body_max_bytes.named;
    case Property::kTimeoutMs: return timeout_ms.named;
    case Property::kClosed: return closed.named;
  }
  return false;
}

bool Props::off(Property p) const {
  switch (p) {
    case Property::kRate: return rate.off;
    case Property::kConcurrent: return concurrent.off;
    case Property::kBodyMaxBytes: return body_max_bytes.off;
    case Property::kTimeoutMs: return timeout_ms.off;
    case Property::kClosed: return closed.off;
  }
  return false;
}

bool Props::any() const {
  return std::any_of(kProperties.begin(), kProperties.end(), [&](Property p) { return named(p); });
}

namespace {

bool is_off(const bj::value& v) { return v.is_string() && v.as_string() == "off"; }

std::optional<double> number(const bj::value& v) {
  if (v.is_int64()) return double(v.as_int64());
  if (v.is_uint64()) return double(v.as_uint64());
  if (v.is_double()) return v.as_double();
  return std::nullopt;
}

std::optional<int64_t> integer(const bj::value& v) {
  if (v.is_int64()) return v.as_int64();
  if (v.is_uint64() && v.as_uint64() <= uint64_t(INT64_MAX)) return int64_t(v.as_uint64());
  if (v.is_double() && std::floor(v.as_double()) == v.as_double() && std::fabs(v.as_double()) < 9e15) {
    return int64_t(v.as_double());
  }
  return std::nullopt;
}

bool fail(std::string* error, std::string text) {
  *error = std::move(text);
  return false;
}

std::string allowed_list(std::initializer_list<const char*> keys) {
  std::string out;
  for (const char* k : keys) out += std::string(out.empty() ? "" : ", ") + k;
  return out;
}

bool only_keys(const bj::object& o, std::initializer_list<const char*> keys, const std::string& where, std::string* error) {
  for (const auto& kv : o) {
    bool ok = false;
    for (const char* k : keys) ok = ok || kv.key() == k;
    if (!ok) return fail(error, where + ": unknown key " + std::string(kv.key()) + " (it takes " + allowed_list(keys) + ")");
  }
  return true;
}

bool one_line_text(std::string_view s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool is_identifier(std::string_view s, size_t max) {
  if (s.empty() || s.size() > max) return false;
  auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
  if (!alpha(s[0])) return false;
  for (char c : s) {
    if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
  }
  return true;
}

bool parse_rate(const bj::value& v, const std::string& where, Prop<Rate>* out, std::string* error) {
  if (!v.is_object()) return fail(error, where + " is an object like {\"per-minute\": 60}, or \"off\"");
  const auto& o = v.as_object();
  if (!only_keys(o, {"per-minute", "burst", "by"}, where, error)) return false;
  const auto* pm = o.if_contains("per-minute");
  auto per = pm ? number(*pm) : std::nullopt;
  if (!per) return fail(error, where + ".per-minute is required: a number of requests per minute");
  if (*per < kRateMinPerMinute || *per > kRateMaxPerMinute) {
    return fail(error, where + ".per-minute must be 0.1 to 1000000");
  }
  Rate r;
  r.per_minute = *per;
  r.burst = std::max(1.0, std::ceil(*per / 4));
  if (const auto* b = o.if_contains("burst")) {
    auto burst = integer(*b);
    if (!burst || *burst < 1 || *burst > 1'000'000) return fail(error, where + ".burst must be a whole number, 1 to 1000000");
    r.burst = double(*burst);
  }
  if (const auto* b = o.if_contains("by")) {
    std::string by = b->is_string() ? std::string(b->as_string()) : "";
    if (by == "client") r.by = By::kClient;
    else if (by == "user") r.by = By::kUser;
    else if (by == "all") r.by = By::kAll;
    else return fail(error, where + ".by must be client, user, or all");
  }
  out->value = r;
  return true;
}

bool parse_concurrent(const bj::value& v, const std::string& where, Prop<Concurrent>* out, std::string* error) {
  if (!v.is_object()) return fail(error, where + " is an object like {\"max\": 20, \"per-client\": 2}, or \"off\"");
  const auto& o = v.as_object();
  if (!only_keys(o, {"max", "per-client"}, where, error)) return false;
  Concurrent c;
  for (auto [key, field] : {std::pair{"max", &c.max}, std::pair{"per-client", &c.per_client}}) {
    const auto* x = o.if_contains(key);
    if (!x) continue;
    auto n = integer(*x);
    if (!n || *n < 1 || *n > kConcurrentMax) return fail(error, where + "." + key + " must be a whole number, 1 to 100000");
    *field = *n;
  }
  if (!c.max && !c.per_client) return fail(error, where + " needs max, per-client, or both");
  if (c.max && c.per_client > c.max) return fail(error, where + ".per-client can't be more than max");
  out->value = c;
  return true;
}

bool parse_closed(const bj::value& v, const std::string& where, Prop<Closed>* out, std::string* error) {
  Closed c;
  if (v.is_bool()) {
    if (!v.as_bool()) return fail(error, where + " is true, an object like {\"status\": 503}, or \"off\" (not false)");
    out->value = c;
    return true;
  }
  if (!v.is_object()) return fail(error, where + " is true, an object like {\"status\": 503, \"retry-after-s\": 600}, or \"off\"");
  const auto& o = v.as_object();
  if (!only_keys(o, {"status", "retry-after-s", "message"}, where, error)) return false;
  if (const auto* s = o.if_contains("status")) {
    auto n = integer(*s);
    if (!n || (*n != 503 && *n != 410 && *n != 451)) return fail(error, where + ".status must be 503, 410, or 451");
    c.status = int(*n);
  }
  if (const auto* r = o.if_contains("retry-after-s")) {
    auto n = integer(*r);
    if (!n || *n < 0 || *n > kRetryAfterMaxS) return fail(error, where + ".retry-after-s must be a whole number, 0 to 86400");
    c.retry_after_s = *n;
  }
  if (const auto* m = o.if_contains("message")) {
    if (!m->is_string()) return fail(error, where + ".message is a string");
    std::string text(m->as_string());
    if (text.size() > kMessageMaxBytes || !one_line_text(text)) {
      return fail(error, where + ".message is one line of at most 300 bytes");
    }
    c.message = std::move(text);
  }
  out->value = c;
  return true;
}

bool parse_bytes_or_ms(const bj::value& v, const std::string& where, int64_t host_max, const char* host_key, Prop<int64_t>* out,
                       std::string* error) {
  auto n = integer(v);
  if (!n || *n < 1 || *n > host_max) {
    return fail(error, where + " must be a whole number, 1 to the host's " + host_key + " (" + std::to_string(host_max) +
                           "), or \"off\": a control may only lower it");
  }
  out->value = *n;
  return true;
}

}  // namespace

std::optional<Props> parse_props(const bj::object& set, const HostLimits& limits, const std::string& where,
                                 std::string* error) {
  if (!only_keys(set, {"rate", "concurrent", "body-max-bytes", "timeout-ms", "closed"}, where, error)) return std::nullopt;
  Props p;
  for (const auto& kv : set) {
    std::string key(kv.key());
    std::string at = where + "." + key;
    const auto& v = kv.value();
    bool ok = true;
    auto mark = [&](auto& prop) {
      prop.named = true;
      prop.off = is_off(v);
      return prop.off;
    };
    if (key == "rate") {
      if (!mark(p.rate)) ok = parse_rate(v, at, &p.rate, error);
    } else if (key == "concurrent") {
      if (!mark(p.concurrent)) ok = parse_concurrent(v, at, &p.concurrent, error);
    } else if (key == "body-max-bytes") {
      if (!mark(p.body_max_bytes)) ok = parse_bytes_or_ms(v, at, limits.body_max_bytes, "limits.body-max-bytes", &p.body_max_bytes, error);
    } else if (key == "timeout-ms") {
      if (!mark(p.timeout_ms)) ok = parse_bytes_or_ms(v, at, limits.timeout_ms_max, "limits.timeout-ms.max", &p.timeout_ms, error);
    } else if (key == "closed") {
      if (!mark(p.closed)) ok = parse_closed(v, at, &p.closed, error);
    }
    if (!ok) return std::nullopt;
  }
  return p;
}

bj::value property_json(const Props& p, Property which) {
  if (!p.named(which)) return nullptr;
  if (p.off(which)) return "off";
  switch (which) {
    case Property::kRate: {
      const auto& r = p.rate.value;
      bj::object o{{"per-minute", r.per_minute}, {"burst", int64_t(r.burst)}, {"by", by_name(r.by)}};
      if (std::floor(r.per_minute) == r.per_minute) o["per-minute"] = int64_t(r.per_minute);
      return o;
    }
    case Property::kConcurrent: {
      bj::object o;
      if (p.concurrent.value.max) o["max"] = p.concurrent.value.max;
      if (p.concurrent.value.per_client) o["per-client"] = p.concurrent.value.per_client;
      return o;
    }
    case Property::kBodyMaxBytes: return p.body_max_bytes.value;
    case Property::kTimeoutMs: return p.timeout_ms.value;
    case Property::kClosed: {
      bj::object o{{"status", p.closed.value.status}};
      if (p.closed.value.retry_after_s >= 0) o["retry-after-s"] = p.closed.value.retry_after_s;
      if (!p.closed.value.message.empty()) o["message"] = p.closed.value.message;
      return o;
    }
  }
  return nullptr;
}

bj::object props_json(const Props& p) {
  bj::object out;
  for (auto which : kProperties) {
    if (p.named(which)) out[property_name(which)] = property_json(p, which);
  }
  return out;
}

std::optional<Pattern> Pattern::parse(std::string_view text, std::string* error) {
  Pattern p;
  p.text = std::string(text);
  auto space = text.find(' ');
  if (space == std::string_view::npos || text.find(' ', space + 1) != std::string_view::npos) {
    return *error = "match is \"METHOD /path\", like \"POST /cart/{id}\" (one space)", std::nullopt;
  }
  std::string_view method = text.substr(0, space), path = text.substr(space + 1);
  if (method == "*") {
    p.method.clear();
  } else {
    if (method.empty() || method.size() > 16) return *error = "match's method is uppercase, like GET, or *", std::nullopt;
    for (char c : method) {
      if (c < 'A' || c > 'Z') return *error = "match's method is uppercase, like GET, or *", std::nullopt;
    }
    for (char c : method) p.method += char(c - 'A' + 'a');
  }
  if (path.empty() || path[0] != '/') return *error = "match's path starts with /", std::nullopt;
  if (path.size() > 512) return *error = "match's path is at most 512 bytes", std::nullopt;
  if (path != "/") {
    std::string_view rest = path.substr(1);
    while (true) {
      auto slash = rest.find('/');
      std::string_view seg = rest.substr(0, slash);
      if (seg.empty()) return *error = "match's path has an empty segment (a // or a trailing /)", std::nullopt;
      if (!p.segments.empty() && p.segments.back().kind == Segment::Kind::kRest) {
        return *error = "match's {name*} may only be the last segment", std::nullopt;
      }
      Segment s;
      if (seg.front() == '{') {
        if (seg.back() != '}' || seg.size() < 3) return *error = "match's segment " + std::string(seg) + " isn't {name} or {name*}", std::nullopt;
        std::string_view name = seg.substr(1, seg.size() - 2);
        s.kind = Segment::Kind::kOne;
        if (name.back() == '*') {
          s.kind = Segment::Kind::kRest;
          name.remove_suffix(1);
        }
        if (!is_identifier(name, 64)) {
          return *error = "match's segment " + std::string(seg) + " names a parameter with letters, digits, and _", std::nullopt;
        }
        s.text = std::string(name);
        ++p.wildcards;
      } else {
        for (char c : seg) {
          if (c == '{' || c == '}' || c == '?' || c == '#' || static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) {
            return *error = "match's segment " + std::string(seg) + " holds a character a literal can't ({, }, ?, #, or a space)",
                   std::nullopt;
          }
        }
        s.text = std::string(seg);
        ++p.literals;
      }
      p.segments.push_back(std::move(s));
      if (p.segments.size() > 32) return *error = "match's path has at most 32 segments", std::nullopt;
      if (slash == std::string_view::npos) break;
      rest = rest.substr(slash + 1);
    }
  }
  return p;
}

bool Pattern::matches(std::string_view m, const std::vector<std::string>& path) const {
  if (!method.empty() && method != m && !(method == "get" && m == "head")) return false;
  size_t i = 0;
  for (const auto& s : segments) {
    if (s.kind == Segment::Kind::kRest) return true;  // the rest, possibly nothing
    if (i >= path.size()) return false;
    if (s.kind == Segment::Kind::kLiteral && s.text != path[i]) return false;
    ++i;
  }
  return i == path.size();
}

bool valid_override_id(std::string_view id, std::string* why) {
  if (!is_identifier(id, 64)) return *why = "an override's id is letters, digits, and _, not starting with a digit, at most 64", false;
  for (const char* taken : {"defaults", "content", "mcp", "identity"}) {
    if (id == taken) return *why = "\"" + std::string(id) + "\" names a control the host gives; choose another id", false;
  }
  return true;
}

bool valid_api(std::string_view api, std::string* why) {
  if (api == "content" || api == "mcp" || api == "identity") return true;
  if (api.starts_with("box:")) {
    std::string_view box = api.substr(4);
    bool ok = !box.empty() && box.size() <= 63 && box[0] >= 'a' && box[0] <= 'z';
    for (char c : box) ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
    if (ok) return true;
  }
  return *why = "an API is box:<box name>, content, mcp, or identity", false;
}

std::optional<Control> parse_host_defaults(const bj::object& defaults, const HostLimits& limits, std::string* error) {
  bj::object set = defaults;
  Control c;
  c.level = Level::kHost;
  c.id = "defaults";
  if (const auto* m = set.if_contains("mode")) {
    std::string mode = m->is_string() ? std::string(m->as_string()) : "";
    if (mode != "enforce" && mode != "observe") return *error = "controls.defaults.mode must be enforce or observe", std::nullopt;
    c.mode = mode == "enforce" ? Mode::kEnforce : Mode::kObserve;
    set.erase("mode");
  }
  auto p = parse_props(set, limits, "controls.defaults", error);
  if (!p) return std::nullopt;
  c.props = std::move(*p);
  return c;
}

DomainControls::Chain DomainControls::chain(std::string_view method, const std::vector<std::string>& path,
                                            std::string_view api) const {
  Chain c{host ? &*host : nullptr, defaults ? &*defaults : nullptr, nullptr, nullptr};
  if (!api.empty()) {
    if (auto it = apis.find(api); it != apis.end()) c[2] = &it->second;
  }
  for (const auto& e : endpoints) {
    if (!e.api.empty() && e.api != api) continue;
    if (e.match->matches(method, path)) {
      c[3] = &e;
      break;
    }
  }
  return c;
}

const Control* DomainControls::find(std::string_view id) const {
  if (id == "defaults") return defaults ? &*defaults : (host ? &*host : nullptr);
  if (auto it = apis.find(id); it != apis.end()) return &it->second;
  for (const auto& e : endpoints) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

namespace {

bool parse_entry(const bj::value& v, Level level, const std::string& where, const HostLimits& limits, Control* out,
                 std::string* error) {
  if (!v.is_object()) return fail(error, where + " is an object");
  const auto& o = v.as_object();
  if (level == Level::kEndpoint) {
    if (!only_keys(o, {"id", "api", "match", "mode", "set", "history"}, where, error)) return false;
  } else if (!only_keys(o, {"mode", "set", "history"}, where, error)) {
    return false;
  }
  out->level = level;
  const auto* m = o.if_contains("mode");
  std::string mode = m && m->is_string() ? std::string(m->as_string()) : "";
  if (mode != "enforce" && mode != "observe") return fail(error, where + ".mode must be enforce or observe");
  out->mode = mode == "enforce" ? Mode::kEnforce : Mode::kObserve;
  const auto* set = o.if_contains("set");
  if (set && !set->is_object()) return fail(error, where + ".set is an object");
  auto p = parse_props(set ? set->as_object() : bj::object{}, limits, where + ".set", error);
  if (!p) return false;
  out->props = std::move(*p);
  if (level == Level::kEndpoint) {
    const auto* id = o.if_contains("id");
    std::string why;
    if (!id || !id->is_string() || !valid_override_id(id->as_string(), &why)) {
      return fail(error, where + ".id: " + (why.empty() ? "an override needs an id" : why));
    }
    out->id = std::string(id->as_string());
    if (const auto* a = o.if_contains("api")) {
      if (!a->is_string() || !valid_api(a->as_string(), &why)) return fail(error, where + ".api: " + why);
      out->api = std::string(a->as_string());
    }
    const auto* mt = o.if_contains("match");
    if (!mt || !mt->is_string()) return fail(error, where + ".match is required, like \"POST /cart/{id}\"");
    auto pat = Pattern::parse(mt->as_string(), &why);
    if (!pat) return fail(error, where + ".match: " + why);
    out->match = std::move(*pat);
  }
  return true;
}

}  // namespace

std::optional<DomainControls> compile(const bj::object& file, const std::string& domain, const std::string& tenant,
                                      const std::optional<Control>& host, const HostLimits& limits, std::string* error) {
  if (!only_keys(file, {"defaults", "apis", "endpoints", "removed"}, "controls for " + domain, error)) return std::nullopt;
  DomainControls d;
  d.domain = domain;
  d.tenant = tenant;
  d.host = host;
  if (const auto* v = file.if_contains("defaults")) {
    Control c;
    c.id = "defaults";
    if (!parse_entry(*v, Level::kDomain, "defaults", limits, &c, error)) return std::nullopt;
    d.defaults = std::move(c);
  }
  if (const auto* v = file.if_contains("apis")) {
    if (!v->is_object()) return *error = "apis is an object of API controls", std::nullopt;
    if (v->as_object().size() > kApisMax) return *error = "at most 50 API controls a domain", std::nullopt;
    for (const auto& kv : v->as_object()) {
      std::string why;
      if (!valid_api(kv.key(), &why)) return *error = "apis." + std::string(kv.key()) + ": " + why, std::nullopt;
      Control c;
      c.id = std::string(kv.key());
      c.api = c.id;
      if (!parse_entry(kv.value(), Level::kApi, "apis." + c.id, limits, &c, error)) return std::nullopt;
      d.api_scoped = true;
      d.apis.emplace(c.id, std::move(c));
    }
  }
  if (const auto* v = file.if_contains("endpoints")) {
    if (!v->is_array()) return *error = "endpoints is a list of overrides", std::nullopt;
    if (v->as_array().size() > kEndpointsMax) return *error = "at most 100 endpoint overrides a domain", std::nullopt;
    size_t i = 0;
    for (const auto& e : v->as_array()) {
      Control c;
      if (!parse_entry(e, Level::kEndpoint, "endpoints[" + std::to_string(i) + "]", limits, &c, error)) return std::nullopt;
      c.order = i++;
      for (const auto& other : d.endpoints) {
        if (other.id == c.id) return *error = "two overrides have the id " + c.id, std::nullopt;
      }
      d.api_scoped |= !c.api.empty();
      d.endpoints.push_back(std::move(c));
    }
    // Most specific first: more literal segments, then fewer wildcards, then a named method over
    // *, then the order they were listed in.
    std::stable_sort(d.endpoints.begin(), d.endpoints.end(), [](const Control& a, const Control& b) {
      const auto &pa = *a.match, &pb = *b.match;
      if (pa.literals != pb.literals) return pa.literals > pb.literals;
      if (pa.wildcards != pb.wildcards) return pa.wildcards < pb.wildcards;
      if (pa.method.empty() != pb.method.empty()) return !pa.method.empty();
      return a.order < b.order;
    });
  }
  return d;
}

Effective effective(const DomainControls::Chain& chain, Property p) {
  Effective e;
  for (size_t i = chain.size(); i-- > 0;) {
    const Control* c = chain[i];
    if (!c || !c->props.named(p)) continue;
    if (c->mode == Mode::kObserve) {
      if (!e.observed) e.observed = c;
      continue;
    }
    e.enforced = c;
    break;
  }
  return e;
}

}  // namespace goober::controls
