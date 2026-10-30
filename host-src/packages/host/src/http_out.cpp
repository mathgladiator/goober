// Speaking HTTP (design/machine/034.SpeakingHttp.md): a service described by a pinned OpenAPI
// spec and small adapters. This builds each request (the spec's operation, the call's path,
// query, and headers, the manifest's pins and constant headers, a json, form, multipart, or raw
// body), applies the secret (a key, or a sealed value the box keeps, refreshed when it's an OAuth
// 2 connection), sends it, and reads what came back (rate limits, Retry-After, errors on 200).
// In the outbox, the same element runs a chunked upload and polls an async job.

#include <algorithm>
#include <charconv>
#include <iostream>
#include <map>
#include <mutex>

#include "goober/backup/backup.h"
#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/signing/oauth1.h"
#include "goober/specs/specs.h"
#include "goober/sqlite/sqlite.h"
#include "goober/vault/vault.h"
#include "goober/web/url.h"
#include "keys.h"
#include "network.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxBody = 64u << 20;
constexpr int64_t kMaxChunks = 1000;
constexpr int64_t kMaxWaitMs = 24 * 3600 * 1000;  // Retry-After and resets past this are capped
constexpr int64_t kMaxExpiresInS = 10LL * 365 * 24 * 3600;

// A header's whole-number seconds, or nullopt: digits only, and small enough to mean something.
std::optional<int64_t> header_seconds(std::string_view s) {
  int64_t v = 0;
  if (s.empty() || s.size() > 12) return std::nullopt;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size() || v < 0) return std::nullopt;
  return v;
}

// An expires_in from a token endpoint, in ms from now; nullopt when absent or senseless.
std::optional<int64_t> expires_in_ms(const bj::object& reply) {
  auto* ei = reply.if_contains("expires_in");
  double d = 0;
  if (ei && ei->is_number()) {
    d = ei->to_number<double>();
  } else if (ei && ei->is_string()) {
    auto s = header_seconds(std::string(ei->as_string()));
    if (!s) return std::nullopt;
    d = double(*s);
  } else {
    return std::nullopt;
  }
  if (!(d > 0) || d > double(kMaxExpiresInS)) return std::nullopt;
  return int64_t(d * 1000);
}

std::string text_of(const bj::value& v) {
  if (v.is_string()) return std::string(v.as_string());
  if (v.is_null()) return "";
  return json::dump(v);
}

// The value at a dotted path ("body.data.id", "body.items.0.id") in an answer.
const bj::value* at_path(const bj::value& root, std::string_view path) {
  const bj::value* cur = &root;
  size_t i = 0;
  while (i <= path.size() && cur) {
    size_t dot = path.find('.', i);
    if (dot == std::string_view::npos) dot = path.size();
    std::string seg(path.substr(i, dot - i));
    if (cur->is_object()) {
      cur = cur->as_object().if_contains(seg);
    } else if (cur->is_array() && !seg.empty() && std::all_of(seg.begin(), seg.end(), ::isdigit)) {
      size_t n = std::stoul(seg);
      cur = n < cur->as_array().size() ? &cur->as_array()[n] : nullptr;
    } else {
      return nullptr;
    }
    i = dot + 1;
  }
  return cur;
}

bool holds(const manifest::Condition& c, const bj::value& answer) {
  if (c.empty()) return false;
  const bj::value* v = at_path(answer, c.at);
  if (c.present) return v && !v->is_null();
  if (!v) return false;
  std::string err;
  auto want = json::parse(c.equals, &err);
  return want && json::dump(*want) == json::dump(*v);
}

// {name} placeholders: each replaced by lookup(name), or left as written.
std::string fill(const std::string& tmpl, const std::function<std::optional<std::string>(const std::string&)>& lookup) {
  std::string out;
  for (size_t i = 0; i < tmpl.size(); ++i) {
    if (tmpl[i] == '{') {
      auto close = tmpl.find('}', i);
      if (close != std::string::npos) {
        if (auto v = lookup(tmpl.substr(i + 1, close - i - 1))) {
          out += *v;
          i = close;
          continue;
        }
      }
    }
    out += tmpl[i];
  }
  return out;
}

void form_pairs(const std::string& prefix, const bj::value& v, const std::string& arrays,
                std::vector<std::pair<std::string, std::string>>* out) {
  if (v.is_object()) {
    for (const auto& kv : v.as_object()) {
      form_pairs(prefix.empty() ? std::string(kv.key()) : prefix + "[" + std::string(kv.key()) + "]", kv.value(), arrays, out);
    }
  } else if (v.is_array()) {
    for (const auto& e : v.as_array()) form_pairs(arrays == "repeat" ? prefix : prefix + "[]", e, arrays, out);
  } else if (!v.is_null()) {
    out->emplace_back(prefix, v.is_bool() ? (v.as_bool() ? "true" : "false") : text_of(v));
  }
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::string out;
  for (const auto& [k, v] : pairs) out += (out.empty() ? "" : "&") + signing::rfc3986(k) + "=" + signing::rfc3986(v);
  return out;
}

struct Part {
  std::string name, filename, content_type, data;
};

std::string multipart(const std::vector<Part>& parts, std::string* content_type) {
  uint8_t rnd[12];
  base::os_random_fill(rnd);
  std::string boundary = "goober-" + base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
  std::string out;
  for (const auto& p : parts) {
    out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + p.name + "\"";
    if (!p.filename.empty()) {
      // A file's name is a person's text: quotes, backslashes, and controls would reshape the header.
      std::string safe;
      for (char ch : p.filename) {
        if (ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) safe += '_';
        else safe += ch;
      }
      out += "; filename=\"" + safe + "\"";
    }
    out += "\r\n";
    if (!p.content_type.empty()) out += "Content-Type: " + p.content_type + "\r\n";
    out += "\r\n" + p.data + "\r\n";
  }
  out += "--" + boundary + "--\r\n";
  *content_type = "multipart/form-data; boundary=" + boundary;
  return out;
}

