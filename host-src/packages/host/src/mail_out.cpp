// A box's mail to people who never signed up (design/machine/018.Mail.md § 3): every mail
// carries a one-click unsubscribe (RFC 8058, and a link in its footer), and a tenant's
// unsubscribed addresses are kept, by hash only, so no box of that tenant can mail them again.

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/sqlite/sqlite.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;

namespace {

// The same hash as the sent ledger's: an address is never stored.
std::string address_hash(const std::string& address) {
  return base::sha256_hex("goober-mail-address\n" + base::to_lower(address)).substr(0, 32);
}

std::unique_ptr<sqlite::Db> open_suppressed(const fs::path& storage) {
  fs::create_directories(storage / "mail");
  auto db = std::make_unique<sqlite::Db>(storage / "mail" / "unsubscribed.db");
  db->exec("CREATE TABLE IF NOT EXISTS unsubscribed (tenant TEXT NOT NULL, address TEXT NOT NULL, at_ms INTEGER NOT "
           "NULL, how TEXT NOT NULL, PRIMARY KEY (tenant, address))");
  return db;
}

bool same(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
  return d == 0;
}

bool is_hex(std::string_view s, size_t n) {
  if (s.size() != n) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

bool Host::mail_suppressed(const std::string& tenant, const std::string& address) {
  auto db = open_suppressed(config_.get()->storage);
  sqlite::Stmt s(*db, "SELECT 1 FROM unsubscribed WHERE tenant = ? AND address = ?");
  s.bind(1, tenant).bind(2, address_hash(address));
  return s.step();
}

void Host::mail_suppress(const std::string& tenant, const std::string& hash, const std::string& how, bool on) {
  auto db = open_suppressed(config_.get()->storage);
  if (on) {
    sqlite::Stmt s(*db, "INSERT OR IGNORE INTO unsubscribed (tenant, address, at_ms, how) VALUES (?, ?, ?, ?)");
    s.bind(1, tenant).bind(2, hash).bind(3, base::now_unix_ms()).bind(4, how).run();
  } else {
    sqlite::Stmt s(*db, "DELETE FROM unsubscribed WHERE tenant = ? AND address = ?");
    s.bind(1, tenant).bind(2, hash).run();
  }
}

std::string Host::unsubscribe_url(const std::string& domain, const std::string& tenant, const std::string& address) {
  auto cfg = config_.get();
  std::string hash = address_hash(address);
  std::string mac = base::hmac_sha256_hex(reply_key(), "goober-unsubscribe\n" + tenant + "\n" + hash).substr(0, 32);
  // On the site the mail is about, so the person sees its name; on this machine, the base URL.
  std::string base = cfg->local || domain.empty() || domain == "localhost" ? cfg->base_url() : "https://" + domain;
  return base + "/goober/unsubscribe/" + tenant + "/" + hash + "/" + mac;
}

void Host::handle_unsubscribe(const Request& req, const Finish& finish, const std::string& path,
                              const std::string& tenant) {
  std::string request_id = req.header("goober-request-id");
  // /goober/unsubscribe/$tenant/$hash/$mac
  std::string rest = path.substr(std::string("/goober/unsubscribe/").size());
  auto a = rest.find('/'), b = a == std::string::npos ? a : rest.find('/', a + 1);
  if (b == std::string::npos) return finish(error_page(tenant, 404, request_id), R"({"unsubscribe":"bad link"})");
  std::string t = rest.substr(0, a), hash = rest.substr(a + 1, b - a - 1), mac = rest.substr(b + 1);
  std::string want = base::hmac_sha256_hex(reply_key(), "goober-unsubscribe\n" + t + "\n" + hash).substr(0, 32);
  if (t != tenant || !is_hex(hash, 32) || !is_hex(mac, 32) || !same(mac, want)) {
    return finish(error_page(tenant, 404, request_id), R"({"unsubscribe":"bad link"})");
  }
  if (req.method == "post") {
    // The one-click POST (RFC 8058) comes from the person's mail provider, with no cookie and
    // no Origin of ours; the link's MAC is the whole authority, and it only ever unsubscribes.
    mail_suppress(tenant, hash, "link", true);
    return finish(identity_page(tenant, 200, "Unsubscribed",
                                "<h1>You're unsubscribed</h1><p>This site won't email you again. If that was a "
                                "mistake, write back to any of its mails and ask.</p>"),
                  R"({"unsubscribe":"done"})");
  }
  if (req.method != "get" && req.method != "head") {
    return finish(error_page(tenant, 405, request_id), R"({"unsubscribe":"method"})");
  }
  // A GET never unsubscribes: mail scanners open every link in a message.
  return finish(identity_page(tenant, 200, "Unsubscribe?",
                              "<h1>Stop these emails?</h1><p>You won't get any more mail from this site's "
                              "boxes. Sign-in codes you ask for still come.</p><form method=\"post\">"
                              "<button name=\"List-Unsubscribe\" value=\"One-Click\">Unsubscribe</button></form>"),
                R"({"unsubscribe":"asked"})");
}

void Host::register_mail_out_tools() {
  using mcp::boolean_property;
  using mcp::object_schema;
  using mcp::string_property;
  mcp_->add_tool(
      {"mail_unsubscribed",
       "A tenant's unsubscribed addresses: the people who asked for no more of its boxes' mail (by the link in "
       "every mail, or an operator). Addresses are kept only as hashes, so give an email to check one. With "
       "set, an operator adds an address (they asked by phone or reply) or, with set false, takes one off "
       "(only when that person asked to hear again).",
       object_schema({{"tenant", string_property("A tenant name from the host config.")},
                      {"email", string_property("An address to check or set.")},
                      {"set", boolean_property("true adds the address; false removes it.")}},
                     {"tenant"}),
       [this](const bj::object& args) {
         auto str = [&](const char* k) {
           auto* v = args.if_contains(k);
           return v && v->is_string() ? std::string(v->as_string()) : std::string();
         };
         std::string tenant = str("tenant"), email = base::to_lower(str("email"));
         if (!base::is_valid_name(tenant)) return ToolResult::fail("a tenant name is required");
         auto db = open_suppressed(config_.get()->storage);
         if (email.empty()) {
           sqlite::Stmt s(*db, "SELECT count(*), max(at_ms) FROM unsubscribed WHERE tenant = ?");
           s.bind(1, tenant);
           s.step();
           return ToolResult::ok(bj::object{
               {"tenant", tenant},
               {"unsubscribed", s.int64(0)},
               {"newest", s.is_null(1) ? bj::value(nullptr) : bj::value(base::format_utc_ms(s.int64(1)))}});
         }
         if (email.find('@') == std::string::npos) return ToolResult::fail("email is one address");
         if (auto* set = args.if_contains("set"); set && set->is_bool()) {
           mail_suppress(tenant, address_hash(email), "operator", set->as_bool());
         }
         return ToolResult::ok(bj::object{{"tenant", tenant}, {"email", email}, {"unsubscribed", mail_suppressed(tenant, email)}});
       }});
}

}  // namespace goober::host
