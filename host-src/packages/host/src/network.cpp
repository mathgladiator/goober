// External services and the outbox (030.TheLargerEnvironment.md, 035.TheOutBox.md;
// design/machine/015.Network.md). A box calls a declared method with goober_call; the host
// builds the request (pinning what require says), attaches the credential the box never
// sees, and either sends it now (effect none or immediate) or captures it in the outbox, in
// the same transaction as the box's writes, for delivery after commit with retries and an
// idempotency key.

#include "network.h"


#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/process.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/router/path.h"
#include "goober/types/types.h"
#include "keys.h"
#include "web_fetch.h"

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
    // Every byte but the unreserved ones is escaped, so a value can't add a segment or a query;
    // and . and .. would walk the path itself (curl resolves them), so they are refused.
    std::string text = text_of(*v);
    if (text.empty() || text == "." || text == "..") return *error = "path." + name + " can't be empty, . or ..", std::nullopt;
    path += base::percent_encode(text);
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
    conf += "header = " + base::curl_quoted(k + ": " + v) + "\n";
  }
  if (!content_type.empty()) conf += "header = " + base::curl_quoted("Content-Type: " + content_type) + "\n";
  // The URL too: its query can hold a person's email or a token, and argv is public.
  conf += "url = " + base::curl_quoted(url) + "\n";
  // Outside local mode, only a public address, and the one checked: a service's name can't
  // point the host at its own network (security review 4, S4-5).
  std::string resolve;
  if (!allow_loopback_http) {
    std::string rest = url.substr(url.find("://") + 3);
    std::string authority = rest.substr(0, rest.find_first_of("/?#"));
    std::string host = authority, port = "443";
    if (authority.starts_with("[")) {
      host = authority.substr(0, authority.find(']') + 1);
      if (authority.size() > host.size() + 1 && authority[host.size()] == ':') port = authority.substr(host.size() + 1);
    } else if (auto colon = authority.rfind(':'); colon != std::string::npos) {
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    }
    std::string why;
    auto addr = pick_address(host, false, &why);
    if (!addr) {
      out.error = why;
      return out;
    }
    if (host.front() != '[' && !std::all_of(host.begin(), host.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; })) {
      resolve = host + ":" + port + ":" + *addr;
    }
  }
  auto written = base::write_private_file(scratch, ".call-", conf);
  if (!written) {
    out.error = "can't prepare the call";
    return out;
  }
  fs::path conf_file = *written;
  std::string upper = verb;
  for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
  std::vector<std::string> argv = {"curl", "-q", "-g", "--path-as-is", "--noproxy", "*", "-sS", "-i", "--max-redirs", "0",
                                   "--proto", loopback ? "=http" : "=https", "--max-time",
                                   std::to_string(std::max<int64_t>(50, timeout_ms) / 1000) + "." +
                                       std::to_string(std::max<int64_t>(50, timeout_ms) % 1000 / 100),
                                   "-X", upper, "-K",
                                   conf_file.string()};
  if (!body.empty()) {
    argv.push_back("--data-binary");
    argv.push_back("@-");
  }
  if (!resolve.empty()) {
    argv.push_back("--resolve");
    argv.push_back(resolve);
  }
  auto r = base::run_process(argv, body, kMaxReplyBytes + 65536, timeout_ms + 500);
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