bool safe_part_name(std::string_view s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    if (c == '"' || c == '\r' || c == '\n' || c == '\\') return false;
  }
  return true;
}

}  // namespace

bool Host::uses_httpx(const manifest::ExternalService& s, const manifest::ExternalMethod& m) {
  return !s.spec.empty() || !s.auth_kind.empty() || s.sealed || !s.headers.empty() || s.any_public || !m.op.empty() ||
         !m.body.empty() || !m.upload.empty() || m.polls || !m.error_when.empty();
}

std::optional<vault::Key> Host::vault_tenant_key(const std::string& tenant, std::string* error) {
  std::lock_guard lock(vault_mu_);
  auto hex = keys_->resolve("keys/system/vault_key", "");
  vault::Key root{};
  if (!hex) {
    // Made once (034 § 4): back it up beside the backup key (goober-host vault key).
    base::os_random_fill(root);
    std::string h = base::hex_encode(std::string_view(reinterpret_cast<char*>(root.data()), root.size()));
    if (!keys_->set("system", "", "vault_key", h, base::now_unix_ms(), error)) return std::nullopt;
  } else {
    auto parsed = backup::parse_key(*hex);
    if (!parsed) return *error = "keys/system/vault_key isn't 64 hex digits", std::nullopt;
    root = *parsed;
  }
  return vault::tenant_key(root, tenant);
}

std::string Host::seal_secret(const std::string& tenant, const std::string& service, const std::string& owner,
                              const std::string& plaintext, std::string* error) {
  auto key = vault_tenant_key(tenant, error);
  if (!key) return "";
  return vault::seal_for(*key, tenant, service, owner, plaintext);
}

sqlite::Db& Host::connections_db() {
  std::lock_guard lock(vault_mu_);
  if (!connections_db_) {
    auto dir = config_.get()->storage / "connections";
    fs::create_directories(dir);
    connections_db_ = std::make_unique<sqlite::Db>(dir / "state.db");
    connections_db_->exec(
        "CREATE TABLE IF NOT EXISTS latest (tenant TEXT NOT NULL, service TEXT NOT NULL, id TEXT NOT NULL, gen INTEGER NOT "
        "NULL, sealed TEXT NOT NULL, dead INTEGER NOT NULL DEFAULT 0, at_ms INTEGER NOT NULL, PRIMARY KEY (tenant, service, id));"
        "CREATE TABLE IF NOT EXISTS states (state TEXT PRIMARY KEY, tenant TEXT NOT NULL, box TEXT NOT NULL, service TEXT NOT "
        "NULL, user_id TEXT NOT NULL, return_path TEXT NOT NULL, ref TEXT NOT NULL, verifier TEXT NOT NULL, domain TEXT NOT "
        "NULL, expires_ms INTEGER NOT NULL);");
  }
  return *connections_db_;
}

