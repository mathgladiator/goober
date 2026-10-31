// Mail in (038.Mail.md; design/machine/024.MailIn.md): Stalwart speaks SMTP to the world and
// calls this host's MTA hook, /internal/routes/mta/stalwart, at RCPT and DATA. At RCPT the host
// finds the mail route that claims the recipient, or refuses it at the door. At DATA it parses
// the message, keeps attachments out of the box, drops automatic mail for routes that didn't
// opt in, and runs the route's box, whose answer becomes the SMTP answer: accept is 250 (the
// host says "discard" to Stalwart, which keeps nothing), reject is 550, defer is 451.
//
// Reply addresses are capabilities: a box sending mail asks for a reply address on one of its
// token routes, and the host mints reply+<token>@domain, where the token is a random id and a
// MAC under a host key. The claims live in storage/mail/replies.db, never in the address.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/libraries/libraries.h"
#include "goober/host/host.h"
#include "goober/mail_inbound/reply.h"
#include "mail_hash.h"
#include "goober/json/json.h"
#include "goober/mail_inbound/address.h"
#include "goober/mail_inbound/dkim.h"
#include "goober/mail_inbound/mime.h"
#include "dns.h"
#include "network.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
namespace inbound = mail::inbound;
using server::http::Request;
using server::http::Response;