namespace {
// How deep a chain of internal calls is on this thread; the host caps it (039).
thread_local int internal_depth = 0;
constexpr int kMaxInternalDepth = 4;
}  // namespace

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
  if (!svc->box.empty()) return internal_call(ctx, *svc, m, request);
  if (svc->web) return web_call(ctx, *svc, request);
  if (svc->mail && m.name == "attachment") return mail_attachment_call(ctx, request);
  if (svc->mail) return mail_call(ctx, *svc, m, request);
  if (svc->push) return push_call(ctx, *svc, m, request);
  std::string err;
  auto built = build_call(*svc, m, request, ctx, &err);
  if (!built) return fail(err);
  std::string credentials = svc->credentials;
  if (auto at = credentials.find("{!tenant}"); at != std::string::npos) credentials.replace(at, 9, ctx.tenant);

  if (m.effect == "outbox") {
    // The host's daily cap per tenant and service (config limits.outbox-daily-per-service),
    // counted when the call is made, so a looping box learns at once.
    std::string why;
    if (!outbox_quota_take(ctx.tenant, svc->name, &why)) return fail(why);
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
    e.result_route = m.result_route;
    e.timeout_ms = m.timeout_ms;
    e.attempts = m.attempts;
    e.backoff_ms = m.backoff_ms;
    e.within_ms = m.within_ms;
    std::string key = e.idempotency_key;
    ctx.pending.push_back(std::move(e));
    return json::dump(bj::object{{"accepted", true}, {"key", key}});
  }

  // Sent now: effect none (safe), or immediate (effectful, and visible as such). Not from a
  // sandbox, which a rehydrated devbox is: nothing it does reaches the world.
  if (config_.get()->sandbox) return fail("this host is a sandbox: no calls leave it");
  auto headers = built->headers;
  if (!svc->credentials.empty()) {
    std::string why;
    auto value = keys_->resolve_for(svc->credentials, ctx.tenant, built->url, &why);
    if (!value) return fail(why);
    headers.emplace_back(svc->auth_header, svc->auth_prefix + *value);
  }
  auto cfg = config_.get();
  int64_t started = base::monotonic_us();
  // A call never outlives the request that makes it: wasm deadlines can't interrupt a host call.
  int64_t timeout_ms = 10000;
  if (ctx.deadline_us) {
    timeout_ms = std::min(timeout_ms, (ctx.deadline_us - base::monotonic_us()) / 1000);
    if (timeout_ms < 50) return fail("no time left for the call");
  }
  auto reply = send_http(m.verb, built->url, headers, built->body, built->content_type, cfg->local,
                         cfg->storage / "tmp", timeout_ms);
  egress_log(ctx.tenant, ctx.manifest->box, svc->name, m.name, m.verb, built->url, reply.status, reply.error,
             base::monotonic_us() - started, built->body.size(), reply.body.size(), ctx.request_id, 0);
  if (!reply.error.empty()) return fail(reply.error);
  bj::object result{{"status", reply.status}, {"content_type", reply.content_type}, {"body", reply.body}};
  if (reply.content_type.starts_with("application/json")) {
    if (auto v = json::parse(reply.body, &err)) result["json"] = std::move(*v);
  }
  return json::dump(result);
}