std::optional<Host::Secret> Host::open_secret(const std::string& tenant, const manifest::ExternalService& svc,
                                              const std::string& sealed, std::string* resealed, std::string* error) {
  auto key = vault_tenant_key(tenant, error);
  if (!key) return std::nullopt;
  auto opened = vault::open_for(*key, tenant, svc.name, sealed);
  if (!opened) return *error = "the credential doesn't open for this service (it's sealed for another, or altered)", std::nullopt;
  std::string err;
  auto v = json::parse(opened->plaintext, &err);
  if (!v || !v->is_object()) return *error = "the credential is unreadable", std::nullopt;
  bj::object o = v->as_object();
  Secret s;
  s.owner = opened->owner;
  s.kind = o.contains("kind") && o.at("kind").is_string() ? std::string(o.at("kind").as_string()) : "";
  s.origin = o.contains("origin") && o.at("origin").is_string() ? std::string(o.at("origin").as_string()) : "";
  auto field = [](const bj::object& x, const char* k) {
    auto* f = x.if_contains(k);
    return f && f->is_string() ? std::string(f->as_string()) : std::string();
  };
  if (s.kind == "key") {
    s.value = field(o, "value");
    return s;
  }
  if (s.kind == "oauth1") {
    s.token = field(o, "token");
    s.token_secret = field(o, "token_secret");
    return s;
  }
  if (s.kind != "oauth2") return *error = "the credential is of no kind this host knows", std::nullopt;
  // The newest tokens the host has seen for this connection win over an older copy (034 § 5).
  std::string id = field(o, "id");
  int64_t gen = o.contains("gen") && o.at("gen").is_int64() ? o.at("gen").as_int64() : 0;
  auto load_latest = [&](bj::object* into, int64_t* into_gen) -> std::optional<std::string> {
    sqlite::Db& db = connections_db();
    std::lock_guard lock(vault_mu_);
    sqlite::Stmt q(db, "SELECT gen, sealed, dead FROM latest WHERE tenant = ? AND service = ? AND id = ?");
    q.bind(1, tenant).bind(2, svc.name).bind(3, id);
    if (!q.step()) return std::nullopt;
    if (q.int64(2)) return std::string("dead");
    if (q.int64(0) > *into_gen) {
      if (auto newer = vault::open_for(*key, tenant, svc.name, q.text(1))) {
        std::string e2;
        if (auto nv = json::parse(newer->plaintext, &e2); nv && nv->is_object()) {
          *into = nv->as_object();
          *into_gen = q.int64(0);
          return q.text(1);
        }
      }
    }
    return std::nullopt;
  };
  if (auto latest = load_latest(&o, &gen); latest && *latest == "dead") {
    return *error = "connection_expired: the person must connect " + svc.name + " again", std::nullopt;
  } else if (latest) {
    *resealed = *latest;
  }
  int64_t expires = o.contains("expires_at") && o.at("expires_at").is_int64() ? o.at("expires_at").as_int64() : 0;
  std::string refresh = field(o, "refresh_token");
  int64_t now = base::now_unix_ms();
  if (expires && expires - svc.connect.early_ms <= now && !refresh.empty() && svc.connect.kind == "oauth2") {
    // One refresh at a time per connection: a rotating refresh token can be spent once.
    std::mutex* m;
    {
      std::lock_guard lock(vault_mu_);
      m = &refresh_locks_[tenant + "\n" + svc.name + "\n" + id];
    }
    std::lock_guard one(*m);
    if (auto latest = load_latest(&o, &gen); latest && *latest == "dead") {
      return *error = "connection_expired: the person must connect " + svc.name + " again", std::nullopt;
    } else if (latest) {
      *resealed = *latest;
    }
    expires = o.contains("expires_at") && o.at("expires_at").is_int64() ? o.at("expires_at").as_int64() : 0;
    refresh = field(o, "refresh_token");
    if (expires && expires - svc.connect.early_ms <= now) {
      auto reply = oauth2_token(tenant, svc, {{"grant_type", "refresh_token"}, {"refresh_token", refresh}}, error);
      if (!reply) {
        if (error->find("invalid_grant") != std::string::npos) {
          sqlite::Db& db = connections_db();
          std::lock_guard lock(vault_mu_);
          sqlite::Stmt d(db, "INSERT INTO latest (tenant, service, id, gen, sealed, dead, at_ms) VALUES (?, ?, ?, ?, '', 1, ?) "
                             "ON CONFLICT (tenant, service, id) DO UPDATE SET dead = 1, at_ms = excluded.at_ms");
          d.bind(1, tenant).bind(2, svc.name).bind(3, id).bind(4, gen).bind(5, now).run();
          *error = "connection_expired: the person must connect " + svc.name + " again";
        }
        return std::nullopt;
      }
      // A 200 without a token (Slack answers ok: false) is a failed refresh, not an empty token.
      if (field(*reply, "access_token").empty()) {
        return *error = "the token endpoint gave no access token when refreshing", std::nullopt;
      }
      o["access_token"] = field(*reply, "access_token");
      if (auto nr = field(*reply, "refresh_token"); !nr.empty()) o["refresh_token"] = nr;  // rotated
      // No expires_in: the token doesn't say it expires, so it isn't refreshed again until it does.
      if (auto ms = expires_in_ms(*reply)) o["expires_at"] = now + *ms;
      else o.erase("expires_at");
      o["gen"] = gen + 1;
      std::string next = vault::seal_for(*key, tenant, svc.name, s.owner, json::dump(o));
      {
        sqlite::Db& db = connections_db();
        std::lock_guard lock(vault_mu_);
        sqlite::Stmt u(db, "INSERT INTO latest (tenant, service, id, gen, sealed, dead, at_ms) VALUES (?, ?, ?, ?, ?, 0, ?) "
                           "ON CONFLICT (tenant, service, id) DO UPDATE SET gen = excluded.gen, sealed = excluded.sealed, "
                           "dead = 0, at_ms = excluded.at_ms");
        u.bind(1, tenant).bind(2, svc.name).bind(3, id).bind(4, gen + 1).bind(5, next).bind(6, now).run();
      }
      *resealed = next;
    }
  }
  s.value = field(o, "access_token");
  if (s.value.empty()) return *error = "the connection holds no access token", std::nullopt;
  return s;
}

std::optional<bj::object> Host::oauth2_token(const std::string& tenant, const manifest::ExternalService& svc,
                                             std::vector<std::pair<std::string, std::string>> form, std::string* error) {
  const auto& c = svc.connect;
  std::string why;
  auto id = keys_->resolve_for(c.client_id, tenant, c.token, &why);
  if (!id) return *error = "the OAuth client id: " + why, std::nullopt;
  std::optional<std::string> secret;
  if (!c.client_secret.empty()) {
    secret = keys_->resolve_for(c.client_secret, tenant, c.token, &why);
    if (!secret) return *error = "the OAuth client secret: " + why, std::nullopt;
  }
  std::vector<std::pair<std::string, std::string>> headers{{"accept", "application/json"}};
  if (c.client_auth == "basic" && secret) {
    headers.emplace_back("authorization", "Basic " + base::base64_encode(signing::rfc3986(*id) + ":" + signing::rfc3986(*secret)));
  } else {
    form.emplace_back("client_id", *id);
    if (secret) form.emplace_back("client_secret", *secret);
  }
  auto cfg = config_.get();
  auto r = send_http("post", c.token, headers, form_encode(form), "application/x-www-form-urlencoded", cfg->local,
                     cfg->storage / "tmp", 15000);
  egress_log(tenant, "", svc.name, "oauth2.token", "POST", c.token, r.status, r.error, 0, 0, r.body.size(), "", 0);
  if (!r.error.empty()) return *error = r.error, std::nullopt;
  std::string err;
  auto v = json::parse(r.body, &err);
  if (r.status != 200 || !v || !v->is_object()) {
    std::string code = v && v->is_object() && v->as_object().contains("error") ? text_of(v->as_object().at("error")) : "";
    return *error = "the token endpoint answered " + std::to_string(r.status) + (code.empty() ? "" : " " + code), std::nullopt;
  }
  return v->as_object();
}

