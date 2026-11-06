// A/B splits and the traffic controller (design/human/310.Deployments.md § Splitting traffic,
// § The traffic controller; design/machine/052.Splits.md): a candidate version of a live box
// serves a share of real requests, assigned by a stable hash (a signed-in person by domain and
// user id, anyone else by address), moved up a ladder of shares while guardrails hold, rolled
// back on its own when the candidate's errors or p99 move past them, and promoted at 100%.
// The registry holds the split (boxes/$box/split); this file assigns requests, counts both
// arms, runs the ladder each second on the timer thread, and has the tools.

#include <algorithm>
#include <cmath>
#include <iostream>

#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"
#include "goober/host/host.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

const std::vector<int64_t> kLadder = {100, 500, 2500, 5000, 10000};  // 1%, 5%, 25%, 50%, 100%
constexpr int64_t kGuardMinRequests = 20;  // each arm, in the step, before a guardrail judges
constexpr int64_t kP99FloorUs = 20000;     // a p99 within 20 ms of the live one never trips

std::string text_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

int64_t int_of(const bj::object& o, const char* k, int64_t fallback = 0) {
  const auto* v = o.if_contains(k);
  if (v && v->is_int64()) return v->as_int64();
  if (v && v->is_uint64()) return int64_t(v->as_uint64());
  if (v && v->is_double()) return int64_t(v->as_double());
  return fallback;
}

double num_of(const bj::object& o, const char* k, double fallback) {
  const auto* v = o.if_contains(k);
  if (v && v->is_number()) return v->to_number<double>();
  return fallback;
}

// A share given in percent (0 to 100, decimals allowed) as basis points, or -1.
int64_t percent_bp(const bj::value* v) {
  if (!v || !v->is_number()) return -1;
  double p = v->to_number<double>();
  if (!(p >= 0 && p <= 100)) return -1;
  return int64_t(std::llround(p * 100));
}

double pct(int64_t bp) { return double(bp) / 100.0; }

}  // namespace

// ---- Counting both arms ----

void Host::SplitArm::add(int status, int64_t us) {
  ++requests;
  if (status >= 500) ++errors;
  int b = 0;
  for (int64_t x = std::max<int64_t>(us, 1); x > 1 && b < 39; x >>= 1) ++b;
  ++latency[b];
}

int64_t Host::SplitArm::quantile_us(double q) const {
  if (requests == 0) return 0;
  int64_t want = std::max<int64_t>(1, int64_t(std::ceil(q * double(requests)))), seen = 0;
  for (int b = 0; b < 40; ++b) {
    seen += latency[b];
    if (seen >= want) return int64_t(1) << (b + 1);  // the bucket's top: at most twice the truth
  }
  return int64_t(1) << 40;
}

bj::object Host::SplitArm::json() const {
  return bj::object{{"requests", requests},
                    {"errors", errors},
                    {"error_rate", requests ? double(errors) / double(requests) : 0.0},
                    {"p50_us", quantile_us(0.5)},
                    {"p99_us", quantile_us(0.99)}};
}

int64_t Host::split_bucket(const std::string& salt, const std::string& box, const std::string& key) {
  auto h = base::sha256_hex(salt + "|" + box + "|" + key);
  return int64_t(std::strtoull(h.substr(0, 15).c_str(), nullptr, 16) % 10000);
}

std::optional<boxes::RouteRef> Host::split_arm(const boxes::ActiveSet& active, const boxes::RouteRef& ref,
                                               const std::string& key) {
  if (active.splits.empty() || !ref.box) return std::nullopt;
  auto it = active.splits.find(ref.box->manifest.box);
  if (it == active.splits.end()) return std::nullopt;
  return active.candidate_for(ref, split_bucket(it->second.salt, it->first, key));
}

void Host::split_count(const std::string& box, const std::string& split_version, bool candidate, int status, int64_t us) {
  std::lock_guard lock(splits_mu_);
  auto& s = split_stats_[box];
  if (s.version != split_version) s = SplitStats{split_version};  // a new split, or the first request since a restart
  (candidate ? s.step_candidate : s.step_live).add(status, us);
  (candidate ? s.total_candidate : s.total_live).add(status, us);
}

// ---- The controller ----

