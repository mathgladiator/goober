// Timers (045.Time.md; design/machine/014.Time.md): the host fires timer routes on their
// schedules, once per tenant the box serves. A host-level timer runs every every-ms or on a
// cron in the host's zone; a data-driven timer reads its by_timer source and fires once per
// active row, on a cron in the row's own zone or once at the row's at-field, after which the
// host turns the row's active-field off in the same transaction as the fire's writes.
//
// The plan is rebuilt from the active boxes, the config's domains, and the source rows; only
// progress is kept: the last due time of each cron fire, in storage/sqlite3/$tenant/timers.db.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/timers/cron.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;

namespace {

constexpr int64_t kRowsRefreshMs = 10'000;  // how stale a source's rows may be
constexpr int64_t kRetryMs = 60'000;        // a failed one-shot fire waits this long
constexpr int64_t kMaxRows = 10'000;        // rows per source per tenant (045: schedules are bounded)

std::string sql_name(const std::string& name) { return "\"" + name + "\""; }

}  // namespace


// Call with timers_->mu held.
sqlite::Db& Host::timer_progress(const std::string& tenant) {
  auto& db = timers_->progress[tenant];
  if (!db) {
    auto dir = config_.get()->storage / "sqlite3" / tenant;
    std::filesystem::create_directories(dir);
    db = std::make_unique<sqlite::Db>(dir / "timers.db");
    db->exec("CREATE TABLE IF NOT EXISTS goober_timer_fires (timer TEXT NOT NULL, row_key TEXT NOT NULL, "
             "last_due INTEGER NOT NULL, PRIMARY KEY (timer, row_key))");
  }
  return *db;
}

void Host::set_timers_inline(bool inline_fires) {
  std::lock_guard lock(timers_->mu);
  timers_->inline_fires = inline_fires;
}

// The tenants a box's timers run for, each with the domain its fires say they came in on.
static std::vector<std::pair<std::string, std::string>> tenants_of(const config::HostConfig& cfg,
                                                                   const manifest::Manifest& m) {
  std::map<std::string, std::string> out;
  for (const auto& [domain, entry] : cfg.domains) {
    if (m.applies_to(domain) && !out.count(entry.tenant)) out[entry.tenant] = domain;
  }
  return {out.begin(), out.end()};
}

