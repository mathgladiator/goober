// External services and the outbox (030.TheLargerEnvironment.md, 035.TheOutBox.md;
// design/machine/015.Network.md). A box calls a declared method with goober_call; the host
// builds the request (pinning what require says), attaches the credential the box never
// sees, and either sends it now (effect none or immediate) or captures it in the outbox, in
// the same transaction as the box's writes, for delivery after commit with retries and an
// idempotency key.

#include "network.h"

#include <fcntl.h>
#include <unistd.h>

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/process.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "keys.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxReplyBytes = 4 << 20;

bool is_loopback_http(std::string_view url) {
  return url.starts_with("http://127.0.0.1:") || url.starts_with("http://127.0.0.1/") ||
         url.starts_with("http://localhost:") || url.starts_with("http://localhost/");
}

std::string text_of(const bj::value& v) {
  if (v.is_string()) return std::string(v.as_string());
  if (v.is_null()) return "";
  return json::dump(v);
}

// A config-file value for curl: double-quoted, with \ and " escaped.
std::string curl_quoted(const std::string& v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

}  // namespace

std::optional<BuiltCall> build_call(const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                                    const bj::value& request, const CallContext& ctx, std::string* error) {
  bj::object path_params, query, body_obj;
  bj::value body = nullptr;
  if (request.is_object()) {
    const auto& r = request.as_object();
    for (const auto& kv : r) {
      if (kv.key() != "path" && kv.key() != "query" && kv.key() != "body") {
        return *error = "a call is {path, query, body}; \"" + std::string(kv.key()) + "\" isn't one", std::nullopt;
      }
    }
    if (auto* p = r.if_contains("path"); p && p->is_object()) path_params = p->as_object();
    if (auto* q = r.if_contains("query"); q && q->is_object()) query = q->as_object();
    if (auto* b = r.if_contains("body")) body = *b;
  } else if (!request.is_null()) {
    return *error = "a call is a JSON object {path, query, body}", std::nullopt;
  }
  // require: the host's values win, so the box has no say over what the manifest pins (030).
  std::vector<std::pair<std::string, std::string>> headers;
  for (const auto& [field, value] : m.require) {
    std::string v = value;
    if (value == "{!email}" || value == "{!user_id}") {
      const char* k = value == "{!email}" ? "email" : "user_id";
      if (!ctx.user.is_object() || !ctx.user.as_object().contains(k)) {
        return *error = "this call pins " + field + " to the signed-in person, and nobody is signed in", std::nullopt;
      }
      v = std::string(ctx.user.as_object().at(k).as_string());
    } else if (value == "{!tenant}") {
      v = ctx.tenant;
    } else if (value == "{!domain}") {
      v = ctx.domain;
    }
    auto dot = field.find('.');
    std::string part = field.substr(0, dot), name = field.substr(dot + 1);
    if (part == "path") path_params[name] = v;
    if (part == "query") query[name] = v;
    if (part == "header") headers.emplace_back(base::to_lower(name), v);
    if (part == "body") {
      if (body.is_null()) body = bj::object{};
      if (!body.is_object()) return *error = "this call pins " + field + ", so the body must be an object", std::nullopt;
      // A dotted path walks into nested objects, creating them as needed.
      bj::object* at = &body.as_object();
      size_t start = 0;
      for (size_t dot2 = name.find('.'); dot2 != std::string::npos; dot2 = name.find('.', start)) {
        auto& next = (*at)[name.substr(start, dot2 - start)];
        if (!next.is_object()) next = bj::object{};
        at = &next.as_object();
        start = dot2 + 1;
      }
      (*at)[name.substr(start)] = v;
    }
  }
  BuiltCall out;
  std::string path;
  for (size_t i = 0; i < m.path.size(); ++i) {
    if (m.path[i] != '{') {
      path += m.path[i];
      continue;
    }
    size_t close = m.path.find('}', i);
    std::string name = m.path.substr(i + 1, close - i - 1);
    auto* v = path_params.if_contains(name);
    if (!v || v->is_null()) return *error = "the call needs path." + name, std::nullopt;
    // Every byte but the unreserved ones is escaped, so a value can't add a segment or a query.
    path += base::percent_encode(text_of(*v));
    i = close;
  }
  out.url = svc.base + path;
  bool first = true;
  for (const auto& [k, v] : std::map<std::string, std::string>(
           [&] {
             std::map<std::string, std::string> sorted;
             for (const auto& kv : query) sorted[std::string(kv.key())] = text_of(kv.value());
             return sorted;
           }())) {
    out.url += (first ? "?" : "&") + base::percent_encode(k) + "=" + base::percent_encode(v);
    first = false;
  }
  if (!body.is_null()) {
    if (body.is_string()) {
      out.body = std::string(body.as_string());
      out.content_type = "text/plain; charset=utf-8";
    } else {
      out.body = json::dump(body);
      out.content_type = "application/json";
    }
  }
  out.headers = std::move(headers);
  return out;
}