std::optional<std::string> Host::split_end(const std::string& box, const std::string& outcome, const std::string& by,
                                           const std::string& why) {
  auto rec = boxes_->split_record(box);
  if (!rec) return "box " + box + " isn't split";
  std::string version = text_of(*rec, "version"), err;
  if (outcome == "promote") {
    // Promoting ends the split in the same step (the registry's activate).
    if (!boxes_->promote(box, version, &err)) return "couldn't promote " + version + ": " + err;
  } else if (!boxes_->split_end(box, &err)) {
    return err;
  }
  bj::object last = *rec;
  last["ended_ms"] = base::now_unix_ms();
  last["outcome"] = outcome;
  last["ended_by"] = by;
  last["end_why"] = why.substr(0, 500);
  {
    std::lock_guard lock(splits_mu_);
    if (auto it = split_stats_.find(box); it != split_stats_.end()) {
      last["live"] = it->second.total_live.json();
      last["candidate"] = it->second.total_candidate.json();
      split_stats_.erase(it);
    }
  }
  last.erase("salt");
  if (!wal_->commit({{wal::FileChange::Kind::kWrite, "boxes/" + box + "/split-last", json::dump(last)}}, &err)) {
    std::cerr << "goober-host: split of " << box << ": " << err << "\n";
  }
  audit(outcome == "promote" ? "split.promote" : "split.rollback", by, "box:" + box, "ok",
        bj::object{{"version", version}, {"why", why.substr(0, 500)}});
  if (changes_) {
    try {
      changes_->record_action(operations::Action{base::now_unix_ms(), by, outcome == "promote" ? "split_promote" : "split_rollback",
                                                 current_request_id(), bj::object{{"box", box}, {"version", version}, {"why", why.substr(0, 500)}},
                                                 false});
    } catch (const std::exception& e) {
      std::cerr << "goober-host: change log: " << e.what() << "\n";
    }
  }
  return std::nullopt;
}

// Why the candidate's step fails a guardrail, or "".
std::string Host::split_breach(const bj::object& rec, const SplitStats& s) {
  const auto& c = s.step_candidate;
  const auto& l = s.step_live;
  if (c.requests < kGuardMinRequests || l.requests < kGuardMinRequests) return "";
  const bj::object guard = rec.contains("guardrails") && rec.at("guardrails").is_object() ? rec.at("guardrails").as_object() : bj::object{};
  double delta = num_of(guard, "error_rate_delta", 0.02), ratio = num_of(guard, "p99_ratio", 1.5);
  double ce = double(c.errors) / double(c.requests), le = double(l.errors) / double(l.requests);
  if (ce - le > delta) {
    return "the candidate's 5xx rate " + std::to_string(int(ce * 1000) / 10.0).substr(0, 5) + "% passed the live version's " +
           std::to_string(int(le * 1000) / 10.0).substr(0, 5) + "% by more than " + std::to_string(int(delta * 1000) / 10.0).substr(0, 4) + " points";
  }
  int64_t cp = c.quantile_us(0.99), lp = l.quantile_us(0.99);
  if (double(cp) > ratio * double(lp) && cp - lp > kP99FloorUs) {
    return "the candidate's p99 " + std::to_string(cp / 1000) + " ms passed " + std::to_string(ratio).substr(0, 4) +
           " times the live version's " + std::to_string(lp / 1000) + " ms";
  }
  return "";
}

void Host::split_tick(int64_t now_ms) {
  auto active = boxes_->active();
  if (active->splits.empty()) return;
  for (const auto& [box, sp] : active->splits) {
    // Read, judged, and written under the lock the tools hold: no step over an operator's change.
    std::lock_guard change(split_change_mu_);
    auto rec = boxes_->split_record(box);
    if (!rec) continue;
    SplitStats s{text_of(*rec, "version")};
    {
      std::lock_guard lock(splits_mu_);
      if (auto it = split_stats_.find(box); it != split_stats_.end() && it->second.version == s.version) s = it->second;
    }
    // Guardrails hold in every mode: a manual share rolls back too.
    if (auto why = split_breach(*rec, s); !why.empty()) {
      if (auto err = split_end(box, "rollback", "host", "guardrail: " + why)) std::cerr << "goober-host: split " << box << ": " << *err << "\n";
      continue;
    }
    if (text_of(*rec, "mode") != "ladder") continue;
    int64_t step_ms = int_of(*rec, "step_minutes", 30) * 60000;
    if (now_ms - int_of(*rec, "step_started_ms", now_ms) < step_ms) continue;
    if (s.step_candidate.requests < int_of(*rec, "min_requests", 200)) continue;  // not enough seen yet
    std::vector<int64_t> ladder;
    if (const auto* l = rec->if_contains("ladder"); l && l->is_array()) {
      for (const auto& x : l->as_array()) {
        if (x.is_int64()) ladder.push_back(x.as_int64());
      }
    }
    if (ladder.empty()) ladder = kLadder;
    int64_t share = int_of(*rec, "share_bp");
    auto next = std::upper_bound(ladder.begin(), ladder.end(), share);
    if (next == ladder.end() || *next >= 10000) {
      if (auto err = split_end(box, "promote", "host", "the ladder reached 100% with every guardrail holding")) {
        std::cerr << "goober-host: split " << box << ": " << *err << "\n";
      }
      continue;
    }
    bj::object r = *rec;
    r["step_started_ms"] = now_ms;
    bj::array hist = r.contains("history") && r.at("history").is_array() ? r.at("history").as_array() : bj::array{};
    hist.push_back(bj::object{{"at_ms", now_ms}, {"share_bp", *next}, {"by", "host"}, {"why", "the step held its guardrails"}});
    r["history"] = std::move(hist);
    std::string err;
    if (!boxes_->split_set(box, s.version, *next, r, true, &err)) {
      std::cerr << "goober-host: split " << box << ": " << err << "\n";
      continue;
    }
    {
      std::lock_guard lock(splits_mu_);
      if (auto it = split_stats_.find(box); it != split_stats_.end()) {
        it->second.step_live = {};
        it->second.step_candidate = {};
      }
    }
    audit("split.step", "host", "box:" + box, "ok", bj::object{{"version", s.version}, {"share_percent", pct(*next)}});
  }
}

