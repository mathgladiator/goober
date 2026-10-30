#include "goober/mail_consent/consent.h"

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/sqlite/sqlite.h"

namespace goober::mail::consent {

namespace fs = std::filesystem;

namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS blocks (
  tenant TEXT NOT NULL,
  address TEXT NOT NULL,                -- address_hash
  at_ms INTEGER NOT NULL,
  how TEXT NOT NULL,                    -- invite_declined, unsubscribed, complaint, operator
  evidence TEXT NOT NULL,
  lifted_at INTEGER,
  lifted_by TEXT,
  statement TEXT,
  id INTEGER PRIMARY KEY AUTOINCREMENT
);
CREATE INDEX IF NOT EXISTS blocks_live ON blocks (tenant, address, lifted_at);
CREATE TABLE IF NOT EXISTS consent (
  tenant TEXT NOT NULL,
  address TEXT NOT NULL,
  list TEXT NOT NULL,
  state TEXT NOT NULL,                  -- in, out
  how TEXT NOT NULL,
  at_ms INTEGER NOT NULL,
  evidence TEXT NOT NULL,
  PRIMARY KEY (tenant, address, list)
);
CREATE INDEX IF NOT EXISTS consent_by_list ON consent (tenant, list, state);
CREATE TABLE IF NOT EXISTS consent_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  tenant TEXT NOT NULL,
  address TEXT NOT NULL,
  at_ms INTEGER NOT NULL,
  what TEXT NOT NULL,                   -- block, lift, in, out, outreach_out
  list TEXT NOT NULL,
  how TEXT NOT NULL,
  evidence TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS consent_log_by_address ON consent_log (tenant, address, id);
CREATE TABLE IF NOT EXISTS outreach (
  tenant TEXT NOT NULL,
  address TEXT NOT NULL,
  first_ms INTEGER NOT NULL,
  last_ms INTEGER NOT NULL,
  count INTEGER NOT NULL,
  opted_out INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (tenant, address)
);
CREATE TABLE IF NOT EXISTS outreach_sent (tenant TEXT NOT NULL, at_ms INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS outreach_sent_by_tenant ON outreach_sent (tenant, at_ms);
CREATE TABLE IF NOT EXISTS asked (tenant TEXT NOT NULL, address TEXT NOT NULL, kind TEXT NOT NULL, at_ms INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS asked_by_address ON asked (tenant, address, kind, at_ms);
)sql";

class Tx {
 public:
  explicit Tx(sqlite::Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
  ~Tx() {
    if (!done_) {
      try {
        db_.exec("ROLLBACK");
      } catch (...) {
      }
    }
  }
  void commit() {
    db_.exec("COMMIT");
    done_ = true;
  }

 private:
  sqlite::Db& db_;
  bool done_ = false;
};

bj::object parse_object(const std::string& text) {
  std::string err;
  auto v = json::parse(text, &err);
  return v && v->is_object() ? v->as_object() : bj::object{};
}

}  // namespace

std::string canonical_address(std::string_view address) {
  std::string a = base::to_lower(address);
  auto at = a.rfind('@');
  if (at == std::string::npos) return a;
  std::string local = a.substr(0, at), domain = a.substr(at + 1);
  while (domain.ends_with(".")) domain.pop_back();
  if (auto plus = local.find('+'); plus != std::string::npos && plus > 0) local.resize(plus);
  if (domain == "googlemail.com") domain = "gmail.com";
  if (domain == "gmail.com") std::erase(local, '.');
  return local + "@" + domain;
}

std::string address_hash(std::string_view address) {
  return base::sha256_hex("goober-mail-address\n" + canonical_address(address)).substr(0, 32);
}

std::string legacy_address_hash(std::string_view address) {
  return base::sha256_hex("goober-mail-address\n" + base::to_lower(address)).substr(0, 32);
}

Store::Store(fs::path db_file) {
  fs::create_directories(db_file.parent_path());
  db_ = std::make_unique<sqlite::Db>(db_file);
  db_->exec(kSchema);
}

Store::Store() = default;

std::unique_ptr<Store> Store::unavailable() { return std::unique_ptr<Store>(new Store()); }

sqlite::Db& Store::db() {
  if (!db_) throw std::runtime_error("the consent store is unreadable: nothing about who may be mailed can be read or kept");
  return *db_;
}

Store::~Store() = default;

void Store::log(const std::string& tenant, const std::string& hash, const std::string& what, const std::string& list,
                const std::string& how, const bj::object& evidence, int64_t now_ms) {
  sqlite::Stmt s(db(), "INSERT INTO consent_log (tenant, address, at_ms, what, list, how, evidence) VALUES (?, ?, ?, ?, ?, ?, ?)");
  s.bind(1, tenant).bind(2, hash).bind(3, now_ms).bind(4, what).bind(5, list).bind(6, how).bind(7, json::dump(evidence)).run();
}

std::optional<Block> Store::block_of(const std::string& tenant, std::string_view address) {
  if (auto b = block(tenant, address_hash(address))) return b;
  std::string legacy = legacy_address_hash(address);
  return legacy == address_hash(address) ? std::nullopt : block(tenant, legacy);
}

std::optional<Block> Store::block(const std::string& tenant, const std::string& hash) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(),
                 "SELECT at_ms, how, evidence FROM blocks WHERE tenant = ? AND address = ? AND lifted_at IS NULL "
                 "ORDER BY id DESC LIMIT 1");
  s.bind(1, tenant).bind(2, hash);
  if (!s.step()) return std::nullopt;
  return Block{s.int64(0), s.text(1), parse_object(s.text(2))};
}