std::optional<Host::HttpxCall> Host::build_httpx(const std::string& tenant, const bj::value& user, const std::string& domain,
                                                 const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                                                 const std::string& op_ref, const bj::object& request,
                                                 const std::string& body_mode, std::string* error) {
  HttpxCall call;
  std::string path_template = m.path;
  call.verb = m.verb;
  std::vector<specs::Param> params;
  std::vector<std::string> body_types;
  std::string ref = op_ref.empty() ? m.op : op_ref;
  if (!ref.empty()) {
    auto spec = specs_->get(svc.spec, error);
    if (!spec) return std::nullopt;
    const auto* op = spec->find(ref);
    if (!op) return *error = svc.spec + " has no operation " + ref, std::nullopt;
    call.verb = op->verb;
    path_template = op->path;
    params = op->params;
    body_types = op->body_types;
  }
  bj::object path_params, query, box_headers;
  bj::value body = nullptr;
  if (auto* p = request.if_contains("path"); p && p->is_object()) path_params = p->as_object();
  if (auto* q = request.if_contains("query"); q && q->is_object()) query = q->as_object();
  if (auto* h = request.if_contains("headers"); h && h->is_object()) box_headers = h->as_object();
  if (auto* b = request.if_contains("body")) body = *b;
  // The manifest's pins (015 § 1 require) win over the box's own values.
  for (const auto& [field, value] : m.require) {
    std::string v = value;
    if (value == "{!email}" || value == "{!user_id}") {
      const char* k = value == "{!email}" ? "email" : "user_id";
      if (!user.is_object() || !user.as_object().contains(k)) {
        return *error = "this call pins " + field + " to the signed-in person, and nobody is signed in", std::nullopt;
      }
      v = text_of(user.as_object().at(k));
    } else if (value == "{!tenant}") {
      v = tenant;
    } else if (value == "{!domain}") {
      v = domain;
    }
    auto dot = field.find('.');
    std::string part = field.substr(0, dot), name = field.substr(dot + 1);
    if (part == "path") path_params[name] = v;
    if (part == "query") query[name] = v;
    if (part == "header") box_headers[base::to_lower(name)] = v;
    if (part == "body") {
      if (body.is_null()) body = bj::object{};
      if (!body.is_object()) return *error = "this call pins " + field + ", so the body must be an object", std::nullopt;
      bj::object* at = &body.as_object();
      size_t start = 0;
      for (size_t d = name.find('.'); d != std::string::npos; d = name.find('.', start)) {
        auto& next = (*at)[name.substr(start, d - start)];
        if (!next.is_object()) next = bj::object{};
        at = &next.as_object();
        start = d + 1;
      }
      (*at)[name.substr(start)] = v;
    }
  }
  // The URL: the base and the path with each parameter encoded, or the call's own for any-public.
  if (m.any_public) {
    auto* u = request.if_contains("url");
    std::string url = u && u->is_string() ? std::string(u->as_string()) : "";
    if (!url.starts_with("https://") || url.size() > 2048 || url.find_first_of(" \r\n\\@") != std::string::npos) {
      if (!(config_.get()->local && url.starts_with("http://127.0.0.1:"))) {
        return *error = "this method takes the whole URL as url: an https address with no user part", std::nullopt;
      }
    }
    call.url = url;
  } else {
    std::string path;
    for (size_t i = 0; i < path_template.size(); ++i) {
      if (path_template[i] != '{') {
        path += path_template[i];
        continue;
      }
      size_t close = path_template.find('}', i);
      if (close == std::string::npos) return *error = "the path " + path_template + " has an unclosed {", std::nullopt;
      std::string name = path_template.substr(i + 1, close - i - 1);
      auto* v = path_params.if_contains(name);
      if (!v || v->is_null()) return *error = "the call needs path." + name, std::nullopt;
      std::string t = text_of(*v);
      if (t.empty() || t == "." || t == "..") return *error = "path." + name + " can't be empty, . or ..", std::nullopt;
      path += base::percent_encode(t);
      i = close;
    }
    call.url = svc.base + path;
  }
  std::map<std::string, std::string> sorted;
  for (const auto& kv : query) sorted[std::string(kv.key())] = text_of(kv.value());
  bool first = call.url.find('?') == std::string::npos;
  for (const auto& [k, v] : sorted) {
    call.url += (first ? "?" : "&") + signing::rfc3986(k) + "=" + signing::rfc3986(v);
    first = false;
  }
  // Headers: the service's constants, then those the box may set (the spec's header parameters).
  for (const auto& [k, v] : svc.headers) call.headers.emplace_back(k, v);
  for (const auto& kv : box_headers) {
    std::string name = base::to_lower(kv.key());
    bool declared = std::any_of(params.begin(), params.end(), [&](const specs::Param& p) {
      return p.in == "header" && base::to_lower(p.name) == name;
    });
    bool pinned = m.require.count("header." + name) || m.require.count("header." + std::string(kv.key()));
    if (!declared && !pinned) return *error = "header " + name + " isn't a header parameter of this operation", std::nullopt;
    if (name == "authorization" || name == "host" || name.starts_with("content-")) return *error = "header " + name + " is the host's", std::nullopt;
    std::string v = text_of(kv.value());
    if (v.find_first_of("\r\n") != std::string::npos) return *error = "a header value is one line", std::nullopt;
    call.headers.emplace_back(name, v);
  }
  // The body, in the method's encoding, else the spec's, else JSON.
  std::string mode = body_mode.empty() ? m.body : body_mode;
  if (mode.empty() && !body_types.empty()) {
    auto has = [&](std::string_view t) {
      return std::any_of(body_types.begin(), body_types.end(), [&](const std::string& b) { return b.starts_with(t); });
    };
    mode = has("application/json") ? "json" : has("multipart/form-data") ? "multipart"
           : has("application/x-www-form-urlencoded")                    ? "form"
                                                                         : "json";
  }
  if (mode.empty()) mode = "json";
  if (!body.is_null()) {
    if (mode == "json") {
      if (body.is_string()) {
        call.body = std::string(body.as_string());
        call.content_type = "text/plain; charset=utf-8";
      } else {
        call.body = json::dump(body);
        call.content_type = "application/json";
      }
    } else if (mode == "form") {
      if (!body.is_object()) return *error = "a form body is an object", std::nullopt;
      form_pairs("", body, m.form_arrays, &call.form);
      call.body = form_encode(call.form);
      call.content_type = "application/x-www-form-urlencoded";
    } else if (mode == "multipart") {
      if (!body.is_object()) return *error = "a multipart body is an object of fields", std::nullopt;
      std::vector<Part> parts;
      for (const auto& kv : body.as_object()) {
        if (!safe_part_name(kv.key())) return *error = "a part's name can't hold quotes or line breaks", std::nullopt;
        const auto& v = kv.value();
        if (v.is_object() && v.as_object().contains("attachment")) {
          auto file = attachment_bytes(tenant, v.as_object().at("attachment"), error);
          if (!file) return std::nullopt;
          parts.push_back(Part{std::string(kv.key()), file->filename, file->content_type, std::move(file->bytes)});
        } else if (v.is_object() && v.as_object().contains("json")) {
          parts.push_back(Part{std::string(kv.key()), "", "application/json", json::dump(v.as_object().at("json"))});
        } else if (v.is_array()) {
          for (const auto& e : v.as_array()) parts.push_back(Part{std::string(kv.key()), "", "", text_of(e)});
        } else {
          parts.push_back(Part{std::string(kv.key()), "", "", v.is_bool() ? (v.as_bool() ? "true" : "false") : text_of(v)});
        }
      }
      call.body = multipart(parts, &call.content_type);
    } else if (mode == "raw") {
      if (!body.is_object() || !body.as_object().contains("attachment")) return *error = "a raw body is {\"attachment\": id}", std::nullopt;
      auto file = attachment_bytes(tenant, body.as_object().at("attachment"), error);
      if (!file) return std::nullopt;
      call.body = std::move(file->bytes);
      call.content_type = file->content_type;
    }
  }
  if (call.body.size() > kMaxBody) return *error = "the body is past 64 MiB", std::nullopt;
  return call;
}

