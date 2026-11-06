// The operators' mailbox (design/machine/055.OperatorMail.md): mail to admin@, postmaster@, and
// abuse@ on the host's production domains comes to the operators, before any box's mail route
// (041 reserves postmaster@ and abuse@ for them). It is kept in storage/mail/operators/, read in
// the panel's Mail view, and answered from there as any of those addresses through the host's
// mailer. Only people read and send it: the panel's own calls, never an MCP tool.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/mail_inbound/address.h"
#include "goober/mail_inbound/mime.h"
#include "mailbox.h"
#include "mailer.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
namespace inbound = mail::inbound;

namespace {

constexpr int64_t kKeepMs = 400LL * 86400000;  // kept 400 days, as the logs are
constexpr size_t kTextMax = 1 << 20;            // a message's text and html, each, cut past this
constexpr size_t kSubjectMax = 998;
constexpr size_t kSendTextMax = 256 * 1024;

const char* const kLocals[] = {"admin", "postmaster", "abuse"};

std::string text_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

bj::object head_of(const bj::object& m) {
  std::string body = text_of(m, "text");
  bj::object row{{"id", m.at("id")}, {"at_ms", m.at("at_ms")}, {"direction", m.at("direction")},
                 {"from", m.at("from")}, {"from_name", m.contains("from_name") ? m.at("from_name") : bj::value("")},
                 {"to", m.at("to")}, {"subject", m.at("subject")},
                 {"preview", body.size() > 160 ? mailbox::cut_utf8(body, 160) + "…" : body}};
  for (const char* k : {"sender_verified", "automatic", "sent", "error"}) {
    if (m.contains(k)) row[k] = m.at(k);
  }
  if (const auto* a = m.if_contains("attachments"); a && a->is_array()) row["attachments"] = int64_t(a->as_array().size());
  return row;
}

}  // namespace

fs::path Host::operator_mailbox_dir() const { return config_.get()->storage / "mail" / "operators"; }

// The tenant whose domain an address is on ("" for the platform's own), or nullopt when it isn't
// one of the operators' addresses.
std::optional<std::string> Host::operator_address_tenant(const std::string& address) const {
  auto parts = inbound::split_address(base::to_lower(address));
  if (!parts) return std::nullopt;
  const auto& [local, domain] = *parts;
  if (std::find(std::begin(kLocals), std::end(kLocals), local) == std::end(kLocals)) return std::nullopt;
  auto cfg = config_.get();
  std::string primary = domain;
  if (auto r = cfg->redirects.find(domain); r != cfg->redirects.end()) primary = r->second;
  if (auto it = cfg->domains.find(primary); it != cfg->domains.end()) {
    // Beta takes no mail from outside (053 § 4).
    if (!it->second.beta_of.empty()) return std::nullopt;
    return it->second.tenant;
  }
  if (!cfg->platform_domain.empty() && domain == cfg->platform_domain) return std::string();
  return std::nullopt;
}

bj::array Host::operator_mail_addresses(const OperatorScope& scope) const {
  auto cfg = config_.get();
  bj::array out;
  auto add = [&](const std::string& domain, const std::string& tenant) {
    if (!scope.all && (tenant.empty() || !scope.tenants.count(tenant))) return;
    for (const char* l : kLocals) out.push_back(bj::value(std::string(l) + "@" + domain));
  };
  for (const auto& [d, e] : cfg->domains) {
    if (e.beta_of.empty() && inbound::valid_domain(d)) add(d, e.tenant);
  }
  if (!cfg->platform_domain.empty() && !cfg->domains.count(cfg->platform_domain)) add(cfg->platform_domain, "");
  return out;
}

bool Host::operator_mailbox_full() {
  return mailbox::bytes(operator_mailbox_dir()) >= config_.get()->disk.operator_mail_bytes;
}