HttpOut send_http(const std::string& verb, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, const std::string& content_type, bool allow_loopback_http,
                  const fs::path& scratch, int64_t timeout_ms) {
  HttpOut out;
  bool loopback = is_loopback_http(url);
  if (!url.starts_with("https://") && !(loopback && allow_loopback_http)) {
    out.error = "only https services can be called";
    return out;
  }
  // Headers, credentials among them, go to curl in a private file, never on its command line.
  std::string conf;
  for (const auto& [k, v] : headers) {
    for (char c : k + v) {
      if (static_cast<unsigned char>(c) < 0x20) {
        out.error = "a header holds a control character";
        return out;
      }
    }
    conf += "header = " + curl_quoted(k + ": " + v) + "\n";
  }
  if (!content_type.empty()) conf += "header = " + curl_quoted("Content-Type: " + content_type) + "\n";
  uint8_t rnd[12];
  base::os_random_fill(rnd);
  fs::create_directories(scratch);
  fs::path conf_file = scratch / (".call-" + base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd)));
  int fd = ::open(conf_file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0 || ::write(fd, conf.data(), conf.size()) != ssize_t(conf.size())) {
    if (fd >= 0) ::close(fd);
    out.error = "can't prepare the call";
    return out;
  }
  ::close(fd);
  std::string upper = verb;
  for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
  std::vector<std::string> argv = {"curl", "-q", "-g", "--noproxy", "*", "-sS", "-i", "--max-redirs", "0",
                                   "--proto", loopback ? "=http" : "=https", "--max-time",
                                   std::to_string(std::max<int64_t>(1, timeout_ms / 1000)), "-X", upper, "-K",
                                   conf_file.string()};
  if (!body.empty()) {
    argv.push_back("--data-binary");
    argv.push_back("@-");
  }
  argv.push_back("--");
  argv.push_back(url);
  auto r = base::run_process(argv, body, kMaxReplyBytes + 65536, timeout_ms + 2000);
  std::error_code ec;
  fs::remove(conf_file, ec);
  if (!r.started) {
    out.error = "could not run curl";
    return out;
  }
  if (r.truncated) {
    out.error = "the reply is larger than " + std::to_string(kMaxReplyBytes) + " bytes";
    return out;
  }
  if (r.timed_out || r.exit_code != 0) {
    std::string msg = r.err;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    out.error = r.timed_out ? "the service took too long" : (msg.empty() ? "the call failed" : msg);
    return out;
  }
  // -i puts the status and headers first; skip any 1xx blocks.
  std::string_view rest = r.out;
  while (true) {
    auto end = rest.find("\r\n\r\n");
    if (end == std::string_view::npos) {
      out.error = "the service's reply couldn't be read";
      return out;
    }
    std::string_view head = rest.substr(0, end);
    rest.remove_prefix(end + 4);
    auto sp = head.find(' ');
    out.status = sp == std::string_view::npos ? 0 : std::atoi(std::string(head.substr(sp + 1, 3)).c_str());
    if (out.status >= 200 || out.status == 0) {
      std::string lower = base::to_lower(head);
      if (auto ct = lower.find("\r\ncontent-type:"); ct != std::string::npos) {
        auto eol = lower.find("\r\n", ct + 2);
        std::string v(head.substr(ct + 15, (eol == std::string::npos ? head.size() : eol) - ct - 15));
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        out.content_type = v;
      }
      break;
    }
  }
  out.body = std::string(rest);
  return out;
}

