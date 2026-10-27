// The outbox (035.TheOutBox.md; design/machine/015.Network.md §4): effectful calls are rows
// in a host-owned goober_outbox table. A box with data keeps its outbox in the database of its
// first strategy, written in the same transaction as the box's own writes, so an element is
// enqueued if and only if the request commits. A box without data keeps it in
// storage/sqlite3/$tenant/outbox.db, written after the box succeeds. A dispatcher delivers
// pending elements in commit order per service, retries with backoff, and dead-letters what
// it can't deliver in time.

#include <iostream>

#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "keys.h"
#include "network.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kOutboxTable = R"sql(
CREATE TABLE IF NOT EXISTS goober_outbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created_ms INTEGER NOT NULL,
  box TEXT NOT NULL, service TEXT NOT NULL, method TEXT NOT NULL,
  verb TEXT NOT NULL, url TEXT NOT NULL, headers TEXT NOT NULL,
  body TEXT NOT NULL, content_type TEXT NOT NULL,
  credentials TEXT NOT NULL, auth_header TEXT NOT NULL, auth_prefix TEXT NOT NULL,
  idem_header TEXT NOT NULL, idem_key TEXT NOT NULL,
  max_attempts INTEGER NOT NULL, backoff_ms INTEGER NOT NULL, deadline_ms INTEGER NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0, next_ms INTEGER NOT NULL,
  state TEXT NOT NULL DEFAULT 'pending',   -- pending, done, or dead
  last_status INTEGER, last_error TEXT, done_ms INTEGER
))sql";

constexpr const char* kInsert =
    "INSERT INTO goober_outbox (created_ms, box, service, method, verb, url, headers, body, content_type, "
    "credentials, auth_header, auth_prefix, idem_header, idem_key, max_attempts, backoff_ms, deadline_ms, next_ms) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

bj::array insert_params(const OutboxElement& e, const std::string& box, int64_t now) {
  bj::array headers;
  for (const auto& [k, v] : e.headers) headers.push_back(bj::array{bj::value(k), bj::value(v)});
  return bj::array{now,          bj::value(box),          bj::value(e.service),   bj::value(e.method),
                   bj::value(e.verb), bj::value(e.url),     bj::value(json::dump(headers)),
                   bj::value(e.body), bj::value(e.content_type), bj::value(e.credentials),
                   bj::value(e.auth_header), bj::value(e.auth_prefix), bj::value(e.idempotency_header),
                   bj::value(e.idempotency_key), e.attempts, e.backoff_ms, now + e.within_ms, now};
}

// The strategy whose database holds a box's outbox, or nullptr for outbox.db.
const manifest::DataStrategy* outbox_strategy(const manifest::Manifest& m) {
  for (const auto& d : m.data) {
    if (d.applies_schema()) return &d;
  }
  return nullptr;
}

}  // namespace

bool Host::enqueue_outbox(const manifest::Manifest& m, const std::string& tenant, std::vector<OutboxElement>& pending,
                          db::Session* session, std::string* error) {
  int64_t now = base::now_unix_ms();
  if (const auto* st = outbox_strategy(m); st && session) {
    // In the request's own transaction: enqueued if and only if it commits.
    if (!session->host_exec(*st, kOutboxTable, nullptr, error)) return false;
    for (const auto& e : pending) {
      if (!session->host_exec(*st, kInsert, insert_params(e, m.box, now), error)) return false;
    }
    return true;
  }
  auto dir = config_.get()->storage / "sqlite3" / tenant;
  fs::create_directories(dir);
  sqlite::Db db(dir / "outbox.db");
  db.exec(kOutboxTable);
  db.exec("BEGIN IMMEDIATE");
  try {
    for (const auto& e : pending) {
      sqlite::Stmt s(db, kInsert);
      auto p = insert_params(e, m.box, now);
      for (size_t i = 0; i < p.size(); ++i) {
        if (p[i].is_int64()) s.bind(int(i) + 1, p[i].as_int64());
        else s.bind(int(i) + 1, std::string_view(p[i].as_string()));
      }
      s.run();
    }
    db.exec("COMMIT");
  } catch (const std::exception& e) {
    db.exec("ROLLBACK");
    *error = e.what();
    return false;
  }
  return true;
}