std::string Host::operator_mail_receive(const std::string& to, const std::string& envelope_from, const inbound::Headers& headers,
                                        const std::string& body, int64_t size) {
  auto message = inbound::parse_message(headers, body);
  std::string why;
  std::string sender = verified_sender(headers, body, &why);
  int64_t now = base::now_unix_ms();
  std::string id = mailbox::new_id(now);
  fs::path dir = operator_mailbox_dir();
  auto boxes = [](const std::vector<inbound::Mailbox>& l) {
    bj::array a;
    for (const auto& b : l) a.push_back(bj::object{{"address", b.address}, {"name", b.name}});
    return a;
  };
  std::string references;
  for (const auto& r : message.references) references += (references.empty() ? "" : " ") + r;
  bj::object m{{"id", id},
               {"at_ms", now},
               {"direction", "in"},
               {"to", base::to_lower(to)},
               {"envelope_from", envelope_from},
               {"from", message.from ? message.from->address : envelope_from},
               {"from_name", message.from ? message.from->name : ""},
               {"to_list", boxes(message.to)},
               {"cc", boxes(message.cc)},
               {"reply_to", message.reply_to.empty() ? "" : message.reply_to.front().address},
               {"subject", message.subject},
               {"text", mailbox::cut_utf8(message.text, kTextMax)},
               {"html", mailbox::cut_utf8(message.html, kTextMax)},
               {"message_id", message.message_id},
               {"in_reply_to", message.in_reply_to},
               {"references", references},
               {"date", message.date},
               {"bytes", size},
               // Whether From is who wrote it: DKIM aligned with From's domain (024 § 5).
               {"sender_verified", !sender.empty()},
               {"sender_why", sender.empty() ? why : ""},
               {"automatic", inbound::is_automatic(headers, envelope_from)}};
  if (message.truncated || message.text.size() > kTextMax || message.html.size() > kTextMax) m["truncated"] = true;
  bj::array parts;
  std::error_code ec;
  for (size_t i = 0; i < message.attachments.size(); ++i) {
    const auto& a = message.attachments[i];
    if (i == 0) fs::create_directories(dir / (id + ".parts"), ec);
    base::write_file_durable(dir / (id + ".parts") / std::to_string(i), a.bytes);
    parts.push_back(bj::object{{"index", int64_t(i)}, {"filename", a.filename}, {"content_type", a.content_type},
                               {"bytes", int64_t(a.bytes.size())}, {"inline", a.inline_part}});
  }
  m["attachments"] = std::move(parts);
  mailbox::write(dir, id, m, head_of(m));
  // Kept 400 days; never dropped for space (past the budget new mail is refused instead).
  mailbox::trim(dir, size_t(-1), kKeepMs, INT64_MAX, now);
  return id;
}

bool Host::operator_mail_in_scope(const OperatorScope& scope, const bj::object& head) const {
  if (scope.all) return true;
  // Whose it is: the address of ours it came to, or went from.
  std::string ours = text_of(head, text_of(head, "direction") == "in" ? "to" : "from");
  auto tenant = operator_address_tenant(ours);
  return tenant && !tenant->empty() && scope.tenants.count(*tenant);
}

bj::array Host::operator_mail_list(const OperatorScope& scope, const std::string& folder, int64_t limit) {
  return mailbox::list(operator_mailbox_dir(), limit, head_of, [&](const bj::object& h) {
    std::string dir = text_of(h, "direction");
    if (folder == "inbox" && dir != "in") return false;
    if (folder == "sent" && dir != "out") return false;
    return operator_mail_in_scope(scope, h);
  });
}

std::optional<bj::object> Host::operator_mail_get(const OperatorScope& scope, const std::string& id) {
  auto m = mailbox::get(operator_mailbox_dir(), id);
  if (!m || !operator_mail_in_scope(scope, *m)) return std::nullopt;
  return m;
}