bool Store::add_block(const std::string& tenant, const std::string& hash, const std::string& how,
                      const bj::object& evidence, int64_t now_ms) {
  std::lock_guard lock(mu_);
  Tx tx(db());
  sqlite::Stmt live(db(), "SELECT 1 FROM blocks WHERE tenant = ? AND address = ? AND lifted_at IS NULL");
  live.bind(1, tenant).bind(2, hash);
  if (live.step()) return false;
  sqlite::Stmt s(db(), "INSERT INTO blocks (tenant, address, at_ms, how, evidence) VALUES (?, ?, ?, ?, ?)");
  s.bind(1, tenant).bind(2, hash).bind(3, now_ms).bind(4, how).bind(5, json::dump(evidence)).run();
  log(tenant, hash, "block", "", how, evidence, now_ms);
  tx.commit();
  return true;
}

std::optional<Block> Store::lift(const std::string& tenant, const std::string& hash, const std::string& by,
                                 const std::string& statement, const std::string& request_id, int64_t now_ms,
                                 std::string* why) {
  if (by != "person" && !by.starts_with("operator:")) return *why = "a block is lifted by the person or an operator", std::nullopt;
  size_t least = by == "person" ? 1 : kMinStatement;
  if (statement.size() < least || statement.size() > 1000) {
    return *why = "say why, in at least " + std::to_string(least) + " characters (and at most 1000)", std::nullopt;
  }
  std::lock_guard lock(mu_);
  Tx tx(db());
  sqlite::Stmt find(db(),
                    "SELECT id, at_ms, how, evidence FROM blocks WHERE tenant = ? AND address = ? AND lifted_at IS NULL "
                    "ORDER BY id DESC LIMIT 1");
  find.bind(1, tenant).bind(2, hash);
  if (!find.step()) return *why = "that address isn't blocked", std::nullopt;
  Block b{find.int64(1), find.text(2), parse_object(find.text(3))};
  int64_t id = find.int64(0);
  sqlite::Stmt up(db(), "UPDATE blocks SET lifted_at = ?, lifted_by = ?, statement = ? WHERE id = ?");
  up.bind(1, now_ms).bind(2, by).bind(3, statement).bind(4, id).run();
  log(tenant, hash, "lift", "", by, bj::object{{"statement", statement}, {"request_id", request_id}, {"block_at", b.at_ms}},
      now_ms);
  tx.commit();
  return b;
}

std::optional<ListState> Store::list_state(const std::string& tenant, const std::string& hash, const std::string& list) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(), "SELECT state, how, at_ms FROM consent WHERE tenant = ? AND address = ? AND list = ?");
  s.bind(1, tenant).bind(2, hash).bind(3, list);
  if (!s.step()) return std::nullopt;
  return ListState{list, s.text(0), s.text(1), s.int64(2)};
}

std::vector<ListState> Store::lists(const std::string& tenant, const std::string& hash) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(), "SELECT list, state, how, at_ms FROM consent WHERE tenant = ? AND address = ? ORDER BY list");
  s.bind(1, tenant).bind(2, hash);
  std::vector<ListState> out;
  while (s.step()) out.push_back(ListState{s.text(0), s.text(1), s.text(2), s.int64(3)});
  return out;
}