void Host::timer_tick(int64_t now_ms) {
  auto cfg = config_.get();
  auto active = boxes_->active();
  auto& st = *timers_;

  // The last due time a cron fire completed, for catching up after downtime.
  auto progress = [&](const std::string& tenant) -> sqlite::Db& { return timer_progress(tenant); };
  auto last_due = [&](const std::string& tenant, const std::string& timer, const std::string& row) -> std::optional<int64_t> {
    sqlite::Stmt s(progress(tenant), "SELECT last_due FROM goober_timer_fires WHERE timer = ? AND row_key = ?");
    s.bind(1, timer).bind(2, row);
    if (!s.step()) return std::nullopt;
    return s.int64(0);
  };

  struct Due {
    boxes::RouteRef ref;
    std::string tenant, domain, key, row_key;
    int64_t due = 0;
    bj::value row;  // the row, for a data-driven fire; null otherwise
    const manifest::DataStrategy* source = nullptr;
  };
  std::vector<Due> due;
  {
    std::lock_guard lock(st.mu);
    for (const auto& [box_name, box] : active->boxes) {
      for (const auto& t : box->manifest.timers) {
        boxes::RouteRef ref{box, &t};
        std::optional<timers::Cron> cron;
        if (!t.cron.empty()) {
          std::string err;
          cron = timers::Cron::parse(t.cron, &err);
        }
        const manifest::DataStrategy* source = nullptr;
        for (const auto& d : box->manifest.data) {
          if (d.name == t.source) source = &d;
        }
        for (const auto& [tenant, domain] : tenants_of(*cfg, box->manifest)) {
          // A cron fire: due at `next`, computed once and kept until it fires.
          auto plan_cron = [&](const std::string& key, const std::string& row_key, const std::chrono::time_zone* zone,
                               bj::value row) {
            auto it = st.next_due.find(key);
            if (it == st.next_due.end()) {
              int64_t next = cron->next(now_ms, zone).value_or(INT64_MAX);
              // Catch up once on what was missed while the host was down (045 "missed").
              if (t.missed == "once") {
                auto last = last_due(tenant, t.name, row_key);
                auto prev = cron->previous(now_ms, zone);
                if (last && prev && *prev > *last) next = *prev;
              }
              it = st.next_due.emplace(key, next).first;
            }
            if (it->second <= now_ms && !st.running.count(key)) {
              due.push_back(Due{ref, tenant, domain, key, row_key, it->second, std::move(row), source});
              int64_t after = std::max(it->second, now_ms);
              it->second = cron->next(after, zone).value_or(INT64_MAX);
            }
          };
          if (t.source.empty()) {
            std::string key = tenant + "|" + t.name + "|";
            if (t.every_ms) {
              auto it = st.next_due.find(key);
              if (it == st.next_due.end()) it = st.next_due.emplace(key, now_ms + *t.every_ms).first;
              if (it->second <= now_ms && !st.running.count(key)) {
                due.push_back(Due{ref, tenant, domain, key, "", it->second, nullptr, nullptr});
                it->second = now_ms + *t.every_ms;
              }
            } else if (cron) {
              plan_cron(key, "", timers::host_zone(), nullptr);
            }
            continue;
          }
          if (!source) continue;
          // The source's rows, read again every few seconds.
          std::string rows_key = tenant + "|" + source->name;
          auto& cache = st.rows[rows_key];
          if (cache.read_at == 0 || now_ms - cache.read_at >= kRowsRefreshMs) {
            cache.read_at = now_ms;
            cache.rows.clear();
            std::string database = source->database_for(tenant);
            if (databases_->exists(tenant, database)) {
              std::string sql = "SELECT * FROM " + sql_name(source->table) +
                                (source->active_field.empty() ? "" : " WHERE " + sql_name(source->active_field) + " = 1") +
                                " LIMIT " + std::to_string(kMaxRows + 1);
              std::string err;
              auto r = databases_->operate(tenant, database, sql, nullptr, kMaxRows + 1, false, &err);
              if (!r) {
                std::cerr << "goober-host: timer source " << source->name << " for " << tenant << ": " << err << "\n";
              } else {
                const auto& cols = r->at("columns").as_array();
                for (const auto& row : r->at("rows").as_array()) {
                  if (cache.rows.size() >= size_t(kMaxRows)) break;
                  bj::object o;
                  for (size_t i = 0; i < cols.size(); ++i) o[cols[i].as_string()] = row.as_array()[i];
                  cache.rows.push_back(std::move(o));
                }
              }
            }
          }
          auto schema = schemas_->get(source->schema);
          const db::Table* table = schema ? schema->table(source->table) : nullptr;
          std::string pk = "id";
          if (table) {
            for (const auto& c : table->columns) {
              if (c.primary_key) pk = c.name;
            }
          }
          for (const auto& row : cache.rows) {
            const bj::value* key_value = row.if_contains(pk);
            if (!key_value) continue;
            std::string row_key = key_value->is_string() ? std::string(key_value->as_string()) : json::dump(*key_value);
            std::string key = tenant + "|" + t.name + "|" + row_key;
            if (!t.at_field.empty()) {
              const bj::value* at = row.if_contains(t.at_field);
              std::optional<int64_t> when;
              if (at && at->is_int64()) when = at->as_int64();
              if (at && at->is_string()) when = timers::parse_instant_ms(at->as_string());
              if (!when || *when > now_ms || st.running.count(key)) continue;
              if (auto r = st.retry_after.find(key); r != st.retry_after.end() && r->second > now_ms) continue;
              due.push_back(Due{ref, tenant, domain, key, row_key, *when, row, source});
              continue;
            }
            const std::chrono::time_zone* zone = timers::host_zone();
            if (!source->timezone_field.empty()) {
              const bj::value* tz = row.if_contains(source->timezone_field);
              zone = tz && tz->is_string() ? timers::find_zone(tz->as_string()) : nullptr;
              if (!zone) continue;  // a row without a real zone never fires
            }
            plan_cron(key, row_key, zone, row);
          }
        }
      }
    }
    for (const auto& d : due) st.running.insert(d.key);
  }

  for (auto& d : due) {
    auto job = [this, d, cfg]() mutable {
      uint8_t rnd[8];
      base::os_random_fill(rnd);
      std::string request_id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
      const auto& t = *d.ref.route;
      // A fire is given the row, or nothing (045 § What a fire is).
      bj::object doc{{"request_id", request_id}, {"method", "timer"},     {"path", "timer:" + t.name},
                     {"params", bj::object{}},   {"query", bj::object{}}, {"headers", bj::object{}},
                     {"body", d.row.is_null() ? std::string() : json::dump(d.row)},
                     {"domain", d.domain},       {"tenant", d.tenant},    {"user", nullptr}};
      CallOptions options;
      options.timeout_ms = t.timeout_ms;
      options.no_response_ok = true;
      if (!t.at_field.empty() && d.source) {
        // Fire, then disable, in the same transaction; a row moved meanwhile stays on.
        const auto* source = d.source;
        const bj::value at = d.row.as_object().at(t.at_field);
        std::string pk_value = d.row_key;
        options.before_commit = [source, at, pk_value, &t, this](db::Session& session, std::string* error) {
          auto schema = schemas_->get(source->schema);
          std::string pk = "id";
          if (auto* table = schema ? schema->table(source->table) : nullptr) {
            for (const auto& c : table->columns) {
              if (c.primary_key) pk = c.name;
            }
          }
          std::string sql = "UPDATE " + sql_name(source->table) + " SET " + sql_name(source->active_field) + " = 0 WHERE " +
                            sql_name(pk) + " = ? AND " + sql_name(t.at_field) + " IS ?";
          bj::value key = bj::string(pk_value);
          try {
            key = bj::value(std::stoll(pk_value));
          } catch (...) {
          }
          return session.host_exec(*source, sql, bj::array{key, at}, error);
        };
      }
      auto call = call_box(d.ref, doc, d.tenant, request_id, std::nullopt, options);
      bool ok = (call.result.outcome == runtime::wasm::CallResult::Outcome::kOk && call.result.status < 500) ||
                call.result.outcome == runtime::wasm::CallResult::Outcome::kNoResponse;
      call.extra["timer"] = t.name;
      call.extra["tenant"] = d.tenant;
      call.extra["due"] = base::format_utc_ms(d.due);
      if (!d.row_key.empty()) call.extra["row"] = d.row_key;
      server::http::Request pseudo;
      pseudo.method = "timer";
      pseudo.path = "timer:" + t.name;
      pseudo.headers.emplace_back("goober-request-id", request_id);
      access_log(pseudo, ok ? (call.result.status ? call.result.status : 204) : 500, base::monotonic_us(), d.domain, json::dump(call.extra));
      std::lock_guard lock(timers_->mu);
      timers_->running.erase(d.key);
      if (!t.cron.empty()) {
        auto& db = timer_progress(d.tenant);
        sqlite::Stmt s(db, "INSERT INTO goober_timer_fires (timer, row_key, last_due) VALUES (?, ?, ?) "
                           "ON CONFLICT (timer, row_key) DO UPDATE SET last_due = excluded.last_due");
        s.bind(1, t.name).bind(2, d.row_key).bind(3, d.due).run();
      }
      if (!t.at_field.empty()) {
        if (ok) {
          timers_->retry_after.erase(d.key);
          timers_->rows.erase(d.tenant + "|" + d.source->name);  // read again: the row is off now
        } else {
          timers_->retry_after[d.key] = base::now_unix_ms() + kRetryMs;
        }
      }
    };
    bool run_inline;
    {
      std::lock_guard lock(st.mu);
      run_inline = st.inline_fires;
    }
    if (run_inline) {
      job();
    } else if (!pool_->post(job, max_queued_)) {
      std::lock_guard lock(st.mu);
      st.running.erase(d.key);  // the queue is full; try again next tick
    }
  }
}

