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
  // What each box did with each deletion (021 § 6): acknowledged (it answered under 500) or
  // failing (given up after every try). Nothing yet is pending.
  db.exec("CREATE TABLE IF NOT EXISTS goober_deletion_acks (event_id INTEGER NOT NULL, box TEXT NOT NULL, "
          "state TEXT NOT NULL, at INTEGER NOT NULL, PRIMARY KEY (event_id, box))");
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
        // invite-declined is heard as nobody: the person never signed in (029 § 2.5); and deleted,
        // since the person is gone (021 § 6).
        bool as_nobody = e.kind == route.identity_event &&
                         (e.kind == identity::kInviteDeclined || e.kind == identity::kDeleted);
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
        if (e.kind == identity::kDeleted) body.erase("email");  // the address is gone; the id is what boxes hold
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
        // A box's answer to a deletion is its acknowledgement (or, given up, its failure).
        auto record_ack = [&](const char* state) {
          if (e.kind != identity::kDeleted) return;
          std::lock_guard lock(timers_->mu);
          auto& db = timer_progress(d.tenant);
          ensure_cursors(db);
          sqlite::Stmt s(db, "INSERT OR REPLACE INTO goober_deletion_acks (event_id, box, state, at) VALUES (?, ?, ?, ?)");
          s.bind(1, e.id).bind(2, box_name).bind(3, std::string_view(state)).bind(4, base::now_unix_ms()).run();
        };
        if (ok) {
          access_log(pseudo, call.result.status ? call.result.status : 204, started_us, d.domain, json::dump(call.extra));
          record_ack("acknowledged");
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
          record_ack("failing");
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

// ---- Deletions (021 § 6)

namespace {
constexpr int64_t kErasureDays = 30;
constexpr int64_t kDayMs = 24LL * 3600 * 1000;

std::optional<std::string> string_arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}
}  // namespace

std::optional<identity::Deletion> Host::delete_account(const std::string& tenant, int64_t user_id, const std::string& how,
                                                       const std::string& actor, const std::string& why_given,
                                                       std::string* why) {
  auto gone = identity_->delete_user(tenant, user_id, how, actor, base::now_unix_ms(), why);
  // The audit log names the account by its id; the address is gone from the realm.
  audit("user.delete", actor, "user:" + tenant + "/" + std::to_string(user_id), gone ? "ok" : "refused",
        bj::object{{"tenant", tenant}, {"how", how}, {"why", gone ? why_given : *why}});
  return gone;
}

bj::object Host::deletion_report(const std::string& tenant, int64_t now_ms) {
  auto cfg = config_.get();
  auto active = boxes_->active();
  struct BoxInfo {
    std::string name, route;  // route: the box's "deleted" identity route, or ""
    bool holds_data = false;
  };
  std::vector<BoxInfo> boxes;
  for (const auto& [name, box] : active->boxes) {
    bool serves = false;
    for (const auto& [tn, dm] : tenants_of(*cfg, box->manifest)) serves |= tn == tenant;
    BoxInfo b{name, "", false};
    for (const auto& r : box->manifest.identity) {
      if (r.identity_event == identity::kDeleted) b.route = r.name;
    }
    for (const auto& d : box->manifest.data) {
      if (d.applies_schema() && (serves || d.tenant == tenant)) b.holds_data = true;
    }
    if (!serves && !b.holds_data) continue;
    if (b.route.empty() && !b.holds_data) continue;
    boxes.push_back(std::move(b));
  }
  // What each box did with each deletion, and how far its route has read.
  std::map<std::pair<int64_t, std::string>, std::string> acks;
  std::map<std::string, int64_t> cursors;
  {
    std::lock_guard lock(timers_->mu);
    auto& db = timer_progress(tenant);
    ensure_cursors(db);
    sqlite::Stmt a(db, "SELECT event_id, box, state FROM goober_deletion_acks");
    while (a.step()) acks[{a.int64(0), a.text(1)}] = a.text(2);
    sqlite::Stmt c(db, "SELECT box, route, after_id FROM goober_identity_cursors");
    while (c.step()) cursors[c.text(0) + "|" + c.text(1)] = c.int64(2);
  }
  bj::array list;
  int overdue = 0, open = 0;
  for (const auto& del : identity_->deletions(tenant, 500)) {
    bj::array states;
    bool all_done = true;
    for (const auto& b : boxes) {
      std::string state;
      if (b.route.empty()) {
        state = "no_route";
      } else if (auto it = acks.find({del.event_id, b.name}); it != acks.end()) {
        state = it->second;
      } else if (auto cur = cursors.find(b.name + "|" + b.route); cur != cursors.end() && cur->second >= del.event_id) {
        state = "missed";  // the route started reading after this deletion
      } else {
        state = "pending";
      }
      all_done &= state == "acknowledged";
      states.push_back(bj::object{{"box", b.name}, {"route", b.route.empty() ? bj::value(nullptr) : bj::value(b.route)},
                                  {"state", state}});
    }
    int64_t age_days = (now_ms - del.at_ms) / kDayMs;
    bool late = !all_done && age_days >= kErasureDays;
    open += all_done ? 0 : 1;
    overdue += late ? 1 : 0;
    list.push_back(bj::object{{"user_id", std::to_string(del.user_id)},
                              {"event_id", tenant + ":" + std::to_string(del.event_id)},
                              {"at", base::format_utc_ms(del.at_ms)},
                              {"how", del.how},
                              {"age_days", age_days},
                              {"acknowledged", all_done},
                              {"overdue", late},
                              {"boxes", std::move(states)}});
  }
  return bj::object{{"tenant", tenant},
                    {"erasure_days", kErasureDays},
                    {"unacknowledged", open},
                    {"overdue", overdue},
                    {"deletions", std::move(list)}};
}