std::optional<Host::FileBytes> Host::attachment_bytes(const std::string& tenant, const bj::value& id_value, std::string* error) {
  int64_t id = id_value.is_int64() ? id_value.as_int64() : id_value.is_string() ? std::atoll(std::string(id_value.as_string()).c_str()) : 0;
  auto meta = id > 0 ? attachments_->get(tenant, id) : std::nullopt;
  if (!meta || meta->state != "stored") return *error = "attachment " + text_of(id_value) + " isn't one of this tenant's", std::nullopt;
  if (meta->size > int64_t(kMaxBody)) return *error = "attachment " + text_of(id_value) + " is past 64 MiB", std::nullopt;
  auto bytes = attachments_->read(tenant, id);
  if (!bytes) return *error = "attachment " + text_of(id_value) + " can't be read", std::nullopt;
  FileBytes f;
  f.filename = meta->name.empty() ? "attachment-" + std::to_string(id) + "." + meta->ext : meta->name;
  f.content_type = meta->content_type;
  f.bytes = std::move(*bytes);
  return f;
}

bool Host::apply_secret(const std::string& tenant, const manifest::ExternalService& svc, const Secret& secret, HttpxCall* call,
                        std::string* error) {
  // A sealed secret goes only to the origin it was made for (the service's base when the person
  // connected), whatever the box's call says or a later manifest's base becomes.
  if (svc.sealed) {
    auto u = web::parse_url(call->url);
    if (secret.origin.empty() || !u || u->origin() != secret.origin) {
      return *error = "the credential is for " + (secret.origin.empty() ? std::string("no recorded origin") : secret.origin) +
                      ", not this URL's; the person must connect " + svc.name + " again",
             false;
    }
  }
  const std::string& v = secret.value;
  const std::string& kind = svc.auth_kind;
  if (kind.empty() || kind == "header" || kind == "bearer") {
    std::string name = svc.auth_header.empty() ? "authorization" : svc.auth_header;
    std::string prefix = kind.empty() ? svc.auth_prefix : kind == "bearer" ? "Bearer " : svc.auth_prefix;
    call->headers.emplace_back(name, prefix + v);
  } else if (kind == "basic") {
    call->headers.emplace_back("authorization", "Basic " + base::base64_encode(svc.basic_user.empty() ? v : svc.basic_user + ":" + v));
  } else if (kind == "query") {
    call->url += (call->url.find('?') == std::string::npos ? "?" : "&") + signing::rfc3986(svc.auth_header) + "=" + signing::rfc3986(v);
  } else if (kind == "oauth1") {
    std::string why;
    auto ck = keys_->resolve_for(svc.oauth1_consumer_key, tenant, call->url, &why);
    auto cs = keys_->resolve_for(svc.oauth1_consumer_secret, tenant, call->url, &why);
    if (!ck || !cs) return *error = "the OAuth 1.0a consumer key: " + why, false;
    uint8_t rnd[16];
    base::os_random_fill(rnd);
    signing::OAuth1 o{*ck, *cs, secret.token, secret.token_secret,
                      base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd)),
                      std::to_string(base::now_unix_ms() / 1000)};
    call->headers.emplace_back("authorization", signing::oauth1_header(call->verb, call->url, call->form, o));
  }
  return true;
}