bj::object Host::split_report_of(const std::string& box) {
  bj::object out{{"box", box}};
  auto rec = boxes_->split_record(box);
  auto active = boxes_->active();
  if (auto it = active->boxes.find(box); it != active->boxes.end()) out["live_version"] = it->second->version;
  if (rec) {
    bj::object r = *rec;
    r.erase("salt");
    out["state"] = "splitting";
    out["candidate_version"] = r.at("version");
    out["share_percent"] = pct(int_of(r, "share_bp"));
    out["mode"] = r.contains("mode") ? r.at("mode") : bj::value("manual");
    if (text_of(r, "mode") == "ladder") {
      out["next_step_at_ms"] = int_of(r, "step_started_ms") + int_of(r, "step_minutes", 30) * 60000;
    }
    out["split"] = std::move(r);
    std::lock_guard lock(splits_mu_);
    if (auto it = split_stats_.find(box); it != split_stats_.end()) {
      out["step"] = bj::object{{"live", it->second.step_live.json()}, {"candidate", it->second.step_candidate.json()}};
      out["since_start"] = bj::object{{"live", it->second.total_live.json()}, {"candidate", it->second.total_candidate.json()}};
    } else {
      out["step"] = bj::object{{"live", SplitArm{}.json()}, {"candidate", SplitArm{}.json()}};
      out["since_start"] = out["step"];
    }
  } else {
    out["state"] = "none";
  }
  if (auto text = base::read_file(config_.get()->storage / "boxes" / box / "split-last")) {
    std::string err;
    if (auto v = json::parse(*text, &err)) out["last"] = *v;
  }
  return out;
}