namespace {

constexpr int64_t kReplyTtlMs = 180LL * 24 * 3600 * 1000;
constexpr size_t kAttachmentReadMax = 8 << 20;

Response hook_json(const bj::object& o) {
  Response res;
  res.status = 200;
  res.headers.emplace_back("content-type", "application/json");
  res.body = json::dump(o);
  return res;
}

bj::object hook_accept() { return bj::object{{"action", "accept"}}; }
// Stalwart answers 250 and keeps nothing: the box has the message.
bj::object hook_discard() {
  return bj::object{{"action", "discard"},
                    {"response", bj::object{{"status", 250}, {"enhanced_status", "2.0.0"}, {"message", "Delivered"}}}};
}
bj::object hook_reject(int status, const std::string& enhanced, const std::string& message) {
  // One line of printable ASCII: it goes into the SMTP reply as it is.
  std::string clean;
  for (char c : message) {
    if (c >= 0x20 && c < 0x7f) clean += c;
    if (clean.size() >= 200) break;
  }
  if (clean.empty()) clean = "Refused";
  return bj::object{{"action", "reject"},
                    {"response", bj::object{{"status", status}, {"enhanced_status", enhanced}, {"message", clean}}}};
}

bool loopback(const std::string& remote) {
  return remote.starts_with("127.") || remote == "::1" || remote.starts_with("::ffff:127.");
}

std::string base32(std::string_view bytes) {
  static const char* kAlphabet = "abcdefghijklmnopqrstuvwxyz234567";
  std::string out;
  uint32_t buffer = 0;
  int bits = 0;
  for (unsigned char c : bytes) {
    buffer = (buffer << 8) | c;
    bits += 8;
    while (bits >= 5) {
      out += kAlphabet[(buffer >> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out += kAlphabet[(buffer << (5 - bits)) & 31];
  return out;
}

std::optional<std::string> unbase32(std::string_view text) {
  std::string out;
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : text) {
    int v = c >= 'a' && c <= 'z' ? c - 'a' : c >= '2' && c <= '7' ? c - '2' + 26 : -1;
    if (v < 0) return std::nullopt;
    buffer = (buffer << 5) | uint32_t(v);
    bits += 5;
    if (bits >= 8) {
      out += char((buffer >> (bits - 8)) & 0xff);
      bits -= 8;
    }
  }
  return out;
}

// The address a pattern makes with these values, for minting reply addresses.
std::optional<std::string> fill_local(const inbound::AddressPattern& p, const std::map<std::string, std::string>& values) {
  std::string out;
  for (const auto& part : p.local) {
    if (!part.param) {
      out += part.text;
      continue;
    }
    auto it = values.find(part.text);
    if (it == values.end()) return std::nullopt;
    out += it->second;
  }
  return out;
}

bool param_value_ok(const std::string& v) {
  if (v.empty() || v.size() > 64) return false;
  for (char c : v) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}

}  // namespace

// The host's key for reply tokens: 32 random bytes, made on first use, readable only by the host.
// Read once and kept: it is asked for on every box call and log line.
std::string Host::reply_key() {
  std::lock_guard lock(reply_key_mu_);
  if (reply_key_.empty()) reply_key_ = load_reply_key();
  return reply_key_;
}

std::string Host::load_reply_key() {
  auto file = config_.get()->storage / "mail" / "reply.key";
  if (auto k = base::read_file(file); k && k->size() == 32) return *k;
  fs::create_directories(file.parent_path());
  uint8_t raw[32];
  base::os_random_fill(raw);
  std::string key(reinterpret_cast<char*>(raw), sizeof raw);
  int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return base::read_file(file).value_or(key);  // another thread made it first
  bool ok = ::write(fd, key.data(), key.size()) == ssize_t(key.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) throw std::runtime_error("couldn't write " + file.string());
  return key;
}

std::unique_ptr<sqlite::Db> Host::open_mail_db() {
  auto dir = config_.get()->storage / "mail";
  fs::create_directories(dir);
  auto db = std::make_unique<sqlite::Db>(dir / "inbound.db");
  db->exec(
      "CREATE TABLE IF NOT EXISTS replies (id TEXT PRIMARY KEY, tenant TEXT NOT NULL, box TEXT NOT NULL, "
      "route TEXT NOT NULL, params TEXT NOT NULL, recipient TEXT NOT NULL, created_ms INTEGER NOT NULL, "
      "expires_ms INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS attachments (id INTEGER PRIMARY KEY AUTOINCREMENT, tenant TEXT NOT NULL, "
      "box TEXT NOT NULL, route TEXT NOT NULL, received_ms INTEGER NOT NULL, filename TEXT NOT NULL, "
      "content_type TEXT NOT NULL, size INTEGER NOT NULL, sha256 TEXT NOT NULL, content_id TEXT NOT NULL, "
      "inline INTEGER NOT NULL, bytes BLOB NOT NULL);"
      "CREATE INDEX IF NOT EXISTS attachments_by_age ON attachments (received_ms);"
      // Who wrote to a tenant, and which message, both as hashes: what lets a box's answer in
      // that thread count as a reply (mail.reply-per-address-daily) and not as outreach.
      "CREATE TABLE IF NOT EXISTS received (tenant TEXT NOT NULL, sender TEXT NOT NULL, message TEXT NOT NULL, "
      "at_ms INTEGER NOT NULL, PRIMARY KEY (tenant, sender, message));");
  return db;
}

std::optional<std::string> Host::mint_reply_address(const CallContext& ctx, const bj::object& reply,
                                                    const std::string& recipient, std::string* error) {
  auto route_v = reply.if_contains("route");
  if (!route_v || !route_v->is_string()) return *error = "reply names the route, like {\"route\": \"ticket_reply\", \"params\": {...}}", std::nullopt;
  std::string route_name(route_v->as_string());
  const manifest::HttpRoute* route = nullptr;
  for (const auto& r : ctx.manifest->mail) {
    if (r.name == route_name) route = &r;
  }
  if (!route) return *error = route_name + " isn't one of this box's mail routes", std::nullopt;
  std::string err;
  auto pattern = inbound::parse_address_pattern(route->mail_address, &err);
  std::string token_param = pattern ? pattern->token_param() : "";
  if (token_param.empty()) return *error = route_name + "'s address has no {name : token} part to mint", std::nullopt;
  std::string domain = pattern->domain.empty() ? ctx.domain : pattern->domain;
  if (!config_.get()->domains.count(domain) || !inbound::valid_domain(domain)) {
    return *error = "replies need a mail domain; " + domain + " isn't one in the host config", std::nullopt;
  }
  std::map<std::string, std::string> params;
  if (const auto* p = reply.if_contains("params"); p && p->is_object()) {
    for (const auto& kv : p->as_object()) {
      std::string v = kv.value().is_string() ? std::string(kv.value().as_string()) : json::dump(kv.value());
      if (v.size() > 256) return *error = "a reply param is at most 256 bytes", std::nullopt;
      params[std::string(kv.key())] = v;
    }
  }
  if (!params.count(token_param)) return *error = "reply.params needs " + token_param + ", the token's claim", std::nullopt;
  // The address holds the plain params as they are; the token's claim stays in the host.
  std::map<std::string, std::string> in_address;
  for (const auto& part : pattern->local) {
    if (!part.param || part.text == token_param) continue;
    auto it = params.find(part.text);
    if (it == params.end() || !param_value_ok(it->second)) {
      return *error = "reply.params." + part.text + " is required in the address: [a-z0-9._-], 1 to 64", std::nullopt;
    }
    in_address[part.text] = it->second;
  }
  uint8_t raw[8];
  base::os_random_fill(raw);
  std::string id(reinterpret_cast<char*>(raw), sizeof raw);
  auto mac = base::hmac_sha256(reply_key(), "goober-reply\n" + id);
  std::string token = base32(id + std::string(reinterpret_cast<const char*>(mac.data()), 7));
  in_address[token_param] = token;
  auto local = fill_local(*pattern, in_address);
  if (!local || local->size() > 64) return *error = "the reply address would be longer than 64 characters", std::nullopt;
  bj::object stored;
  for (const auto& [k, v] : params) stored[k] = v;
  int64_t now = base::now_unix_ms();
  {
    std::lock_guard lock(mail_in_mu_);
    auto db = open_mail_db();
    sqlite::Stmt s(*db, "INSERT INTO replies (id, tenant, box, route, params, recipient, created_ms, expires_ms) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    s.bind(1, base::hex_encode(id)).bind(2, ctx.tenant).bind(3, ctx.manifest->box).bind(4, route_name);
    s.bind(5, json::dump(stored)).bind(6, recipient).bind(7, now).bind(8, now + kReplyTtlMs).run();
  }
  return *local + "@" + domain;
}

std::optional<Host::MailMatch> Host::match_mail(const std::string& address, std::string* why,
                                                std::shared_ptr<const boxes::ActiveBox> only) {
  auto parts = inbound::split_address(address);
  if (!parts) return *why = "not an address", std::nullopt;
  auto [local, domain] = *parts;
  auto cfg = config_.get();
  auto entry = cfg->domains.find(domain);
  if (entry == cfg->domains.end()) return *why = "not a domain of this host", std::nullopt;
  auto active = boxes_->active();
  struct Candidate {
    boxes::RouteRef ref;
    inbound::AddressPattern pattern;
    std::map<std::string, std::string> params;
    int score;
  };
  std::optional<Candidate> best;
  std::vector<std::shared_ptr<const boxes::ActiveBox>> boxes;
  if (only) boxes.push_back(only);
  else for (const auto& [name, b] : active->boxes) boxes.push_back(b);
  for (const auto& box : boxes) {
    if (!box->manifest.applies_to(domain)) continue;
    for (const auto& r : box->manifest.mail) {
      std::string err;
      auto p = inbound::parse_address_pattern(r.mail_address, &err);
      if (!p || (!p->domain.empty() && p->domain != domain)) continue;
      auto params = inbound::match_local(*p, local);
      if (!params) continue;
      // Specific before catch-all (038); then a named domain, then more literal text.
      size_t literal = 0;
      for (const auto& part : p->local) literal += part.param ? 0 : part.text.size();
      int score = (p->catch_all ? 0 : 1 << 20) + (p->domain.empty() ? 0 : 1 << 10) + int(literal);
      if (!best || score > best->score) best = Candidate{boxes::RouteRef{box, &r}, *p, *params, score};
    }
  }
  if (!best) return *why = "no mail route claims it", std::nullopt;
  MailMatch m{best->ref, entry->second.tenant, domain, address, best->params, {}, ""};
  // A token is checked before anything else happens: a forged one never wakes the box.
  if (std::string tp = best->pattern.token_param(); !tp.empty()) {
    auto raw = unbase32(best->params[tp]);
    if (!raw || raw->size() != 15) return *why = "bad reply token", std::nullopt;
    std::string id = raw->substr(0, 8);
    auto mac = base::hmac_sha256(reply_key(), "goober-reply\n" + id);
    if (!base::equal_constant_time(raw->substr(8), std::string_view(reinterpret_cast<const char*>(mac.data()), 7))) {
      return *why = "bad reply token", std::nullopt;
    }
    std::lock_guard lock(mail_in_mu_);
    auto db = open_mail_db();
    sqlite::Stmt s(*db, "SELECT tenant, box, route, params, recipient FROM replies WHERE id = ? AND expires_ms > ?");
    s.bind(1, base::hex_encode(id)).bind(2, base::now_unix_ms());
    if (!s.step() || s.text(0) != m.tenant || s.text(1) != best->ref.box->manifest.box || s.text(2) != best->ref.route->name) {
      return *why = "reply token unknown or expired", std::nullopt;
    }
    std::string err;
    if (auto claims = json::parse(s.text(3), &err); claims && claims->is_object()) {
      for (const auto& kv : claims->as_object()) {
        m.params[std::string(kv.key())] = kv.value().is_string() ? std::string(kv.value().as_string()) : json::dump(kv.value());
      }
    }
    m.reply_recipient = s.text(4);
  }
  return m;
}

void Host::handle_mail_hook(const Request& req, const Finish& finish) {
  auto cfg = config_.get();
  const auto& in = cfg->mail.inbound;
  auto not_found = [&](const char* why) {
    Response res;
    res.status = 404;
    return finish(std::move(res), json::dump(bj::object{{"mail_in", "refused"}, {"why", why}}));
  };
  // Only Stalwart, on this machine, with its token: never anything that came through Caddy.
  if (!in.enabled()) return not_found("mail.inbound isn't configured");
  if (!loopback(req.remote) || !req.header("x-forwarded-for").empty() || !req.header("via").empty() ||
      !req.header("forwarded").empty()) {
    return not_found("not from this machine");
  }
  auto token = base::read_file(in.token_file);
  std::string expected = token ? std::string(token->substr(0, token->find_first_of("\r\n"))) : "";
  if (expected.size() < 32 || !base::equal_constant_time(req.header("authorization"), "Bearer " + expected)) {
    return not_found("wrong token");
  }
  if (req.method != "post") return not_found("not a POST");
  std::string err;
  auto parsed = json::parse(req.body, &err);
  if (!parsed || !parsed->is_object()) return finish(hook_json(hook_reject(451, "4.3.0", "Try again later")), R"({"mail_in":"unreadable"})");
  const auto& o = parsed->as_object();
  auto str = [](const bj::value* v) { return v && v->is_string() ? std::string(v->as_string()) : std::string(); };
  std::string stage = str(o.contains("context") ? o.at("context").as_object().if_contains("stage") : nullptr);
  std::string from;
  std::vector<std::string> to;
  if (const auto* env = o.if_contains("envelope"); env && env->is_object()) {
    if (const auto* f = env->as_object().if_contains("from"); f && f->is_object()) from = base::to_lower(str(f->as_object().if_contains("address")));
    if (const auto* t = env->as_object().if_contains("to"); t && t->is_array()) {
      for (const auto& a : t->as_array()) {
        if (a.is_object()) to.push_back(base::to_lower(str(a.as_object().if_contains("address"))));
      }
    }
  }
  bj::object log{{"mail_in", stage}, {"from", from}};
  auto answer = [&](const bj::object& response, const char* outcome) {
    log["outcome"] = outcome;
    return finish(hook_json(response), json::dump(log));
  };
  if (stage != "rcpt" && stage != "data") return answer(hook_accept(), "pass");
  if (to.empty()) return answer(hook_reject(503, "5.5.1", "No recipient"), "no_recipient");
  log["to"] = to.back();
  // One recipient per message: one route decides, so its answer can be the SMTP answer. A
  // sender sends the rest again in another transaction (RFC 5321 § 4.5.3.1.10).
  if (to.size() > 1) return answer(hook_reject(452, "4.5.3", "One recipient per message here; send the others separately"), "one_recipient");
  std::string why;
  auto match = match_mail(to.back(), &why);
  if (!match) {
    log["why"] = why;
    return answer(hook_reject(550, "5.1.1", "No such recipient here"), "no_route");
  }
  const auto& route = *match->ref.route;
  const auto& box = *match->ref.box;
  log["box"] = box.manifest.box;
  log["route"] = route.name;
  log["tenant"] = match->tenant;
  if (stage == "rcpt") return answer(hook_accept(), "rcpt_accepted");

  // DATA: the whole message.
  const auto* msg = o.if_contains("message");
  if (!msg || !msg->is_object()) return answer(hook_reject(451, "4.3.0", "Try again later"), "no_message");
  const auto& mo = msg->as_object();
  int64_t size = mo.contains("size") && mo.at("size").is_int64() ? mo.at("size").as_int64() : int64_t(req.body.size());
  log["bytes"] = size;
  if (size > std::min<int64_t>(*route.max_body_bytes, in.max_bytes)) {
    return answer(hook_reject(552, "5.3.4", "Message too big for this address"), "too_big");
  }
  inbound::Headers headers;
  if (const auto* hs = mo.if_contains("headers"); hs && hs->is_array()) {
    for (const auto& h : hs->as_array()) {
      if (h.is_array() && h.as_array().size() == 2 && h.as_array()[0].is_string() && h.as_array()[1].is_string()) {
        headers.emplace_back(std::string(h.as_array()[0].as_string()), std::string(h.as_array()[1].as_string()));
      }
    }
  }
  std::string body = str(mo.if_contains("contents"));
  auto message = inbound::parse_message(headers, body);
  bool automatic = inbound::is_automatic(headers, from);
  if (automatic && !route.mail_automatic) {
    // Taken and dropped: answering a bounce or an auto-reply is how loops start (038).
    return answer(hook_discard(), "automatic_dropped");
  }
  if (int64_t(message.attachments.size()) > route.mail_attachments) {
    return answer(hook_reject(550, "5.3.4", route.mail_attachments == 0 ? "This address takes no attachments"
                                                                         : "This address takes at most " +
                                                                               std::to_string(route.mail_attachments) +
                                                                               " attachments"),
                  "too_many_attachments");
  }
  // Attachments go to the host's store; the box gets handles (038 § Attachments).
  int64_t now = base::now_unix_ms();
  std::vector<int64_t> stored;
  bj::array attachments;
  {
    std::lock_guard lock(mail_in_mu_);
    auto db = open_mail_db();
    for (const auto& a : message.attachments) {
      sqlite::Stmt s(*db, "INSERT INTO attachments (tenant, box, route, received_ms, filename, content_type, size, sha256, "
                          "content_id, inline, bytes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
      s.bind(1, match->tenant).bind(2, box.manifest.box).bind(3, route.name).bind(4, now).bind(5, a.filename);
      s.bind(6, a.content_type).bind(7, int64_t(a.bytes.size())).bind(8, base::sha256_hex(a.bytes));
      s.bind(9, a.content_id).bind(10, int64_t(a.inline_part)).bind_blob(11, a.bytes).run();
      int64_t id = db->last_insert_rowid();
      stored.push_back(id);
      attachments.push_back(bj::object{{"id", std::to_string(id)},
                                       {"filename", a.filename},
                                       {"content_type", a.content_type},
                                       {"bytes", int64_t(a.bytes.size())},
                                       {"inline", a.inline_part},
                                       {"content_id", a.content_id}});
    }
  }
  auto drop_attachments = [&] {
    if (stored.empty()) return;
    std::lock_guard lock(mail_in_mu_);
    auto db = open_mail_db();
    for (int64_t id : stored) {
      sqlite::Stmt s(*db, "DELETE FROM attachments WHERE id = ?");
      s.bind(1, id).run();
    }
  };

  std::string request_id = req.header("goober-request-id");
  bj::object doc = mail_route_doc(*match, headers, body, message, from, size, automatic, std::move(attachments), request_id,
                                  &log);
  CallOptions options;
  options.timeout_ms = route.timeout_ms;
  options.no_response_ok = true;
  auto call = call_box(match->ref, doc, match->tenant, request_id, std::nullopt, options);
  const auto& r = call.result;
  log["status"] = r.status;
  log["box_outcome"] = runtime::wasm::outcome_name(r.outcome);
  std::string outcome;
  auto reply = mail_hook_reply(r, &outcome);
  // A box that ran and refused or put off the mail chose to (028 § 4.3): an answer, kept apart
  // from failures, which call_box recorded.
  if (failures_ && r.outcome == runtime::wasm::CallResult::Outcome::kOk &&
      (outcome == "box_rejected" || outcome == "box_deferred")) {
    operations::Failures::Event e;
    e.at_ms = base::now_unix_ms();
    e.tenant = match->tenant;
    e.box = box.manifest.box;
    e.version = box.version;
    e.route = route.name;
    e.kind = "mail";
    e.cls = outcome == "box_rejected" ? "mail_rejected" : "mail_deferred";
    e.message = "the box answered " + std::to_string(r.status);
    e.request_id = request_id;
    failures_->failure(std::move(e));
  }
  if (outcome == "delivered") {
    if (message.from && !message.message_id.empty()) {
      try {
        auto db = open_mail_db();
        // Kept 90 days: who wrote in is a relationship for transactional mail (029 § 3); a reply's
        // thread is checked against the last 30.
        db->exec("DELETE FROM received WHERE at_ms < " + std::to_string(base::now_unix_ms() - 90LL * 86400000));
        sqlite::Stmt s(*db, "INSERT OR IGNORE INTO received (tenant, sender, message, at_ms) VALUES (?, ?, ?, ?)");
        s.bind(1, match->tenant)
            .bind(2, received_hash(message.from->address))
            .bind(3, received_hash(message.message_id))
            .bind(4, base::now_unix_ms())
            .run();
      } catch (const std::exception& e) {
        std::cerr << "goober-host: mail in: couldn't note the sender: " << e.what() << "\n";
      }
    }
  } else {
    drop_attachments();
  }
  return answer(reply, outcome.c_str());
}

bj::object Host::mail_route_doc(const MailMatch& m, const inbound::Headers& headers, const std::string& body,
                                const inbound::Message& message, const std::string& from, int64_t size, bool automatic,
                                bj::array attachments, const std::string& request_id, bj::object* log) {
  // The sender, as a verified claim or not at all (038): see design/machine/024.MailIn.md § 5.
  // A verified sender with an account in this tenant's realm (not deleted, not disabled) is
  // named by its user id; the box still runs as nobody (decided 2026-10-31, 038 § 10).
  std::string sender_why;
  std::string sender = verified_sender(headers, body, &sender_why);
  std::optional<int64_t> from_user_id;
  if (!sender.empty()) {
    try {
      if (auto u = identity_->active_user(m.tenant, sender)) from_user_id = u->user_id;
    } catch (const std::exception& e) {
      std::cerr << "goober-host: mail in: looking up the sender: " << e.what() << "\n";
    }
  }
  (*log)["sender_verified"] = !sender.empty();
  if (from_user_id) (*log)["from_user_id"] = std::to_string(*from_user_id);
  bj::value from_user = from_user_id ? bj::value(std::to_string(*from_user_id)) : bj::value(nullptr);
  auto mailbox = [](const inbound::Mailbox& b) { return bj::object{{"address", b.address}, {"name", b.name}}; };
  auto mailboxes = [&](const std::vector<inbound::Mailbox>& l) {
    bj::array a;
    for (const auto& b : l) a.push_back(mailbox(b));
    return a;
  };
  bj::array refs;
  for (const auto& r : message.references) refs.push_back(bj::value(r));
  bj::object typed{{"to", m.to},
                   {"envelope_from", from},
                   {"from", message.from ? bj::value(mailbox(*message.from)) : bj::value(nullptr)},
                   {"sender", sender.empty() ? bj::value(nullptr) : bj::value(sender)},
                   {"sender_why", sender.empty() ? bj::value(sender_why) : bj::value(nullptr)},
                   {"from_user_id", from_user},
                   {"to_list", mailboxes(message.to)},
                   {"cc", mailboxes(message.cc)},
                   {"reply_to", mailboxes(message.reply_to)},
                   {"subject", message.subject},
                   {"text", message.text},
                   {"reply_text", inbound::reply_text(message.text)},
                   {"html", message.html},
                   {"message_id", message.message_id},
                   {"in_reply_to", message.in_reply_to},
                   {"references", std::move(refs)},
                   {"date", message.date},
                   {"attachments", std::move(attachments)},
                   {"automatic", automatic},
                   {"truncated", message.truncated},
                   {"bytes", size}};
  // A calendar in the message (an RSVP, a proposal, an invite), as ics@1's interpret reads it
  // for the address it came to (design/machine/831.Libraries.md § 6).
  for (const auto& part : message.attachments) {
    if (part.content_type != "text/calendar" && part.content_type != "application/ics" && !part.filename.ends_with(".ics")) continue;
    libraries::Context lctx{base::now_unix_ms()};
    std::string out = libraries::call(*libraries::find("ics", 1), "interpret",
                                      json::dump(bj::object{{"ics", part.bytes}, {"me", m.to}}), lctx);
    std::string perr;
    if (auto v = json::parse(out, &perr); v && v->is_object() && !v->as_object().contains("error")) {
      bj::object meaning = std::move(v->as_object());
      meaning["ics"] = part.bytes.size() <= 262144 ? bj::value(part.bytes) : bj::value(nullptr);
      typed["calendar"] = std::move(meaning);
      break;
    }
  }
  if (!m.reply_recipient.empty()) {
    // Whether the reply came from the address the token was sent to: a forwarded token still
    // works, but the box can tell.
    typed["reply_from_recipient"] = !sender.empty() && sender == m.reply_recipient;
  }
  bj::object params;
  for (const auto& [k, v] : m.params) params[k] = v;
  return bj::object{{"request_id", request_id},
                 {"method", "receive"},
                 {"path", "mail:" + m.to},
                 {"params", std::move(params)},
                 {"query", bj::object{}},
                 {"headers", bj::object{}},
                 {"body", json::dump(typed)},
                 {"domain", m.domain},
                 {"tenant", m.tenant},
                 {"user", nullptr},
                 {"mail", bj::object{{"to", m.to},
                                     {"sender", sender.empty() ? bj::value(nullptr) : bj::value(sender)},
                                     {"from_user_id", from_user}}}};
}

bj::object Host::mail_hook_reply(const runtime::wasm::CallResult& r, std::string* outcome) {
  using Outcome = runtime::wasm::CallResult::Outcome;
  auto box_message = [&](const char* fallback) {
    std::string e;
    if (auto v = json::parse(r.body, &e); v && v->is_object() && v->as_object().contains("error") &&
                                          v->as_object().at("error").is_string()) {
      return std::string(v->as_object().at("error").as_string());
    }
    return std::string(fallback);
  };
  if (r.outcome == Outcome::kNoResponse || (r.outcome == Outcome::kOk && r.status >= 200 && r.status < 300)) {
    *outcome = "delivered";
    return hook_discard();
  }
  if (r.outcome == Outcome::kOk && r.status >= 400 && r.status < 500 && r.status != 429) {
    *outcome = "box_rejected";
    return hook_reject(550, "5.7.1", box_message("Refused by the recipient"));
  }
  // 429, 503, a failure, a trap, a timeout: the sender tries again later, so nothing is lost.
  *outcome = "box_deferred";
  return hook_reject(451, "4.3.0", r.outcome == Outcome::kOk ? box_message("Try again later") : "Try again later");
}

std::string Host::mail_attachment_call(CallContext& ctx, const bj::value& request) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  const auto* id_v = request.is_object() ? request.as_object().if_contains("id") : nullptr;
  int64_t id = 0;
  if (id_v && id_v->is_string()) {
    try {
      id = std::stoll(std::string(id_v->as_string()));
    } catch (...) {
    }
  }
  if (id <= 0) return fail("attachment takes {\"id\": \"...\"}, an id from a received message");
  std::lock_guard lock(mail_in_mu_);
  auto db = open_mail_db();
  // Only the box that received it, in the tenant it arrived for.
  sqlite::Stmt s(*db, "SELECT filename, content_type, size, bytes FROM attachments WHERE id = ? AND tenant = ? AND box = ?");
  s.bind(1, id).bind(2, ctx.tenant).bind(3, ctx.manifest->box);
  if (!s.step()) return fail("no such attachment for this box (they're kept " +
                             std::to_string(config_.get()->mail.inbound.attachment_days) + " days)");
  if (size_t(s.int64(2)) > kAttachmentReadMax) return fail("the attachment is past 8 MiB, more than a box reads at once");
  return json::dump(bj::object{{"filename", s.text(0)},
                               {"content_type", s.text(1)},
                               {"bytes", s.int64(2)},
                               {"base64", base::base64_encode(s.blob(3))}});
}

std::string Host::verified_sender(const inbound::Headers& headers, const std::string& body, std::string* why) {
  // Stalwart 1.0.1's hook carries no authentication results (§ 5), but it hands over the
  // headers and body exactly as sent, so the host verifies DKIM itself.
  inbound::KeyLookup lookup = dkim_dns_ ? inbound::KeyLookup(dkim_dns_) : inbound::KeyLookup([](const std::string& name, std::string* e) {
    return dns::lookup_txt(name, e);
  });
  return inbound::verified_author(headers, body, lookup, base::now_unix_ms() / 1000, why);
}

int Host::prune_mail(int64_t now_ms) {
  auto cfg = config_.get();
  std::lock_guard lock(mail_in_mu_);
  if (!fs::exists(cfg->storage / "mail" / "inbound.db")) return 0;
  auto db = open_mail_db();
  sqlite::Stmt a(*db, "DELETE FROM attachments WHERE received_ms < ?");
  a.bind(1, now_ms - cfg->mail.inbound.attachment_days * 24 * 3600 * 1000).run();
  int gone = int(db->changes());
  sqlite::Stmt r(*db, "DELETE FROM replies WHERE expires_ms < ?");
  r.bind(1, now_ms).run();
  return gone;
}

}  // namespace goober::host