std::optional<Host::Secret> Host::secret_for(const std::string& tenant, const manifest::ExternalService& svc,
                                             const bj::object& request, const std::string& url, std::string* resealed,
                                             std::string* error) {
  if (svc.sealed) {
    auto* c = request.if_contains("credential");
    if (!c || !c->is_string()) return *error = "this service's calls carry a credential: a sealed value from a connection", std::nullopt;
    return open_secret(tenant, svc, std::string(c->as_string()), resealed, error);
  }
  if (svc.auth_kind == "oauth1" && !svc.oauth1_token.empty()) {
    std::string why;
    auto t = keys_->resolve_for(svc.oauth1_token, tenant, url, &why);
    auto ts = keys_->resolve_for(svc.oauth1_token_secret, tenant, url, &why);
    if (!t || !ts) return *error = "the OAuth 1.0a token: " + why, std::nullopt;
    Secret s;
    s.kind = "oauth1";
    s.token = *t;
    s.token_secret = *ts;
    return s;
  }
  if (svc.credentials.empty()) return Secret{};  // no secret: nothing applied
  std::string why;
  auto value = keys_->resolve_for(svc.credentials, tenant, url, &why);
  if (!value) return *error = why, std::nullopt;
  Secret s;
  s.kind = "key";
  s.value = *value;
  return s;
}

HttpOut Host::send_httpx(const std::string& tenant, const std::string& box, const manifest::ExternalService& svc,
                         const manifest::ExternalMethod& m, const HttpxCall& call, int64_t timeout_ms,
                         const std::string& request_id, int64_t outbox_id) {
  auto cfg = config_.get();
  int64_t started = base::monotonic_us();
  // Discovered URLs (any-public) are checked like the crawler's: a public address only, outside local mode.
  // Outside local mode, only public addresses, as for every service (send_http checks).
  auto out = send_http(call.verb, call.url, call.headers, call.body, call.content_type, cfg->local, cfg->storage / "tmp",
                       timeout_ms);
  egress_log(tenant, box, svc.name, m.name, call.verb, call.url, out.status, out.error, base::monotonic_us() - started,
             call.body.size(), out.body.size(), request_id, outbox_id);
  int64_t now = base::now_unix_ms();
  // Rate limits (034 § 9): Retry-After on 429 and 503, and an exhausted window's reset.
  // Waits are capped at a day, so a hostile or broken header can't park an element for good.
  if (svc.retry_after && (out.status == 429 || out.status == 503)) {
    if (auto s = header_seconds(out.header("retry-after"))) out.retry_at_ms = now + std::min(*s * 1000, kMaxWaitMs);
  }
  if (!svc.limit_remaining.empty() && out.header(svc.limit_remaining) == "0" && !svc.limit_reset.empty()) {
    if (auto r = header_seconds(out.header(svc.limit_reset))) {
      // An epoch time in seconds, or seconds from now.
      int64_t at = *r > 1'000'000'000 ? *r * 1000 : now + *r * 1000;
      out.retry_at_ms = std::max(out.retry_at_ms, std::min(at, now + kMaxWaitMs));
    }
  }
  if (!m.error_when.empty() && out.error.empty()) {
    if (holds(m.error_when, answer_of(out))) out.error = "the service answered an error (error-when " + m.error_when.at + ")";
  }
  return out;
}

bj::object Host::answer_of(const HttpOut& out) {
  bj::object headers;
  for (const auto& [k, v] : out.headers) headers[k] = v;
  std::string err;
  bj::value body = bj::string(out.body);
  if (out.content_type.find("json") != std::string::npos) {
    if (auto v = json::parse(out.body, &err)) body = *v;
  }
  return bj::object{{"status", out.status}, {"body", std::move(body)}, {"headers", std::move(headers)}};
}

std::string Host::httpx_call(CallContext& ctx, const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                             const bj::value& request) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  if (!request.is_object() && !request.is_null()) return fail("a call is a JSON object {path, query, headers, body, credential, url}");
  bj::object r = request.is_object() ? request.as_object() : bj::object{};
  for (const auto& kv : r) {
    static const std::set<std::string, std::less<>> kKeys = {"path", "query", "headers", "body", "credential", "url"};
    if (!kKeys.count(kv.key())) return fail("a call is {path, query, headers, body, credential, url}; \"" + std::string(kv.key()) + "\" isn't one");
  }
  if (m.any_public && !r.contains("url")) return fail("this method takes the whole URL as url");
  // A GET imported from a spec is safe, unless the manifest said otherwise.
  std::string effect = m.effect;
  std::string err;
  if (!m.op.empty() && !m.effect_given) {
    if (auto spec = specs_->get(svc.spec, &err)) {
      if (const auto* op = spec->find(m.op)) effect = op->verb == "get" || op->verb == "head" ? "none" : "outbox";
    }
  }
  // The credential opens now (refreshed if due), so a bad one fails in the box's hands.
  std::string resealed;
  auto secret = secret_for(ctx.tenant, svc, r, svc.base, &resealed, &err);
  if (!secret) {
    bj::object e{{"error", err}};
    if (err.starts_with("connection_expired")) e["connect"] = "/goober/connect/" + ctx.manifest->box + "/" + svc.name;
    return json::dump(e);
  }
  if (!resealed.empty()) r["credential"] = resealed;
  // A person's own secret serves their own requests: another signed-in person's call can't use it.
  // (Timers, results, and outbox deliveries carry no person, so the box decides there.)
  if (const auto* u = ctx.user.if_object(); u && !secret->owner.empty()) {
    const auto* uid = u->if_contains("user_id");
    if (uid && uid->is_string() && uid->as_string() != secret->owner) {
      return fail("the credential belongs to another person (user " + secret->owner + ")");
    }
  }
  if (effect == "outbox") {
    std::string why;
    if (!outbox_quota_take(ctx.tenant, svc.name, &why)) return fail(why);
    // Checked now, built again at delivery (when the secret is applied and a token refreshed).
    if (m.upload.empty()) {
      auto check = build_httpx(ctx.tenant, ctx.user, ctx.domain, svc, m, "", r, "", &err);
      if (!check) return fail(err);
    }
    OutboxElement e;
    e.service = svc.name;
    e.method = m.name;
    e.verb = "HTTPX";
    e.url = "httpx:" + svc.name + "." + m.name;
    e.body = json::dump(bj::object{{"request", r}, {"user", ctx.user}, {"domain", ctx.domain}});
    e.idempotency_header = m.idempotency_header;
    e.idempotency_key = base::sha256_hex("goober-outbox\n" + ctx.request_id + "\n" + std::to_string(ctx.pending.size())).substr(0, 32);
    e.attempts = m.attempts;
    e.backoff_ms = m.backoff_ms;
    e.within_ms = std::max<int64_t>(m.within_ms, m.polls ? m.poll.within_ms + 3600000 : 0);
    e.result_route = m.result_route;
    e.timeout_ms = m.timeout_ms;
    std::string key = e.idempotency_key;
    ctx.pending.push_back(std::move(e));
    bj::object out{{"accepted", true}, {"key", key}};
    if (!resealed.empty()) out["credential"] = resealed;
    return json::dump(out);
  }
  auto call = build_httpx(ctx.tenant, ctx.user, ctx.domain, svc, m, "", r, "", &err);
  if (!call) return fail(err);
  if (!apply_secret(ctx.tenant, svc, *secret, &*call, &err)) return fail(err);
  int64_t left_ms = std::max<int64_t>(50, (ctx.deadline_us - base::monotonic_us()) / 1000);
  auto out = send_httpx(ctx.tenant, ctx.manifest->box, svc, m, *call, std::min<int64_t>(left_ms, 10000), ctx.request_id, 0);
  bj::object answer{{"status", out.status}, {"content_type", out.content_type}, {"body", base::valid_utf8(out.body)}};
  if (out.content_type.find("json") != std::string::npos) {
    if (auto v = json::parse(out.body, &err)) answer["json"] = *v;
  }
  if (!out.error.empty()) answer["error"] = out.error;
  if (out.retry_at_ms) answer["retry_at"] = base::format_utc_ms(out.retry_at_ms);
  if (!resealed.empty()) answer["credential"] = resealed;
  return json::dump(answer);
}