std::optional<bj::object> Host::operator_mail_attachment(const OperatorScope& scope, const std::string& id, int64_t index) {
  auto m = operator_mail_get(scope, id);
  if (!m) return std::nullopt;
  const auto* parts = m->if_contains("attachments");
  if (!parts || !parts->is_array() || index < 0 || index >= int64_t(parts->as_array().size())) return std::nullopt;
  const auto& part = parts->as_array()[size_t(index)].as_object();
  auto bytes = base::read_file(operator_mailbox_dir() / (id + ".parts") / std::to_string(index));
  if (!bytes) return std::nullopt;
  return bj::object{{"filename", part.at("filename")}, {"content_type", part.at("content_type")},
                    {"base64", base::base64_encode(*bytes)}};
}

bool Host::operator_mail_delete(const OperatorScope& scope, const std::string& id) {
  if (!operator_mail_get(scope, id)) return false;
  return mailbox::remove(operator_mailbox_dir(), id);
}

std::optional<bj::object> Host::operator_mail_send(const OperatorScope& scope, const std::string& by, const bj::object& args,
                                                   std::string* error) {
  std::string from = base::to_lower(text_of(args, "from")), to = base::to_lower(text_of(args, "to"));
  std::string subject = text_of(args, "subject"), text = text_of(args, "text"), from_name = text_of(args, "from_name");
  auto tenant = operator_address_tenant(from);
  if (!tenant) return *error = from + " isn't one of the operators' addresses (admin@, postmaster@, or abuse@ on a production domain)", std::nullopt;
  if (!scope.all && (tenant->empty() || !scope.tenants.count(*tenant))) {
    return *error = "you're focused on other tenants: " + from + " isn't yours to send as", std::nullopt;
  }
  if (!inbound::split_address(to) || to.find_first_of(",; \t\r\n") != std::string::npos) {
    return *error = "to is one address, like ann@example.org", std::nullopt;
  }
  if (subject.empty() || subject.size() > kSubjectMax) return *error = "a subject, 1 to 998 bytes", std::nullopt;
  if (text.size() > kSendTextMax) return *error = "the text is at most 256 KiB", std::nullopt;
  if (base::valid_utf8(subject) != subject || base::valid_utf8(text) != text) return *error = "subject and text must be UTF-8", std::nullopt;
  identity::Mail mail;
  mail.to = to;
  mail.subject = subject;
  mail.text = text;
  mail.from_address = from;
  mail.from_name = from_name;
  // A reply threads under the message it answers (RFC 5322 § 3.6.4).
  if (std::string reply = text_of(args, "in_reply_to"); !reply.empty()) {
    auto orig = operator_mail_get(scope, reply);
    if (!orig) return *error = "in_reply_to " + reply + " isn't a message of the mailbox you may read", std::nullopt;
    mail.in_reply_to = text_of(*orig, "message_id");
    std::string refs = text_of(*orig, "references");
    mail.references = refs.empty() ? mail.in_reply_to : refs + " " + mail.in_reply_to;
  }
  MailOutcome outcome = mail_->deliver(mail);
  int64_t now = base::now_unix_ms();
  std::string id = mailbox::new_id(now);
  bj::object kept{{"id", id},
                  {"at_ms", now},
                  {"direction", "out"},
                  {"from", from},
                  {"from_name", from_name},
                  {"to", to},
                  {"subject", subject},
                  {"text", text},
                  {"html", ""},
                  {"in_reply_to", mail.in_reply_to},
                  {"references", mail.references},
                  {"message_id", outcome.message_id},
                  {"by", by},
                  {"via", outcome.via},
                  {"sent", outcome.sent}};
  if (!outcome.sent) kept["error"] = outcome.error;
  fs::path dir = operator_mailbox_dir();
  mailbox::write(dir, id, kept, head_of(kept));
  audit("operators.mail_send", by, from + " to " + to, outcome.sent ? "ok" : "failed",
        bj::object{{"id", id}, {"via", outcome.via}, {"subject", subject.substr(0, 200)}, {"error", outcome.error}});
  if (!outcome.sent) return *error = "not sent (" + outcome.via + "): " + outcome.error + "; a copy is kept in Sent", std::nullopt;
  return bj::object{{"id", id}, {"sent", true}, {"via", outcome.via}};
}

}  // namespace goober::host