void Host::register_deletion_tools() {
  auto tenant_prop = mcp::string_property("A tenant name, like playground.");
  auto known_tenant = [this](const std::optional<std::string>& t) {
    if (!t) return false;
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == *t) return true;
    }
    return false;
  };
  mcp_->add_tool(
      {"user_delete",
       "Deletes a person's account in a tenant's realm, for good: their sessions, agents' grants, roles, and pending "
       "codes end, and their address is erased everywhere the realm keeps it. The user id stays as a tombstone, never "
       "reused; the address may sign up again as a new person. Boxes with a routes.identity \"deleted\" route hear it "
       "and must erase their rows for that user_id within 30 days (see deletions). Give the email or the user_id, and "
       "why (it goes in the audit log). People delete their own accounts at /identity/account.",
       mcp::object_schema({{"tenant", tenant_prop},
                           {"email", mcp::string_property("The person's address.")},
                           {"user_id", mcp::string_property("The person's user id, instead of the address.")},
                           {"why", mcp::string_property("Why, for the audit log: like \"the person asked by email\".")}},
                          {"tenant", "why"}),
       [this, known_tenant](const bj::object& args) {
         auto tenant = string_arg(args, "tenant");
         if (!known_tenant(tenant)) return mcp::ToolResult::fail("not a tenant in the host config");
         auto why_given = string_arg(args, "why").value_or("");
         if (why_given.empty()) return mcp::ToolResult::fail("why is required: it goes in the audit log");
         int64_t user_id = 0;
         if (auto email = string_arg(args, "email")) {
           auto u = identity_->find_user(*tenant, *email);
           if (!u) return mcp::ToolResult::fail("no account with that address in " + *tenant);
           user_id = u->user_id;
         } else if (auto id = string_arg(args, "user_id")) {
           user_id = std::atoll(id->c_str());
         } else if (auto it = args.find("user_id"); it != args.end() && it->value().is_int64()) {
           user_id = it->value().as_int64();
         }
         if (user_id <= 0) return mcp::ToolResult::fail("give the email or the user_id");
         std::string why;
         auto gone = delete_account(*tenant, user_id, "operator", "operator:" + current_operator(), why_given, &why);
         if (!gone) return mcp::ToolResult::fail(why);
         return mcp::ToolResult::ok(bj::object{{"deleted", std::to_string(gone->user_id)},
                                               {"tenant", *tenant},
                                               {"event_id", *tenant + ":" + std::to_string(gone->event_id)},
                                               {"next", "deletions shows which boxes have acknowledged it"}});
       }});
  mcp_->add_tool({"deletions",
                  "Accounts deleted in a tenant's realm, and for each, every box that holds the tenant's data: "
                  "acknowledged (its routes.identity \"deleted\" route answered), pending, failing (given up), missed "
                  "(the route was added after), or no_route. Boxes must erase a deleted person's rows within 30 days; "
                  "overdue marks a deletion past that with a box not acknowledged.",
                  mcp::object_schema({{"tenant", tenant_prop}}, {"tenant"}), [this, known_tenant](const bj::object& args) {
                    auto tenant = string_arg(args, "tenant");
                    if (!known_tenant(tenant)) return mcp::ToolResult::fail("not a tenant in the host config");
                    return mcp::ToolResult::ok(deletion_report(*tenant, base::now_unix_ms()));
                  }});
}

}  // namespace goober::host