HttpOut Host::deliver_outbox_httpx(const std::string& tenant, sqlite::Db& db, int64_t id, const std::string& box,
                                   const std::string& service, const std::string& method, const std::string& body,
                                   const std::string& idem_header, const std::string& idem_key, int64_t timeout_ms,
                                   int64_t now_ms) {
  HttpOut fail;
  auto active = boxes_->active();
  auto bit = active->boxes.find(box);
  const manifest::ExternalService* svc = nullptr;
  if (bit != active->boxes.end()) {
    for (const auto& s : bit->second->manifest.external) {
      if (s.name == service) svc = &s;
    }
  }
  const manifest::ExternalMethod* m = nullptr;
  if (svc) {
    if (auto it = svc->methods.find(method); it != svc->methods.end()) m = &it->second;
  }
  if (!m) {
    fail.status = 410;
    fail.error = "the box no longer has " + service + "." + method;
    return fail;
  }
  std::string err;
  auto v = json::parse(body, &err);
  if (!v || !v->is_object()) {
    fail.status = 400;
    fail.error = "the element is unreadable";
    return fail;
  }
  bj::object state = v->as_object();
  bj::object request = state.contains("request") && state.at("request").is_object() ? state.at("request").as_object() : bj::object{};
  bj::value user = state.contains("user") ? state.at("user") : bj::value(nullptr);
  std::string domain = state.contains("domain") && state.at("domain").is_string() ? std::string(state.at("domain").as_string()) : "";
  auto save_state = [&]() {
    sqlite::Stmt u(db, "UPDATE goober_outbox SET body = ? WHERE id = ?");
    u.bind(1, json::dump(state)).bind(2, id).run();
  };
  // The secret, opened at each attempt (and refreshed when due); a newer seal is kept.
  auto secret_and_send = [&](const std::string& op_ref, const bj::object& req, const std::string& mode,
                             const std::vector<std::pair<std::string, std::string>>& extra_headers,
                             std::string* body_override, std::string* ct_override) -> HttpOut {
    HttpOut out;
    std::string resealed, e2;
    auto secret = secret_for(tenant, *svc, request, svc->base, &resealed, &e2);
    if (!secret) {
      out.status = e2.starts_with("connection_expired") ? 401 : 0;
      out.error = e2;
      return out;
    }
    if (!resealed.empty()) {
      request["credential"] = resealed;
      state["request"] = request;
      save_state();
    }
    auto call = build_httpx(tenant, user, domain, *svc, *m, op_ref, req, mode, &e2);
    if (!call) {
      out.status = 400;
      out.error = e2;
      return out;
    }
    if (body_override) {
      call->body = std::move(*body_override);
      call->content_type = *ct_override;
    }
    for (const auto& h : extra_headers) call->headers.push_back(h);
    if (!apply_secret(tenant, *svc, *secret, &*call, &e2)) {
      out.error = e2;
      return out;
    }
    return send_httpx(tenant, box, *svc, *m, *call, timeout_ms, "", id);
  };
  std::vector<std::pair<std::string, std::string>> idem;
  if (!idem_header.empty()) idem.emplace_back(idem_header, idem_key);
  auto ok = [](const HttpOut& o) { return o.error.empty() && o.status >= 200 && o.status < 300; };
  std::string phase = state.contains("phase") && state.at("phase").is_string() ? std::string(state.at("phase").as_string()) : "";

  if (phase.empty()) {
    HttpOut first;
    if (!m->upload.empty()) {
      // The chunked upload (034 § 7): init, each append, finalize, as one element.
      const auto& up = m->upload;
      bj::value wrapped = bj::object{{"body", request.contains("body") ? request.at("body") : bj::value(nullptr)}};
      const bj::value* aid = at_path(wrapped, up.attachment_at);
      if (!aid) {
        fail.status = 400;
        fail.error = "the call names no attachment at " + up.attachment_at;
        return fail;
      }
      auto file = attachment_bytes(tenant, *aid, &err);
      if (!file) {
        fail.status = 400;
        fail.error = err;
        return fail;
      }
      if (up.chunk_bytes > 0 && int64_t(file->bytes.size() / size_t(up.chunk_bytes)) >= kMaxChunks) {
        fail.status = 400;
        fail.error = "the file needs more than " + std::to_string(kMaxChunks) + " chunks; raise chunk-bytes";
        return fail;
      }
      std::string upload_id;
      auto vars = [&](const std::string& k) -> std::optional<std::string> {
        if (k == "content_type") return file->content_type;
        if (k == "bytes") return std::to_string(file->bytes.size());
        if (k == "filename") return file->filename;
        if (k == "id") return upload_id;
        return std::nullopt;
      };
      // A template's strings filled in; a value that is exactly "{bytes}" becomes a number.
      std::function<bj::value(const bj::value&)> fill_json = [&](const bj::value& t) -> bj::value {
        if (t.is_string()) {
          std::string s(t.as_string());
          if (s == "{bytes}") return bj::value(int64_t(file->bytes.size()));
          return bj::value(fill(s, vars));
        }
        if (t.is_object()) {
          bj::object o;
          for (const auto& kv : t.as_object()) o[kv.key()] = fill_json(kv.value());
          return o;
        }
        if (t.is_array()) {
          bj::array a;
          for (const auto& e : t.as_array()) a.push_back(fill_json(e));
          return a;
        }
        return t;
      };
      auto step_request = [&](const manifest::UploadStep& s) {
        bj::object pth;
        for (const auto& [k, val] : s.path) pth[k] = fill(val, vars);
        bj::object rq{{"path", pth}};
        if (!s.body.empty()) {
          std::string e3;
          if (auto t = json::parse(s.body, &e3)) rq["body"] = fill_json(*t);
        }
        return rq;
      };
      auto init = secret_and_send(up.init.op, step_request(up.init), "json", idem, nullptr, nullptr);
      if (!ok(init)) return init;
      const bj::value* idv = at_path(answer_of(init), up.init.id_at);
      if (!idv || idv->is_null()) {
        init.status = 502;
        init.error = "the upload's init answered no id at " + up.init.id_at;
        return init;
      }
      upload_id = text_of(*idv);
      int index = 0;
      for (size_t at = 0; at < file->bytes.size(); at += size_t(up.chunk_bytes), ++index) {
        std::string chunk = file->bytes.substr(at, size_t(up.chunk_bytes));
        std::vector<Part> parts;
        for (const auto& [name, val] : up.append.parts) {
          if (val == "{chunk}") {
            parts.push_back(Part{name, file->filename, "application/octet-stream", chunk});
          } else {
            parts.push_back(Part{name, "", "", fill(fill(val, vars), [&](const std::string& k) -> std::optional<std::string> {
                                   return k == "index" ? std::optional(std::to_string(index)) : std::nullopt;
                                 })});
          }
        }
        std::string ct;
        std::string mp = multipart(parts, &ct);
        auto app = secret_and_send(up.append.op, step_request(up.append), "json", {}, &mp, &ct);
        if (!ok(app)) return app;
      }
      first = secret_and_send(up.finalize.op, step_request(up.finalize), "json", {}, nullptr, nullptr);
    } else {
      first = secret_and_send("", request, "", idem, nullptr, nullptr);
    }
    if (!ok(first) || !m->polls) return first;
    // An async job (034 § 8): wait, then ask.
    state["phase"] = "polling";
    state["first"] = answer_of(first);
    state["since_ms"] = now_ms;
    save_state();
    HttpOut wait;
    wait.polling = true;
    wait.retry_at_ms = now_ms + m->poll.every_ms;
    return wait;
  }

  // Polling.
  const bj::value first = state.contains("first") ? state.at("first") : bj::value(bj::object{});
  auto from_first = [&](const std::string& k) -> std::optional<std::string> {
    if (const bj::value* x = at_path(first, k)) return text_of(*x);
    return std::nullopt;
  };
  bj::object pth, qry;
  for (const auto& [k, val] : m->poll.path) pth[k] = fill(val, from_first);
  for (const auto& [k, val] : m->poll.query) qry[k] = fill(val, from_first);
  bj::object preq{{"path", pth}, {"query", qry}};
  auto out = secret_and_send(m->poll.op, preq, "json", {}, nullptr, nullptr);
  if (!out.error.empty() && out.status == 0) return out;  // transport: the outbox retries
  bj::object answer = answer_of(out);
  int64_t since = state.contains("since_ms") && state.at("since_ms").is_int64() ? state.at("since_ms").as_int64() : now_ms;
  if (ok(out) && holds(m->poll.done, answer)) return out;
  if (holds(m->poll.failed, answer)) {
    out.status = 422;
    if (out.error.empty()) out.error = "the job failed (" + m->poll.failed.at + ")";
    return out;
  }
  if (now_ms - since > m->poll.within_ms) {
    out.status = 504;
    out.error = "the job didn't finish within " + std::to_string(m->poll.within_ms / 1000) + " s";
    return out;
  }
  HttpOut wait;
  wait.polling = true;
  int64_t every = m->poll.every_ms;
  if (!m->poll.every_from.empty()) {
    if (const bj::value* s = at_path(answer, m->poll.every_from); s && s->is_number()) {
      every = std::clamp<int64_t>(int64_t(s->to_number<double>() * 1000), 1000, 600000);
    }
  }
  wait.retry_at_ms = std::max(out.retry_at_ms, now_ms + every);
  return wait;
}

}  // namespace goober::host