bj::array Host::timers_due() {
  std::lock_guard lock(timers_->mu);
  bj::array out;
  for (const auto& [key, due] : timers_->next_due) {
    auto a = key.find('|'), b = key.find('|', a + 1);
    bj::object o{{"tenant", key.substr(0, a)}, {"timer", key.substr(a + 1, b - a - 1)}};
    if (b + 1 < key.size()) o["row"] = key.substr(b + 1);
    o["next_due"] = due == INT64_MAX ? bj::value(nullptr) : bj::value(base::format_utc_ms(due));
    o["running"] = timers_->running.count(key) > 0;
    out.push_back(std::move(o));
  }
  return out;
}

std::optional<int> Host::run_timer_now(const std::string& name, const std::string& tenant, std::string* error) {
  auto active = boxes_->active();
  for (const auto& [box_name, box] : active->boxes) {
    for (const auto& t : box->manifest.timers) {
      if (t.name != name) continue;
      bool serves = false;
      for (const auto& [tn, domain] : tenants_of(*config_.get(), box->manifest)) serves |= tn == tenant;
      if (!serves) return *error = "box " + box_name + " doesn't serve tenant " + tenant, std::nullopt;
      // Fire as the schedule would, but now: the next tick's plan for this timer is
      // forgotten so it can fire the rows again.
      {
        std::lock_guard lock(timers_->mu);
        for (auto it = timers_->next_due.begin(); it != timers_->next_due.end();) {
          bool mine = it->first.starts_with(tenant + "|" + name + "|");
          it = mine ? timers_->next_due.erase(it) : std::next(it);
        }
        if (!t.source.empty()) timers_->rows.erase(tenant + "|" + t.source);
      }
      int fired = 0;
      std::vector<bj::value> rows{nullptr};
      if (!t.source.empty()) {
        rows.clear();
        const manifest::DataStrategy* source = nullptr;
        for (const auto& d : box->manifest.data) {
          if (d.name == t.source) source = d.pattern == "by_timer" ? &d : nullptr;
        }
        if (!source) return *error = "timer " + name + " has no by_timer source", std::nullopt;
        std::string database = source->database_for(tenant);
        if (!databases_->exists(tenant, database)) return 0;
        std::string sql = "SELECT * FROM " + sql_name(source->table) +
                          (source->active_field.empty() ? "" : " WHERE " + sql_name(source->active_field) + " = 1") +
                          " LIMIT " + std::to_string(kMaxRows);
        auto r = databases_->operate(tenant, database, sql, nullptr, kMaxRows, false, error);
        if (!r) return std::nullopt;
        const auto& cols = r->at("columns").as_array();
        for (const auto& row : r->at("rows").as_array()) {
          bj::object o;
          for (size_t i = 0; i < cols.size(); ++i) o[cols[i].as_string()] = row.as_array()[i];
          rows.push_back(std::move(o));
        }
      }
      for (const auto& row : rows) {
        std::string domain;
        for (const auto& [tn, dm] : tenants_of(*config_.get(), box->manifest)) {
          if (tn == tenant) domain = dm;
        }
        uint8_t rnd[8];
        base::os_random_fill(rnd);
        std::string request_id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
        bj::object doc{{"request_id", request_id}, {"method", "timer"},     {"path", "timer:" + name},
                       {"params", bj::object{}},   {"query", bj::object{}}, {"headers", bj::object{}},
                       {"body", row.is_null() ? std::string() : json::dump(row)},
                       {"domain", domain},         {"tenant", tenant},      {"user", nullptr}};
        CallOptions options;
        options.timeout_ms = t.timeout_ms;
        options.no_response_ok = true;
        auto call = call_box(boxes::RouteRef{box, &t}, doc, tenant, request_id, std::nullopt, options);
        if (call.result.outcome == runtime::wasm::CallResult::Outcome::kOk ||
            call.result.outcome == runtime::wasm::CallResult::Outcome::kNoResponse) {
          ++fired;
        }
      }
      return fired;
    }
  }
  return *error = "no active timer named " + name, std::nullopt;
}

void Host::run_timers() {
  std::unique_lock lock(timer_thread_mu_);
  while (!timers_stopping_) {
    lock.unlock();
    try {
      timer_tick(base::now_unix_ms());
    } catch (const std::exception& e) {
      std::cerr << "goober-host: timers: " << e.what() << "\n";
    }
    lock.lock();
    timer_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return timers_stopping_; });
  }
}

}  // namespace goober::host