namespace {

bool plain_email(std::string_view s) {
  auto at = s.find('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= s.size() || s.size() > 254) return false;
  if (s.find('@', at + 1) != std::string_view::npos || s.find('.', at) == std::string_view::npos) return false;
  for (unsigned char c : s) {
    if (c <= 0x20 || c >= 0x7f || c == '<' || c == '>' || c == '"' || c == ',' || c == ';' || c == '(' || c == ')' ||
        c == '\\' || c == '[' || c == ']') {
      return false;
    }
  }
  return true;
}

}  // namespace

bool Host::outbox_quota_take(const std::string& tenant, const std::string& service, std::string* why) {
  auto cfg = config_.get();
  int64_t now = base::now_unix_ms();
  std::lock_guard lock(outbox_quota_mu_);
  fs::create_directories(cfg->storage / "outbox");
  sqlite::Db db(cfg->storage / "outbox" / "daily.db");
  db.exec("CREATE TABLE IF NOT EXISTS calls (tenant TEXT NOT NULL, service TEXT NOT NULL, at_ms INTEGER NOT NULL)");
  db.exec("CREATE INDEX IF NOT EXISTS calls_by_service ON calls (tenant, service, at_ms)");
  db.exec("DELETE FROM calls WHERE at_ms < " + std::to_string(now - 2LL * 86400000));
  sqlite::Stmt count(db, "SELECT count(*) FROM calls WHERE tenant = ? AND service = ? AND at_ms > ?");
  count.bind(1, tenant).bind(2, service).bind(3, now - 86400000);
  count.step();
  if (count.int64(0) >= cfg->outbox_daily_per_service) {
    *why = "over the host's daily limit: " + std::to_string(cfg->outbox_daily_per_service) + " calls to " + service +
           " a day for this tenant (config limits.outbox-daily-per-service)";
    return false;
  }
  sqlite::Stmt add(db, "INSERT INTO calls (tenant, service, at_ms) VALUES (?, ?, ?)");
  add.bind(1, tenant).bind(2, service).bind(3, now).run();
  return true;
}

std::string Host::mail_call(CallContext& ctx, const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                            const bj::value& request) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  const char* kShape = "send takes {to, subject, text, reply_to?} or {to, template, vars?, reply_to?}, and reply? to "
                       "mint a reply address, and in_reply_to? and references? (Message-IDs) to answer in a thread";
  if (!request.is_object()) return fail(kShape);
  const auto& r = request.as_object();
  for (const auto& kv : r) {
    if (kv.key() != "to" && kv.key() != "subject" && kv.key() != "text" && kv.key() != "reply_to" &&
        kv.key() != "template" && kv.key() != "vars" && kv.key() != "reply" && kv.key() != "in_reply_to" &&
        kv.key() != "references") {
      return fail(std::string(kShape) + "; \"" + std::string(kv.key()) + "\" isn't one");
    }
  }
  auto text_field = [&](const char* k) -> std::string {
    auto* v = r.if_contains(k);
    return v && v->is_string() ? std::string(v->as_string()) : "";
  };
  std::string to = base::to_lower(text_field("to")), subject = text_field("subject"), text = text_field("text"),
              reply_to = base::to_lower(text_field("reply_to"));
  if (!plain_email(to)) return fail("to must be one email address");
  if (!reply_to.empty() && !plain_email(reply_to)) return fail("reply_to must be one email address");
  // Unsubscribed is final for every box of the tenant (design/machine/018.Mail.md § 3).
  if (mail_suppressed(ctx.tenant, to)) return fail("unsubscribed: " + to + " asked for no more mail from this site");
  // From an address of the tenant's own, on a domain the host config gives it.
  auto cfg = config_.get();
  std::string from_address = svc.mail_from, site = ctx.domain;
  if (!from_address.empty()) {
    std::string from_domain = from_address.substr(from_address.find('@') + 1);
    auto d = cfg->domains.find(from_domain);
    if (d == cfg->domains.end() || d->second.tenant != ctx.tenant) {
      return fail("mail.from " + from_address + " isn't on a domain of tenant " + ctx.tenant);
    }
    site = from_domain;
  }
  if (site.empty() || site == "*") {
    for (const auto& [name, entry] : cfg->domains) {
      if (entry.tenant == ctx.tenant) {
        site = name;
        break;
      }
    }
  }
  // Threading (RFC 5322 § 3.6.4): Message-IDs, given with or without their angle brackets.
  auto message_ids = [](const std::string& v, size_t most, std::string* out) {
    std::string ids;
    size_t n = 0, i = 0;
    while (i < v.size()) {
      while (i < v.size() && (v[i] == ' ' || v[i] == ',')) ++i;
      if (i >= v.size()) break;
      size_t j = i;
      while (j < v.size() && v[j] != ' ' && v[j] != ',') ++j;
      std::string id = v.substr(i, j - i);
      i = j;
      if (id.front() == '<' && id.back() == '>') id = id.substr(1, id.size() - 2);
      if (id.empty() || id.size() > 250 || id.find('@') == std::string::npos) return false;
      for (unsigned char c : id) {
        if (c <= 0x20 || c >= 0x7f || c == '<' || c == '>') return false;
      }
      if (++n > most) return false;
      ids += (ids.empty() ? "<" : " <") + id + ">";
    }
    *out = ids;
    return true;
  };
  std::string in_reply_to, references;
  if (!message_ids(text_field("in_reply_to"), 1, &in_reply_to)) return fail("in_reply_to is one Message-ID, like <abc@mail.example.com>");
  if (!message_ids(text_field("references"), 20, &references)) return fail("references is up to 20 Message-IDs, separated by spaces");
  if (!in_reply_to.empty() && references.empty()) references = in_reply_to;
  // A reply address that is a capability (038): the host mints reply+<token>@domain on one of
  // the box's token routes, and replies come back to that route with the claims.
  std::string reply_address;
  if (const auto* reply = r.if_contains("reply")) {
    if (!reply_to.empty()) return fail("give reply or reply_to, not both");
    if (!reply->is_object()) return fail("reply is {\"route\": \"ticket_reply\", \"params\": {\"ticket\": \"42\"}}");
    std::string err;
    auto minted = mint_reply_address(ctx, reply->as_object(), to, &err);
    if (!minted) return fail(err);
    reply_to = reply_address = *minted;
  }
  // A template of the tenant's own (design/machine/022.MailTemplates.md), rendered now so a bad
  // one fails the call; never one of the host's, so no box can send what looks like a sign-in code.
  std::string html;
  std::string unsubscribe = unsubscribe_url(site, ctx.tenant, to);
  bj::object vars{{"site", site}, {"email", to}};
  if (r.contains("template")) {
    if (r.contains("subject") || r.contains("text")) return fail("give a template or a subject and text, not both");
    std::string name = text_field("template");
    if (mail::templates::builtin(name)) return fail(name + " is one of the host's own mails; boxes send their own templates");
    if (const auto* v = r.if_contains("vars")) {
      if (!v->is_object()) return fail("vars is an object");
      if (json::dump(*v).size() > 16384) return fail("vars is at most 16 KiB");
      for (const auto& kv : v->as_object()) {
        if (kv.key() != "email") vars[kv.key()] = kv.value();  // the recipient is always to
      }
    }
    vars["unsubscribe"] = unsubscribe;  // never the box's to choose
    std::string err;
    auto rendered = mail_templates_->render(ctx.tenant, name, vars, &err);
    if (!rendered) return fail(err);
    subject = rendered->subject;
    text = rendered->text;
    html = rendered->html;
  } else if (r.contains("vars")) {
    return fail("vars go with a template");
  }
  if (subject.empty() || subject.size() > 200) return fail("subject is 1 to 200 characters");
  if (text.empty() || text.size() > 65536) return fail("text is 1 to 65536 bytes");
  for (char c : subject) {
    if (c == '\r' || c == '\n') return fail("subject is one line");
  }
  if (html.empty()) {
    vars["unsubscribe"] = unsubscribe;
    auto wrapped = mail_templates_->wrap_text(ctx.tenant, subject, text, vars);
    html = wrapped.html;
    text = wrapped.text;
  }
  OutboxElement e;
  e.service = svc.name;
  e.method = m.name;
  e.verb = "MAIL";
  e.url = "mail:" + to;
  e.body = json::dump(bj::object{{"to", to},
                                 {"subject", subject},
                                 {"text", base::valid_utf8(text)},
                                 {"html", base::valid_utf8(html)},
                                 {"from_name", svc.mail_from_name},
                                 {"from_address", from_address},
                                 {"reply_to", reply_to},
                                 {"in_reply_to", in_reply_to},
                                 {"references", references},
                                 {"list_unsubscribe", "<" + unsubscribe + ">"}});
  e.idempotency_key = base::sha256_hex("goober-outbox\n" + ctx.request_id + "\n" + std::to_string(ctx.pending.size()))
                          .substr(0, 32);
  e.attempts = m.attempts;
  e.backoff_ms = m.backoff_ms;
  e.within_ms = m.within_ms;
  std::string key = e.idempotency_key;
  ctx.pending.push_back(std::move(e));
  bj::object accepted{{"accepted", true}, {"key", key}};
  if (!reply_address.empty()) accepted["reply_to"] = reply_address;
  return json::dump(accepted);
}