bool Store::opt_in(const std::string& tenant, const std::string& hash, const std::string& list, const std::string& how,
                   const bj::object& evidence, int64_t now_ms, std::string* why) {
  if (how != "invite" && how != "account" && how != "confirmed" && how != "operator") {
    return *why = "an opt-in is by invite, account, confirmed, or operator", false;
  }
  std::lock_guard lock(mu_);
  Tx tx(db());
  sqlite::Stmt blocked(db(), "SELECT 1 FROM blocks WHERE tenant = ? AND address = ? AND lifted_at IS NULL");
  blocked.bind(1, tenant).bind(2, hash);
  if (blocked.step()) return *why = "the address is blocked: it asked for no mail from this site", false;
  sqlite::Stmt cur(db(), "SELECT state FROM consent WHERE tenant = ? AND address = ? AND list = ?");
  cur.bind(1, tenant).bind(2, hash).bind(3, list);
  bool was_out = cur.step() && cur.text(0) == "out";
  if (was_out && how == "operator") {
    return *why = "the person left this list; only they can join it again", false;
  }
  sqlite::Stmt s(db(),
                 "INSERT INTO consent (tenant, address, list, state, how, at_ms, evidence) VALUES (?, ?, ?, 'in', ?, ?, ?) "
                 "ON CONFLICT (tenant, address, list) DO UPDATE SET state = 'in', how = excluded.how, "
                 "at_ms = excluded.at_ms, evidence = excluded.evidence");
  s.bind(1, tenant).bind(2, hash).bind(3, list).bind(4, how).bind(5, now_ms).bind(6, json::dump(evidence)).run();
  log(tenant, hash, "in", list, how, evidence, now_ms);
  tx.commit();
  return true;
}

void Store::opt_out(const std::string& tenant, const std::string& hash, const std::string& list, const std::string& how,
                    const bj::object& evidence, int64_t now_ms) {
  std::lock_guard lock(mu_);
  Tx tx(db());
  sqlite::Stmt s(db(),
                 "INSERT INTO consent (tenant, address, list, state, how, at_ms, evidence) VALUES (?, ?, ?, 'out', ?, ?, ?) "
                 "ON CONFLICT (tenant, address, list) DO UPDATE SET state = 'out', how = excluded.how, "
                 "at_ms = excluded.at_ms, evidence = excluded.evidence");
  s.bind(1, tenant).bind(2, hash).bind(3, list).bind(4, how).bind(5, now_ms).bind(6, json::dump(evidence)).run();
  log(tenant, hash, "out", list, how, evidence, now_ms);
  tx.commit();
}

int64_t Store::count_in(const std::string& tenant, const std::string& list) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(), "SELECT count(*) FROM consent WHERE tenant = ? AND list = ? AND state = 'in'");
  s.bind(1, tenant).bind(2, list);
  return s.step() ? s.int64(0) : 0;
}

std::vector<std::string> Store::members(const std::string& tenant, const std::string& list) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(), "SELECT address FROM consent WHERE tenant = ? AND list = ? AND state = 'in'");
  s.bind(1, tenant).bind(2, list);
  std::vector<std::string> out;
  while (s.step()) out.push_back(s.text(0));
  return out;
}

std::optional<Store::Outreach> Store::outreach(const std::string& tenant, const std::string& hash) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(), "SELECT first_ms, last_ms, count, opted_out FROM outreach WHERE tenant = ? AND address = ?");
  s.bind(1, tenant).bind(2, hash);
  if (!s.step()) return std::nullopt;
  return Outreach{s.int64(0), s.int64(1), s.int64(2), s.int64(3) != 0};
}

void Store::outreach_opt_out(const std::string& tenant, const std::string& hash, const std::string& how, int64_t now_ms) {
  std::lock_guard lock(mu_);
  Tx tx(db());
  sqlite::Stmt s(db(),
                 "INSERT INTO outreach (tenant, address, first_ms, last_ms, count, opted_out) VALUES (?, ?, ?, ?, 0, 1) "
                 "ON CONFLICT (tenant, address) DO UPDATE SET opted_out = 1");
  s.bind(1, tenant).bind(2, hash).bind(3, now_ms).bind(4, now_ms).run();
  log(tenant, hash, "outreach_out", "", how, bj::object{}, now_ms);
  tx.commit();
}

