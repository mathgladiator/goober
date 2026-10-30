// Identity routes (design/machine/021.IdentityEvents.md): a box declares routes.identity
// "first-verified" or "signed-in", and the host calls it after each such sign-in in the realm
// of a domain the box serves. Sign-in appends the event in its own transaction; this tick reads
// the realm's events after each route's cursor and calls the route once per event, as the person
// who signed in. Delivery is at least once, in order, per route: the cursor moves past an event
// only when the call succeeded or gave up. Sign-in never waits on a box.

#include <functional>
#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;

namespace {

constexpr int kBatch = 50;               // events read per route per tick
constexpr int kMaxFailures = 8;          // calls to one event before it is skipped
constexpr int64_t kMaxBackoffMs = 10 * 60 * 1000;
constexpr int64_t kPruneEveryMs = 3600 * 1000;

void ensure_cursors(sqlite::Db& db) {
  db.exec("CREATE TABLE IF NOT EXISTS goober_identity_cursors (box TEXT NOT NULL, route TEXT NOT NULL, "
          "after_id INTEGER NOT NULL, failures INTEGER NOT NULL DEFAULT 0, retry_at INTEGER NOT NULL DEFAULT 0, "
          "PRIMARY KEY (box, route))");
}

}  // namespace

void Host::identity_tick(int64_t now_ms) {
  auto cfg = config_.get();
  auto active = boxes_->active();

  // Hourly: expired codes, old events, and invites never taken up (identity::prune).
  bool prune = false;
  {
    std::lock_guard lock(timers_->mu);
    if (now_ms - timers_->identity_pruned_ms >= kPruneEveryMs) {
      timers_->identity_pruned_ms = now_ms;
      prune = true;
    }
  }
  if (prune) {
    std::set<std::string> realms{std::string(identity::kHostRealm)};
    for (const auto& [domain, entry] : cfg->domains) realms.insert(entry.tenant);
    // Each step, and each realm, on its own: one unreadable database never stops the rest
    // (log rotation above all, or the disk fills).
    auto step = [&](const std::string& what, const std::function<void()>& f) {
      try {
        f();
      } catch (const std::exception& e) {
        std::cerr << "goober-host: housekeeping: " << what << ": " << e.what() << "\n";
        loop_trouble("identity", what + ": " + e.what());
      }
    };
    for (const auto& realm : realms) {
      step("pruning realm " + realm, [&] {
        int gone = identity_->prune(realm, now_ms);
        if (gone > 0) std::cerr << "goober-host: identity: deleted " << gone << " never-verified account(s) in " << realm << "\n";
      });
    }
    // Received attachments past mail.inbound.attachment-days, and expired reply tokens.
    step("mail", [&] { prune_mail(now_ms); });
    // Invites' one reminder each, and consent's old log rows (029).
    step("invites", [&] { remind_invites(now_ms); });
    step("consent", [&] { consent_->prune(now_ms); });
    // Quiet logs rotate, and rotated files past 7 days leave the machine (032 § 2).
    step("logs", [&] { logs_housekeeping(now_ms); });
  }

  struct Delivery {
    boxes::RouteRef ref;
    std::string tenant, domain, key;
    int64_t after_id = 0;
    int failures = 0;
    std::vector<identity::Event> events;
  };
  std::vector<Delivery> work;
  for (const auto& [box_name, box] : active->boxes) {
    for (const auto& r : box->manifest.identity) {
      for (const auto& [tenant, domain] : tenants_of(*cfg, box->manifest)) {
        // One tenant's unreadable cursors or realm never hold up another tenant's events.
        try {
          std::string key = tenant + "|" + box_name + "|" + r.name;
          int64_t after_id = 0;
          int failures = 0;
          {
            std::lock_guard lock(timers_->mu);
            if (timers_->identity_running.count(key)) continue;
            auto& db = timer_progress(tenant);
            ensure_cursors(db);
            sqlite::Stmt s(db, "SELECT after_id, failures, retry_at FROM goober_identity_cursors WHERE box = ? AND route = ?");
            s.bind(1, box_name).bind(2, r.name);
            if (!s.step()) {
              // A new route hears what happens from now on, not the realm's history.
              sqlite::Stmt put(db, "INSERT INTO goober_identity_cursors (box, route, after_id) VALUES (?, ?, ?)");
              put.bind(1, box_name).bind(2, r.name).bind(3, identity_->last_event_id(tenant)).run();
              continue;
            }
            if (s.int64(2) > now_ms) continue;
            after_id = s.int64(0);
            failures = int(s.int64(1));
          }
          auto events = identity_->events_after(tenant, after_id, kBatch);
          if (events.empty()) continue;
          {
            std::lock_guard lock(timers_->mu);
            timers_->identity_running.insert(key);
          }
          work.push_back(Delivery{boxes::RouteRef{box, &r}, tenant, domain, key, after_id, failures, std::move(events)});
        } catch (const std::exception& e) {
          std::cerr << "goober-host: identity events: " << tenant << ": " << e.what() << "\n";
          loop_trouble("identity", tenant + ": " + e.what());
        }
      }
    }
  }

  for (auto& d : work) {
    auto job = [this, d, now_ms]() mutable {
      struct Release {
        Host* host;
        std::string key;
        ~Release() {
          std::lock_guard lock(host->timers_->mu);
          host->timers_->identity_running.erase(key);
        }
      } release{this, d.key};
      const auto& route = *d.ref.route;
      const std::string& box_name = d.ref.box->manifest.box;
      auto save = [&](int64_t after_id, int failures, int64_t retry_at) {
        std::lock_guard lock(timers_->mu);
        auto& db = timer_progress(d.tenant);
        sqlite::Stmt s(db, "UPDATE goober_identity_cursors SET after_id = ?, failures = ?, retry_at = ? WHERE box = ? AND route = ?");
        s.bind(1, after_id).bind(2, int64_t(failures)).bind(3, retry_at).bind(4, box_name).bind(5, route.name).run();
      };
      for (const auto& e : d.events) {
        // Other kinds move the cursor without a call, as does a person since deleted or disabled.
        auto person = e.kind == route.identity_event ? identity_->user(d.tenant, e.user_id) : std::nullopt;
        // invite-declined is heard as nobody: the person never signed in (029 § 2.5).
        bool as_nobody = e.kind == route.identity_event && e.kind == identity::kInviteDeclined;
        if (!person && !as_nobody) {
          save(e.id, 0, 0);
          d.after_id = e.id;
          d.failures = 0;
          continue;
        }
        int64_t started_us = base::monotonic_us();
        uint8_t rnd[8];
        base::os_random_fill(rnd);
        std::string request_id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
        std::optional<SignedIn> who;
        if (person) who = SignedIn{*person, false, person->roles, false, ""};
        // The event is the body; the person is the caller, as on their own request.
        bj::object body{{"event", e.kind},
                        {"event_id", d.tenant + ":" + std::to_string(e.id)},  // the same on every retry
                        {"user_id", std::to_string(e.user_id)},
                        {"email", e.email},
                        {"at", base::format_utc_ms(e.at_ms)}};
        if (e.invite_id) {
          body["invite_id"] = std::to_string(e.invite_id);
          body["box"] = e.box.empty() ? bj::value(nullptr) : bj::value(e.box);
          if (as_nobody) body["user_id"] = nullptr;
        }
        bj::object doc{{"request_id", request_id}, {"method", "identity"},  {"path", "identity:" + e.kind},
                       {"params", bj::object{}},   {"query", bj::object{}}, {"headers", bj::object{}},
                       {"body", json::dump(body)}, {"domain", d.domain},    {"tenant", d.tenant},
                       {"user", user_json(who)}};
        CallOptions options;
        options.timeout_ms = route.timeout_ms;
        options.no_response_ok = true;
        auto call = call_box(d.ref, doc, d.tenant, request_id, who, options);
        bool ok = (call.result.outcome == runtime::wasm::CallResult::Outcome::kOk && call.result.status < 500) ||
                  call.result.outcome == runtime::wasm::CallResult::Outcome::kNoResponse;
        call.extra["identity"] = e.kind;
        call.extra["tenant"] = d.tenant;
        call.extra["event_id"] = e.id;
        call.extra["user_id"] = std::to_string(e.user_id);
        server::http::Request pseudo;
        pseudo.method = "identity";
        pseudo.path = "identity:" + route.name;
        pseudo.headers.emplace_back("goober-request-id", request_id);
        if (ok) {
          access_log(pseudo, call.result.status ? call.result.status : 204, started_us, d.domain, json::dump(call.extra));
          save(e.id, 0, 0);
          d.after_id = e.id;
          d.failures = 0;
          continue;
        }
        int failures = d.failures + 1;
        if (failures >= kMaxFailures) {
          // Given up: the route doesn't hold back every later sign-in for one it can't take.
          call.extra["gave_up"] = true;
          access_log(pseudo, 500, started_us, d.domain, json::dump(call.extra));
          std::cerr << "goober-host: identity route " << route.name << " in " << box_name << " gave up on event " << e.id
                    << " after " << failures << " tries\n";
          save(e.id, 0, 0);
          d.after_id = e.id;
          d.failures = 0;
          continue;
        }
        access_log(pseudo, 500, started_us, d.domain, json::dump(call.extra));
        int64_t backoff = std::min<int64_t>(kMaxBackoffMs, 1000LL << std::min(failures, 20));
        save(d.after_id, failures, now_ms + backoff);
        return;  // in order: nothing after this event until it goes through or is given up
      }
    };
    bool run_inline;
    {
      std::lock_guard lock(timers_->mu);
      run_inline = timers_->inline_fires;
    }
    if (run_inline) {
      job();
    } else if (!pool_->post(job, max_queued_)) {
      std::lock_guard lock(timers_->mu);
      timers_->identity_running.erase(d.key);  // the queue is full; next tick
    }
  }
}

}  // namespace goober::host