std::string Host::internal_call(CallContext& ctx, const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                                const bj::value& request) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  auto active = boxes_->active();
  auto it = active->boxes.find(svc.box);
  if (it == active->boxes.end()) return fail("box " + svc.box + " isn't running");
  const manifest::HttpRoute* route = nullptr;
  for (const auto& r : it->second->manifest.internal) {
    if (r.name == m.name) route = &r;
  }
  // Both sides agree at upload; checked again here, since a box can be promoted back.
  if (!route || std::find(route->callers.begin(), route->callers.end(), ctx.manifest->box) == route->callers.end()) {
    return fail(svc.box + "." + m.name + " doesn't take calls from " + ctx.manifest->box);
  }
  if (internal_depth >= kMaxInternalDepth) return fail("internal calls nest deeper than " + std::to_string(kMaxInternalDepth));
  bj::object path_in, query_in;
  bj::value body = nullptr;
  if (request.is_object()) {
    if (auto* p = request.as_object().if_contains("path"); p && p->is_object()) path_in = p->as_object();
    if (auto* q = request.as_object().if_contains("query"); q && q->is_object()) query_in = q->as_object();
    if (auto* b = request.as_object().if_contains("body")) body = *b;
  }
  // The path, from the route's own pattern: typed parameters, checked like a URL's.
  std::map<std::string, std::string> params;
  std::string path;
  for (const auto& seg : route->pattern.segments) {
    if (seg.literal) {
      path += "/" + seg.text;
      continue;
    }
    auto* v = path_in.if_contains(seg.text);
    if (!v) return fail("the call needs path." + seg.text);
    std::string text = text_of(*v);
    if (!router::matches_type(text, seg.type) || text.empty() || text.size() > 1024) {
      return fail("path." + seg.text + " isn't a valid value for the route");
    }
    params[seg.text] = text;
    path += "/" + base::percent_encode(text);
  }
  if (path.empty()) path = "/";
  // Authority never grows: the callee checks the person the call is made for (039).
  std::optional<manifest::Caller> caller;
  if (ctx.who) caller = manifest::Caller{std::to_string(ctx.who->user.user_id), ctx.who->host, ctx.who->roles};
  if (!route->is_public || !route->requirements.empty()) {
    if (manifest::check_access(*route, caller ? &*caller : nullptr, params, ctx.tenant) != manifest::Access::kAllowed) {
      return json::dump(bj::object{{"status", 403}, {"error", "the person this call is for may not use " + m.name}});
    }
  }
  const auto& callee = *it->second;
  std::string body_text;
  if (std::string type = types::json_type_name(route->body); !type.empty()) {
    std::string err;
    auto checked = callee.types ? callee.types->check(type, body, &err) : std::nullopt;
    if (!checked) return json::dump(bj::object{{"status", 400}, {"error", type + " " + err}});
    body_text = json::dump(*checked);
  } else if (!body.is_null()) {
    body_text = text_of(body);
  }
  bj::object query;
  if (route->query_declared) {
    for (const auto& [name, schema] : route->query) {
      auto* v = query_in.if_contains(name);
      if (!v) {
        if (schema->default_value) query[name] = *schema->default_value;
        continue;
      }
      std::string err;
      auto checked = types::check_text(*schema, text_of(*v), &err);
      if (!checked) return json::dump(bj::object{{"status", 400}, {"error", "query." + name + " " + err}});
      query[name] = std::move(*checked);
    }
  } else {
    for (const auto& kv : query_in) query[kv.key()] = text_of(kv.value());
  }
  uint8_t rnd[8];
  base::os_random_fill(rnd);
  std::string request_id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
  bj::object params_json;
  for (const auto& [k, v] : params) params_json[k] = v;
  bj::object doc{{"request_id", request_id},
                 {"method", route->verb},
                 {"path", path},
                 {"params", params_json},
                 {"query", query},
                 {"headers", bj::object{}},
                 {"body", body_text},
                 {"domain", ctx.domain},
                 {"tenant", ctx.tenant},
                 {"user", ctx.user},
                 // !caller_box and !caller_route: who made the call (039).
                 {"caller", bj::object{{"box", ctx.manifest->box}, {"route", ctx.route}, {"request_id", ctx.request_id}}}};
  // Deadlines only shrink: the callee gets what's left of the caller's time, at most its own.
  CallOptions options;
  int64_t left_ms = (ctx.deadline_us - base::monotonic_us()) / 1000;
  if (left_ms < 10) return fail("no time left for the call");
  auto cfg = config_.get();
  options.timeout_ms = std::min(left_ms, cfg->timeout_ms.resolve(callee.manifest.behavior.timeout_ms));
  ++internal_depth;
  int64_t started = base::monotonic_us();
  BoxCall call;
  try {
    call = call_box(boxes::RouteRef{it->second, route}, doc, ctx.tenant, request_id, ctx.who, options);
  } catch (...) {
    --internal_depth;
    throw;
  }
  --internal_depth;
  const auto& r = call.result;
  int status = r.outcome == runtime::wasm::CallResult::Outcome::kOk ? r.status : 500;
  call.extra["internal"] = ctx.manifest->box + " -> " + svc.box;
  call.extra["caller_request_id"] = ctx.request_id;
  server::http::Request pseudo;
  pseudo.method = route->verb;
  pseudo.path = "internal:" + svc.box + path;
  pseudo.headers.emplace_back("goober-request-id", request_id);
  access_log(pseudo, status, started, ctx.domain, json::dump(call.extra));
  if (status >= 500) return json::dump(bj::object{{"status", status}, {"error", svc.box + "." + m.name + " failed"}});
  bj::object result{{"status", status}, {"content_type", r.content_type}, {"body", r.body}};
  if (r.content_type.starts_with("application/json")) {
    std::string err;
    if (auto v = json::parse(r.body, &err)) result["json"] = std::move(*v);
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