// Every outbox a box could have filled: (tenant, database or "" for outbox.db).
std::vector<std::pair<std::string, std::string>> Host::outbox_locations() {
  std::set<std::pair<std::string, std::string>> out;
  auto cfg = config_.get();
  auto active = boxes_->active();
  for (const auto& [name, box] : active->boxes) {
    bool any = false;
    for (const auto& s : box->manifest.external) any |= s.has_outbox();
    if (!any) continue;
    const auto* st = outbox_strategy(box->manifest);
    for (const auto& [domain, entry] : cfg->domains) {
      if (box->manifest.applies_to(domain)) out.emplace(entry.tenant, st ? st->database_for(entry.tenant) : "");
    }
  }
  return {out.begin(), out.end()};
}

std::unique_ptr<sqlite::Db> Host::open_outbox(const std::string& tenant, const std::string& database) {
  auto cfg = config_.get();
  std::unique_ptr<sqlite::Db> db;
  if (database.empty()) {
    auto file = cfg->storage / "sqlite3" / tenant / "outbox.db";
    if (!fs::exists(file)) return nullptr;
    db = std::make_unique<sqlite::Db>(file);
  } else {
    if (!databases_->exists(tenant, database)) return nullptr;
    std::string err;
    db = databases_->open(tenant, database, nullptr, &err);
    if (!db) return nullptr;
  }
  sqlite::Stmt has(*db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'goober_outbox'");
  if (!has.step()) return nullptr;
  return db;
}

int Host::outbox_tick(int64_t now_ms) {
  auto cfg = config_.get();
  int delivered = 0;
  for (const auto& [tenant, database] : outbox_locations()) {
    auto db = open_outbox(tenant, database);
    if (!db) continue;
    db->exec("DELETE FROM goober_outbox WHERE state = 'done' AND done_ms < " +
             std::to_string(now_ms - 7LL * 24 * 3600 * 1000));
    struct Row {
      int64_t id, attempts, max_attempts, backoff_ms, deadline_ms;
      std::string box, service, method, verb, url, headers, body, content_type, credentials, auth_header, auth_prefix,
          idem_header, idem_key;
    };
    std::vector<Row> rows;
    {
      sqlite::Stmt s(*db,
                     "SELECT id, attempts, max_attempts, backoff_ms, deadline_ms, box, service, method, verb, url, headers, "
                     "body, content_type, credentials, auth_header, auth_prefix, idem_header, idem_key FROM goober_outbox "
                     "WHERE state = 'pending' ORDER BY id LIMIT 50");
      while (s.step()) {
        rows.push_back(Row{s.int64(0), s.int64(1), s.int64(2), s.int64(3), s.int64(4), s.text(5), s.text(6), s.text(7),
                           s.text(8), s.text(9), s.text(10), s.text(11), s.text(12), s.text(13), s.text(14), s.text(15),
                           s.text(16), s.text(17)});
      }
    }
    // Commit order per service: once one element waits, the ones behind it wait too.
    std::set<std::string> waiting;
    for (const auto& r : rows) {
      if (waiting.count(r.service)) continue;
      sqlite::Stmt next(*db, "SELECT next_ms FROM goober_outbox WHERE id = ?");
      next.bind(1, r.id);
      if (next.step() && next.int64(0) > now_ms) {
        waiting.insert(r.service);
        continue;
      }
      std::vector<std::pair<std::string, std::string>> headers;
      std::string err;
      if (auto h = json::parse(r.headers, &err); h && h->is_array()) {
        for (const auto& p : h->as_array()) headers.emplace_back(p.at(0).as_string(), p.at(1).as_string());
      }
      if (!r.idem_header.empty()) headers.emplace_back(r.idem_header, r.idem_key);
      HttpOut reply;
      if (!r.credentials.empty()) {
        auto value = keys_->resolve(r.credentials, tenant);
        if (!value) reply.error = "the key " + r.credentials + " isn't set";
        else headers.emplace_back(r.auth_header, r.auth_prefix + *value);
      }
      int64_t started = base::monotonic_us();
      if (reply.error.empty()) {
        reply = send_http(r.verb, r.url, headers, r.body, r.content_type, cfg->local, cfg->storage / "tmp", 10000);
      }
      egress_log(tenant, r.box, r.service, r.method, r.verb, r.url, reply.status, reply.error,
                 base::monotonic_us() - started, r.body.size(), reply.body.size(), "", r.id);
      int64_t attempts = r.attempts + 1;
      bool ok = reply.error.empty() && reply.status >= 200 && reply.status < 300;
      // A 4xx other than 408 or 429 won't get better by asking again.
      bool permanent = reply.error.empty() && reply.status >= 400 && reply.status < 500 && reply.status != 408 &&
                       reply.status != 429;
      std::string state = ok ? "done" : (permanent || attempts >= r.max_attempts || now_ms >= r.deadline_ms) ? "dead" : "pending";
      int64_t backoff = r.backoff_ms << std::min<int64_t>(attempts - 1, 20);
      int64_t next_ms = now_ms + std::min<int64_t>(backoff, 3'600'000);
      sqlite::Stmt u(*db,
                     "UPDATE goober_outbox SET attempts = ?, state = ?, next_ms = ?, last_status = ?, last_error = ?, "
                     "done_ms = ? WHERE id = ?");
      u.bind(1, attempts).bind(2, state).bind(3, next_ms).bind(4, int64_t(reply.status));
      reply.error.empty() ? u.bind_null(5) : u.bind(5, reply.error);
      state == "pending" ? u.bind_null(6) : u.bind(6, now_ms);
      u.bind(7, r.id).run();
      if (ok) ++delivered;
      if (state == "pending") waiting.insert(r.service);
      if (state == "dead") {
        std::cerr << "goober-host: outbox element " << r.id << " (" << r.service << "." << r.method << ") for " << tenant
                  << " is dead: " << (reply.error.empty() ? "status " + std::to_string(reply.status) : reply.error) << "\n";
      }
    }
  }
  return delivered;
}

bj::array Host::outbox_list(const std::string& state) {
  bj::array out;
  for (const auto& [tenant, database] : outbox_locations()) {
    auto db = open_outbox(tenant, database);
    if (!db) continue;
    sqlite::Stmt s(*db,
                   "SELECT id, created_ms, box, service, method, verb, url, attempts, state, last_status, last_error "
                   "FROM goober_outbox WHERE state = ? ORDER BY id DESC LIMIT 100");
    s.bind(1, state);
    while (s.step()) {
      std::string url = s.text(6);
      out.push_back(bj::object{{"tenant", tenant},
                               {"database", database.empty() ? "outbox" : database},
                               {"id", s.int64(0)},
                               {"created_at", base::format_utc_ms(s.int64(1))},
                               {"box", s.text(2)},
                               {"call", s.text(3) + "." + s.text(4)},
                               {"request", s.text(5) + " " + url.substr(0, url.find('?'))},
                               {"attempts", s.int64(7)},
                               {"state", s.text(8)},
                               {"last_status", s.is_null(9) ? bj::value(nullptr) : bj::value(s.int64(9))},
                               {"last_error", s.is_null(10) ? bj::value(nullptr) : bj::value(s.text(10))}});
    }
  }
  return out;
}

bool Host::outbox_set(const std::string& tenant, const std::string& database, int64_t id, bool retry) {
  auto db = open_outbox(tenant, database == "outbox" ? "" : database);
  if (!db) return false;
  sqlite::Stmt s(*db, retry ? "UPDATE goober_outbox SET state = 'pending', attempts = 0, next_ms = 0, deadline_ms = "
                              "? WHERE id = ? AND state = 'dead'"
                            : "DELETE FROM goober_outbox WHERE id = ? AND state = 'dead'");
  if (retry) {
    s.bind(1, base::now_unix_ms() + 600000).bind(2, id);
  } else {
    s.bind(1, id);
  }
  s.run();
  return db->changes() > 0;
}

void Host::run_outbox() {
  std::unique_lock lock(timer_thread_mu_);
  while (!timers_stopping_) {
    lock.unlock();
    try {
      outbox_tick(base::now_unix_ms());
    } catch (const std::exception& e) {
      std::cerr << "goober-host: outbox: " << e.what() << "\n";
    }
    lock.lock();
    timer_cv_.wait_for(lock, std::chrono::seconds(2), [this] { return timers_stopping_; });
  }
}

}  // namespace goober::host