void Host::register_split_tools() {
  using mcp::any_property;
  using mcp::integer_property;
  using mcp::object_schema;
  using mcp::string_property;
  auto box_prop = string_property("The live box.");

  mcp_->add_tool(
      {"split_start",
       "Starts an A/B split of a live box: `version` (stored dark with version_upload {activate: false}) serves a share "
       "of real requests, with real effects, beside the live version. Each request is assigned by a stable hash (a "
       "signed-in person by domain and user id, anyone else by address), so a person stays on one version for the "
       "whole split. HTTP routes and the box's MCP tools split; timers, mail, internal calls, jobs, and identity "
       "events stay on the live version. Both versions must answer the same routes and bind the same data "
       "(otherwise refused, naming each difference). With a ladder (the default: 1, 5, 25, 50, 100 percent), the "
       "host moves up a step every step_minutes (default 30) once the candidate served min_requests (default 200) "
       "in the step, and promotes it at 100%. Guardrails, judged once each arm served 20 requests in a step: the "
       "candidate's 5xx rate past the live one's by error_rate_delta (default 0.02), or its p99 past p99_ratio (default "
       "1.5) times the live one's (and by 20 ms): the host rolls back on its own. With `share` instead, the split "
       "holds that percent until split_set or split_stop. Audited. Watch it with split_report.",
       object_schema({{"box", box_prop},
                      {"version", string_property("The candidate version (version_list).")},
                      {"why", string_property("What the candidate changes, for the audit log.")},
                      {"share", any_property("A fixed share in percent (like 5) instead of the ladder.")},
                      {"ladder", any_property("The ladder's shares in percent, rising, ending at 100, like [1, 10, 50, 100].")},
                      {"step_minutes", integer_property("Minutes on each step, 1 to 1440; 30 when left out.")},
                      {"min_requests", integer_property("Requests the candidate serves in a step before the next, 1 to 1000000; 200.")},
                      {"guardrails", any_property("{error_rate_delta?: 0 to 1 (0.02), p99_ratio?: 1 to 100 (1.5)}.")}},
                     {"box", "version", "why"}),
       [this](const bj::object& args) {
         std::lock_guard change(split_change_mu_);
         std::string box = text_of(args, "box"), version = text_of(args, "version"), why = text_of(args, "why");
         if (why.empty()) return ToolResult::fail("why is required: say what the candidate changes");
         if (boxes_->split_record(box)) return ToolResult::fail(box + " is split already: split_set changes it, split_stop ends it");
         bj::object rec{{"by", current_operator()}, {"why", why.substr(0, 500)}, {"started_ms", base::now_unix_ms()},
                        {"step_started_ms", base::now_unix_ms()}};
         int64_t share = -1;
         if (args.contains("share")) {
           share = percent_bp(args.if_contains("share"));
           if (share < 0) return ToolResult::fail("share is a percent, 0 to 100");
           rec["mode"] = "manual";
         } else {
           bj::array ladder;
           if (const auto* l = args.if_contains("ladder")) {
             if (!l->is_array() || l->as_array().empty() || l->as_array().size() > 10) return ToolResult::fail("ladder is 1 to 10 percents");
             int64_t prev = 0;
             for (const auto& x : l->as_array()) {
               int64_t bp = percent_bp(&x);
               if (bp <= prev) return ToolResult::fail("ladder rises: each share in percent, above the one before, at most 100");
               ladder.push_back(bp);
               prev = bp;
             }
             if (prev != 10000) return ToolResult::fail("ladder ends at 100");
           } else {
             for (auto bp : kLadder) ladder.push_back(bp);
           }
           share = ladder.front().as_int64();
           rec["mode"] = "ladder";
           rec["ladder"] = std::move(ladder);
           auto sm = mcp::integer_arg(args, "step_minutes");
           auto mr = mcp::integer_arg(args, "min_requests");
           if ((sm.present && (!sm.ok || sm.value < 1 || sm.value > 1440)) ||
               (mr.present && (!mr.ok || mr.value < 1 || mr.value > 1000000))) {
             return ToolResult::fail("step_minutes is 1 to 1440 and min_requests 1 to 1000000");
           }
           rec["step_minutes"] = sm.ok ? sm.value : 30;
           rec["min_requests"] = mr.ok ? mr.value : 200;
         }
         bj::object guard{{"error_rate_delta", 0.02}, {"p99_ratio", 1.5}};
         if (const auto* g = args.if_contains("guardrails")) {
           if (!g->is_object()) return ToolResult::fail("guardrails is {error_rate_delta?, p99_ratio?}");
           for (const auto& kv : g->as_object()) {
             double v = kv.value().is_number() ? kv.value().to_number<double>() : -1;
             if (kv.key() == "error_rate_delta" && v >= 0 && v <= 1) guard["error_rate_delta"] = v;
             else if (kv.key() == "p99_ratio" && v >= 1 && v <= 100) guard["p99_ratio"] = v;
             else return ToolResult::fail("guardrails takes error_rate_delta (0 to 1) and p99_ratio (1 to 100)");
           }
         }
         rec["guardrails"] = std::move(guard);
         rec["history"] = bj::array{bj::object{{"at_ms", base::now_unix_ms()}, {"share_bp", share}, {"by", current_operator()}, {"why", "started"}}};
         std::string err;
         if (!boxes_->split_set(box, version, share, rec, false, &err)) return ToolResult::fail(err);
         {
           std::lock_guard lock(splits_mu_);
           split_stats_.insert_or_assign(box, SplitStats{version});
         }
         audit("split.start", current_operator(), "box:" + box, "ok",
               bj::object{{"version", version}, {"share_percent", pct(share)}, {"mode", rec.at("mode")}, {"why", why.substr(0, 500)}});
         // The change log's row is the generic one every operator call writes (operations.cpp).
         return ToolResult::ok(split_report_of(box));
       }});

  mcp_->add_tool(
      {"split_set",
       "Changes a running split: `share` (percent, 0 to below 100) holds that share and pauses the ladder (0 sends everyone "
       "to the live version while the split stays); mode \"ladder\" resumes the ladder from the next step above the "
       "share. Guardrails keep holding either way. Audited.",
       object_schema({{"box", box_prop},
                      {"share", any_property("The share in percent.")},
                      {"mode", mcp::enum_property("ladder resumes the ladder.", {"ladder"})},
                      {"why", string_property("Why, for the audit log.")}},
                     {"box"}),
       [this](const bj::object& args) {
         std::lock_guard change(split_change_mu_);
         std::string box = text_of(args, "box");
         auto rec = boxes_->split_record(box);
         if (!rec) return ToolResult::fail("box " + box + " isn't split (split_start starts one)");
         bj::object r = *rec;
         int64_t share = int_of(r, "share_bp");
         if (args.contains("share")) {
           share = percent_bp(args.if_contains("share"));
           if (share < 0 || share >= 10000) return ToolResult::fail("share is a percent, 0 to below 100 (split_stop promote goes all the way)");
           r["mode"] = "manual";
         }
         if (text_of(args, "mode") == "ladder") {
           if (!r.contains("ladder")) {
             bj::array l;
             for (auto bp : kLadder) l.push_back(bp);
             r["ladder"] = std::move(l);
             r["step_minutes"] = int_of(r, "step_minutes", 30);
             r["min_requests"] = int_of(r, "min_requests", 200);
           }
           r["mode"] = "ladder";
         }
         r["step_started_ms"] = base::now_unix_ms();
         bj::array hist = r.contains("history") && r.at("history").is_array() ? r.at("history").as_array() : bj::array{};
         hist.push_back(bj::object{{"at_ms", base::now_unix_ms()}, {"share_bp", share}, {"by", current_operator()}, {"why", text_of(args, "why")}});
         r["history"] = std::move(hist);
         std::string err;
         if (!boxes_->split_set(box, text_of(r, "version"), share, r, true, &err)) return ToolResult::fail(err);
         {
           std::lock_guard lock(splits_mu_);
           if (auto it = split_stats_.find(box); it != split_stats_.end()) {
             it->second.step_live = {};
             it->second.step_candidate = {};
           }
         }
         audit("split.set", current_operator(), "box:" + box, "ok",
               bj::object{{"share_percent", pct(share)}, {"mode", r.at("mode")}, {"why", text_of(args, "why")}});
         return ToolResult::ok(split_report_of(box));
       }});

  mcp_->add_tool(
      {"split_stop",
       "Ends a split: outcome \"rollback\" sends everyone to the live version at once (the candidate stays stored); "
       "\"promote\" makes the candidate the live version. Either way the split's totals are kept as split_report's "
       "`last`. Audited.",
       object_schema({{"box", box_prop},
                      {"outcome", mcp::enum_property("rollback or promote.", {"rollback", "promote"})},
                      {"why", string_property("Why, for the audit log.")}},
                     {"box", "outcome"}),
       [this](const bj::object& args) {
         std::lock_guard change(split_change_mu_);
         std::string box = text_of(args, "box"), outcome = text_of(args, "outcome");
         if (outcome != "rollback" && outcome != "promote") return ToolResult::fail("outcome is rollback or promote");
         if (auto err = split_end(box, outcome, current_operator(), text_of(args, "why"))) return ToolResult::fail(*err);
         return ToolResult::ok(split_report_of(box));
       }});

  mcp_->add_tool(
      {"split_report",
       "Read tool. One box's split: state (splitting or none), the live and candidate versions, share_percent, mode "
       "(ladder or manual), the next step's time, the guardrails, history (every share and who set it), and both arms "
       "side by side for the current step and since the start (requests, errors, error_rate, p50_us, p99_us; counted "
       "since the split started or the host last started); and `last`, the previous split's outcome and totals.",
       object_schema({{"box", box_prop}}, {"box"}), [this](const bj::object& args) {
         return ToolResult::ok(split_report_of(text_of(args, "box")));
       }});

  mcp_->add_tool(
      {"split_list",
       "Read tool. Every running split: box, versions, share_percent, mode, and both arms since the start.",
       object_schema({}, {}), [this](const bj::object&) {
         bj::array out;
         for (const auto& [box, sp] : boxes_->active()->splits) {
           auto r = split_report_of(box);
           out.push_back(bj::object{{"box", box},
                                    {"live_version", r.contains("live_version") ? r.at("live_version") : bj::value(nullptr)},
                                    {"candidate_version", r.at("candidate_version")},
                                    {"share_percent", r.at("share_percent")},
                                    {"mode", r.at("mode")},
                                    {"since_start", r.at("since_start")}});
         }
         return ToolResult::ok(std::move(out));
       }});
}

}  // namespace goober::host