void Store::outreach_sent(const std::string& tenant, const std::string& hash, int64_t now_ms) {
  std::lock_guard lock(mu_);
  Tx tx(db());
  sqlite::Stmt s(db(),
                 "INSERT INTO outreach (tenant, address, first_ms, last_ms, count) VALUES (?, ?, ?, ?, 1) "
                 "ON CONFLICT (tenant, address) DO UPDATE SET last_ms = excluded.last_ms, count = count + 1");
  s.bind(1, tenant).bind(2, hash).bind(3, now_ms).bind(4, now_ms).run();
  sqlite::Stmt t(db(), "INSERT INTO outreach_sent (tenant, at_ms) VALUES (?, ?)");
  t.bind(1, tenant).bind(2, now_ms).run();
  tx.commit();
}

int64_t Store::outreach_since(const std::string& tenant, int64_t since_ms) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(), "SELECT count(*) FROM outreach_sent WHERE tenant = ? AND at_ms > ?");
  s.bind(1, tenant).bind(2, since_ms);
  return s.step() ? s.int64(0) : 0;
}

int64_t Store::asked_since(const std::string& tenant, const std::string& hash, const std::string& kind, int64_t since_ms) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(), "SELECT count(*) FROM asked WHERE tenant = ? AND address = ? AND kind = ? AND at_ms > ?");
  s.bind(1, tenant).bind(2, hash).bind(3, kind).bind(4, since_ms);
  return s.step() ? s.int64(0) : 0;
}

void Store::asked(const std::string& tenant, const std::string& hash, const std::string& kind, int64_t now_ms) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(), "INSERT INTO asked (tenant, address, kind, at_ms) VALUES (?, ?, ?, ?)");
  s.bind(1, tenant).bind(2, hash).bind(3, kind).bind(4, now_ms).run();
}

bj::array Store::history(const std::string& tenant, const std::string& hash) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(db(),
                 "SELECT at_ms, what, list, how, evidence FROM consent_log WHERE tenant = ? AND address = ? ORDER BY id");
  s.bind(1, tenant).bind(2, hash);
  bj::array out;
  while (s.step()) {
    bj::object row{{"at_ms", s.int64(0)}, {"what", s.text(1)}, {"how", s.text(3)}, {"evidence", parse_object(s.text(4))}};
    if (!s.text(2).empty()) row["list"] = s.text(2);
    out.push_back(std::move(row));
  }
  return out;
}

void Store::prune(int64_t now_ms) {
  std::lock_guard lock(mu_);
  sqlite::Stmt a(db(), "DELETE FROM consent_log WHERE at_ms < ?");
  a.bind(1, now_ms - kLogKeepMs).run();
  sqlite::Stmt b(db(), "DELETE FROM asked WHERE at_ms < ?");
  b.bind(1, now_ms - 31LL * 24 * 3600 * 1000).run();
  sqlite::Stmt c(db(), "DELETE FROM outreach_sent WHERE at_ms < ?");
  c.bind(1, now_ms - 2LL * 24 * 3600 * 1000).run();
}

int Store::import_unsubscribed(const fs::path& file, int64_t now_ms) {
  std::error_code ec;
  if (!fs::exists(file, ec)) return 0;
  int n = 0;
  {
    sqlite::Db old(file);
    old.exec("CREATE TABLE IF NOT EXISTS unsubscribed (tenant TEXT NOT NULL, address TEXT NOT NULL, at_ms INTEGER NOT "
             "NULL, how TEXT NOT NULL, PRIMARY KEY (tenant, address))");
    sqlite::Stmt s(old, "SELECT tenant, address, at_ms, how FROM unsubscribed");
    std::vector<std::tuple<std::string, std::string, int64_t, std::string>> rows;
    while (s.step()) rows.emplace_back(s.text(0), s.text(1), s.int64(2), s.text(3));
    for (const auto& [tenant, hash, at, how] : rows) {
      n += add_block(tenant, hash, "unsubscribed", bj::object{{"via", how}, {"imported_ms", now_ms}}, at) ? 1 : 0;
    }
  }
  fs::rename(file, fs::path(file.string() + ".imported"), ec);
  return n;
}

}  // namespace goober::mail::consent