std::string Host::external_call(CallContext& ctx, std::string_view service, std::string_view method,
                                std::string_view request_text) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  const manifest::ExternalService* svc = nullptr;
  for (const auto& s : ctx.manifest->external) {
    if (s.name == service) svc = &s;
  }
  if (!svc) return fail("no external service named " + std::string(service) + " in the manifest");
  auto mit = svc->methods.find(std::string(method));
  if (mit == svc->methods.end()) return fail("service " + svc->name + " has no method " + std::string(method));
  const auto& m = mit->second;
  // The per-request budget: past it, the call never leaves the host (030).
  int64_t& used = ctx.calls[svc->name + "." + m.name];
  if (++used > m.budget_calls) return fail("over budget: " + std::to_string(m.budget_calls) + " calls per request");
  bj::value request = nullptr;
  if (!request_text.empty()) {
    std::string err;
    auto parsed = json::parse(request_text, &err);
    if (!parsed) return fail("the call isn't JSON: " + err);
    request = std::move(*parsed);
  }
  std::string err;
  auto built = build_call(*svc, m, request, ctx, &err);
  if (!built) return fail(err);
  std::string credentials = svc->credentials;
  if (auto at = credentials.find("{!tenant}"); at != std::string::npos) credentials.replace(at, 9, ctx.tenant);

  if (m.effect == "outbox") {
    OutboxElement e;
    e.service = svc->name;
    e.method = m.name;
    e.verb = m.verb;
    e.url = built->url;
    e.body = built->body;
    e.content_type = built->content_type;
    e.credentials = svc->credentials.empty() ? "" : svc->credentials;
    e.auth_header = svc->auth_header;
    e.auth_prefix = svc->auth_prefix;
    e.headers = built->headers;
    e.idempotency_header = m.idempotency_header;
    // The key comes from the request and the element's place in it: stable across retries (035).
    e.idempotency_key = base::sha256_hex("goober-outbox\n" + ctx.request_id + "\n" + std::to_string(ctx.pending.size()))
                            .substr(0, 32);
    e.attempts = m.attempts;
    e.backoff_ms = m.backoff_ms;
    e.within_ms = m.within_ms;
    std::string key = e.idempotency_key;
    ctx.pending.push_back(std::move(e));
    return json::dump(bj::object{{"accepted", true}, {"key", key}});
  }

  // Sent now: effect none (safe), or immediate (effectful, and visible as such).
  auto headers = built->headers;
  if (!svc->credentials.empty()) {
    auto value = keys_->resolve(svc->credentials, ctx.tenant);
    if (!value) return fail("the key " + credentials + " isn't set; an operator sets it with key_set");
    headers.emplace_back(svc->auth_header, svc->auth_prefix + *value);
  }
  auto cfg = config_.get();
  int64_t started = base::monotonic_us();
  auto reply = send_http(m.verb, built->url, headers, built->body, built->content_type, cfg->local,
                         cfg->storage / "tmp", 10000);
  egress_log(ctx.tenant, ctx.manifest->box, svc->name, m.name, m.verb, built->url, reply.status, reply.error,
             base::monotonic_us() - started, built->body.size(), reply.body.size(), ctx.request_id, 0);
  if (!reply.error.empty()) return fail(reply.error);
  bj::object result{{"status", reply.status}, {"content_type", reply.content_type}, {"body", reply.body}};
  if (reply.content_type.starts_with("application/json")) {
    if (auto v = json::parse(reply.body, &err)) result["json"] = std::move(*v);
  }
  return json::dump(result);
}

void Host::egress_log(const std::string& tenant, const std::string& box, const std::string& service,
                      const std::string& method, const std::string& verb, const std::string& url, int status,
                      const std::string& error, int64_t us, size_t bytes_out, size_t bytes_in,
                      const std::string& request_id, int64_t outbox_id) {
  // What went out and what came back, without query strings or bodies, which can hold secrets (030).
  bj::object line{{"at", base::format_utc_ms(base::now_unix_ms())},
                  {"tenant", tenant},
                  {"box", box},
                  {"service", service},
                  {"method", method},
                  {"verb", verb},
                  {"url", url.substr(0, url.find('?'))},
                  {"status", status},
                  {"us", us},
                  {"bytes_out", int64_t(bytes_out)},
                  {"bytes_in", int64_t(bytes_in)}};
  if (!error.empty()) line["error"] = error;
  if (!request_id.empty()) line["request_id"] = request_id;
  if (outbox_id) line["outbox_id"] = outbox_id;
  base::append_line(config_.get()->storage / "logs" / "egress.log", json::dump(line));
}

}  // namespace goober::host
