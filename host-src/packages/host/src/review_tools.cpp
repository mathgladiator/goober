// Reviewing changes to boxes (design/machine/005.FirstVersion.md § 9.1): version_diff (what one
// version changes from another, each change classified widens, narrows, or neutral), the review
// version_upload answers and a held upload shows the person approving it, and version_health
// (how a version has done since it went live, against the one before it).

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/boxes/review.h"
#include "goober/host/host.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

std::optional<std::string> text_of(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

// "2026-10-31T03:46:46.556Z" (base::format_utc_ms) as unix ms, or 0.
int64_t parse_utc_ms(std::string_view s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, ms = 0;
  if (std::sscanf(std::string(s).c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y, &mo, &d, &h, &mi, &sec, &ms) < 6) return 0;
  // Days from the civil date (Howard Hinnant's algorithm).
  y -= mo <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = unsigned(y - era * 400);
  unsigned doy = (153 * unsigned(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + unsigned(d) - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468;
  return ((days * 24 + h) * 60 + mi) * 60000 + int64_t(sec) * 1000 + ms;
}

struct Sample {
  int64_t calls = 0, failures = 0;
  std::vector<int64_t> us;
  bj::object json() {
    std::sort(us.begin(), us.end());
    auto pct = [&](double p) -> int64_t {
      if (us.empty()) return 0;
      return us[std::min(us.size() - 1, size_t(p * double(us.size() - 1) + 0.5))];
    };
    double rate = calls ? double(failures) / double(calls) : 0;
    return bj::object{{"calls", calls}, {"failures", failures}, {"error_rate", rate}, {"p50_us", pct(0.5)}, {"p99_us", pct(0.99)}};
  }
  double rate() const { return calls ? double(failures) / double(calls) : 0; }
};

std::string percent(double r) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f%%", r * 100);
  return buf;
}

}  // namespace

std::optional<bj::object> Host::review_upload(const bj::object& args, std::string* error) {
  auto it = args.find("manifest");
  if (it == args.end()) return *error = "manifest is required", std::nullopt;
  boxes::VersionFiles to;
  to.manifest_text = it->value().is_string() ? std::string(it->value().get_string()) : json::pretty(it->value());
  if (auto wat = text_of(args, "wat")) {
    if (!runtime::wasm::Engine::wat_to_wasm(*wat, &to.wasm, error)) return std::nullopt;
  } else if (auto b64 = text_of(args, "wasm_base64")) {
    auto decoded = base::base64_decode(*b64);
    if (!decoded) return *error = "wasm_base64 is not valid base64", std::nullopt;
    to.wasm = std::move(*decoded);
  }
  if (auto t = args.find("types"); t != args.end() && !t->value().is_null()) {
    to.types_text = t->value().is_string() ? std::string(t->value().get_string()) : json::pretty(t->value());
  }
  auto m = manifest::parse_manifest(to.manifest_text, error);
  if (!m) return std::nullopt;
  // Against what serves now: the live version, or the one a disabled box would come back with.
  std::optional<boxes::VersionFiles> from;
  auto active = boxes_->active();
  if (auto b = active->boxes.find(m->box); b != active->boxes.end()) {
    from = boxes_->files(m->box, b->second->version);
  } else if (auto d = boxes_->disabled(m->box); d && !d->version.empty()) {
    from = boxes_->files(m->box, d->version);
  }
  return boxes::review(from ? &*from : nullptr, to, error);
}

void Host::register_review_tools() {
  auto box_prop = mcp::string_property("The box's name, as box_list or version_list shows it.");

  mcp_->add_tool(
      {"version_diff",
       "Read tool. What one stored version of a box changes from another, for a reviewer to read in one pass: routes "
       "(added, removed, and changed: who may call them, public, agents, CORS, cache and browser-ms, template, "
       "timeout, body, response), tools, timers, jobs, mail addresses, identity events; data strategies and their "
       "grants per table, column, and operation; external services and methods (effects, budgets, From addresses); "
       "flags, libraries, templates, domains, limits; and whether the module (sha256, bytes) or the types document "
       "changed. Each change has a class: widens (more reach: a new grant, a write allowed, a route made public or "
       "easier to call, a new mail address, service, or From address, browsers keeping answers, a new flag value), "
       "narrows, or neutral; widens repeats the widenings as sentences. Defaults: from is the live version (or the "
       "one a disabled box had), to is the newest stored (by stored_at in version_list). Changes nothing.",
       mcp::object_schema({{"box", box_prop},
                           {"from", mcp::string_property("A stored version (default: the live one).")},
                           {"to", mcp::string_property("A stored version (default: the newest stored).")}},
                          {"box"}),
       [this](const bj::object& args) {
         auto box = text_of(args, "box").value_or("");
         auto versions = boxes_->versions(box);
         if (versions.empty()) return ToolResult::fail("box " + box + " has no stored versions");
         std::string from = text_of(args, "from").value_or("");
         std::string to = text_of(args, "to").value_or("");
         if (from.empty()) {
           for (const auto& v : versions) {
             if (v.active) from = v.version;
           }
           if (from.empty()) {
             if (auto d = boxes_->disabled(box)) from = d->version;
           }
         }
         if (to.empty()) {
           const boxes::VersionInfo* newest = &versions.front();
           for (const auto& v : versions) {
             if (v.stored_ms > newest->stored_ms) newest = &v;
           }
           to = newest->version;
         }
         auto b = boxes_->files(box, to);
         if (!b) return ToolResult::fail("box " + box + " has no version " + to);
         std::optional<boxes::VersionFiles> a;
         if (!from.empty()) {
           a = boxes_->files(box, from);
           if (!a) return ToolResult::fail("box " + box + " has no version " + from);
         }
         std::string err;
         auto r = boxes::review(a ? &*a : nullptr, *b, &err);
         if (!r) return ToolResult::fail(err);
         if (from == to) (*r)["note"] = "from and to are the same version: nothing to compare (pass from or to)";
         return ToolResult::ok(std::move(*r));
       }});

  mcp_->add_tool(
      {"version_health",
       "Read tool. How a box's live version has done since it went live, per route, against the version it replaced: "
       "calls, failures (5xx and box failures), error rate, p50 and p99 (µs), from the access log, over minutes after "
       "it went live and the same minutes before. Findings name a route whose errors or p99 rose, or that stopped being "
       "called, and give the rollback (version_promote {box, version: the previous}). It never rolls back by itself. "
       "When it went live comes from the change log's deploy row; a version with no such row (before the change log) "
       "is compared with nothing.",
       mcp::object_schema({{"box", box_prop},
                           {"minutes", mcp::integer_property("The window on each side of going live, 5 to 1440 (default 60).")}},
                          {"box"}),
       [this](const bj::object& args) {
         auto box = text_of(args, "box").value_or("");
         auto active = boxes_->active();
         auto b = active->boxes.find(box);
         if (b == active->boxes.end()) return ToolResult::fail("box " + box + " isn't live (box_list shows the live boxes)");
         std::string version = b->second->version;
         int64_t minutes = 60;
         if (auto m = args.find("minutes"); m != args.end() && m->value().is_int64()) minutes = m->value().as_int64();
         minutes = std::clamp<int64_t>(minutes, 5, 1440);
         int64_t window = minutes * 60000, now = base::now_unix_ms();
         // When this version went live, and what it replaced: the newest deploy row naming it.
         int64_t live_ms = 0;
         std::string previous;
         if (changes_) {
           for (const auto& row : changes_->list("deploy", "", 0, 1000)) {
             const auto& o = row.as_object();
             if (o.at("target").as_string() != box) continue;
             const auto& d = o.at("detail").as_object();
             auto* v = d.if_contains("version");
             if (!v || !v->is_string() || v->as_string() != version) continue;
             live_ms = o.at("at_ms").as_int64();
             if (auto* p = d.if_contains("previous"); p && p->is_string()) previous = std::string(p->as_string());
             break;
           }
         }
         bj::object out{{"box", box}, {"version", version}, {"minutes", minutes}};
         if (!live_ms) {
           out["note"] = "the change log has no record of when this version went live, so there is nothing to compare";
           return ToolResult::ok(std::move(out));
         }
         out["went_live_at"] = base::format_utc_ms(live_ms);
         out["live_for_minutes"] = (now - live_ms) / 60000;
         out["previous"] = previous.empty() ? bj::value(nullptr) : bj::value(previous);
         // The access log, newest files last: access.log.1 (rotated) then access.log.
         std::map<std::string, Sample> before, after;
         auto logs = config_.get()->storage / "logs";
         for (const auto& file : {logs / "access.log.1", logs / "access.log"}) {
           for (const auto& line : base::tail_lines(file, 200000)) {
             if (line.find("\"box\":\"" + box + "\"") == std::string::npos) continue;
             std::string err;
             auto v = json::parse(line, &err);
             if (!v || !v->is_object()) continue;
             const auto& o = v->as_object();
             auto str = [&](const char* k) {
               auto* x = o.if_contains(k);
               return x && x->is_string() ? std::string(x->as_string()) : std::string();
             };
             if (str("box") != box) continue;
             int64_t at = parse_utc_ms(str("at"));
             std::string ver = str("version"), route = str("route");
             Sample* s = nullptr;
             if (ver == version && at >= live_ms && at < live_ms + window) s = &after[route];
             else if (!previous.empty() && ver == previous && at >= live_ms - window && at < live_ms) s = &before[route];
             if (!s) continue;
             ++s->calls;
             int64_t status = o.contains("status") && o.at("status").is_int64() ? o.at("status").as_int64() : 0;
             std::string outcome = str("outcome");
             if (status >= 500 || (!outcome.empty() && outcome != "ok" && outcome != "no_response")) ++s->failures;
             if (auto* us = o.if_contains("us"); us && us->is_int64()) s->us.push_back(us->as_int64());
           }
         }
         bj::array routes, findings;
         std::set<std::string> names;
         for (const auto& [r, _] : before) names.insert(r);
         for (const auto& [r, _] : after) names.insert(r);
         std::string rollback = previous.empty() ? "" : "version_promote {box: \"" + box + "\", version: \"" + previous + "\"} rolls back";
         auto finding = [&](const std::string& route, const std::string& kind, const std::string& text) {
           findings.push_back(bj::object{{"route", route}, {"kind", kind},
                                         {"text", text + (rollback.empty() ? "" : "; " + rollback)}});
         };
         for (const auto& r : names) {
           Sample& x = before[r];
           Sample& y = after[r];
           double rx = x.rate(), ry = y.rate();
           auto bx = x.json(), by = y.json();
           int64_t px = bx.at("p99_us").as_int64(), py = by.at("p99_us").as_int64();
           std::string since = " since version " + version + " went live " + std::to_string((now - live_ms) / 60000) + " minutes ago";
           if (y.failures >= 3 && ry >= 0.01 && ry > 2 * rx) {
             finding(r, "errors", "route " + r + ": failures rose from " + percent(rx) + " to " + percent(ry) + since);
           }
           if (y.calls >= 20 && x.calls >= 20 && py > 2 * px && py - px > 50000) {
             finding(r, "latency", "route " + r + ": p99 rose from " + std::to_string(px / 1000) + " ms to " +
                                       std::to_string(py / 1000) + " ms" + since);
           }
           if (x.calls >= 20 && y.calls == 0 && now - live_ms > window / 2) {
             finding(r, "silent", "route " + r + ": " + std::to_string(x.calls) + " calls before, none" + since +
                                     " (removed, renamed, or no longer reached?)");
           }
           routes.push_back(bj::object{{"route", r}, {"before", std::move(bx)}, {"after", std::move(by)}});
         }
         out["routes"] = std::move(routes);
         out["findings"] = std::move(findings);
         if (!rollback.empty()) out["rollback"] = rollback;
         return ToolResult::ok(std::move(out));
       }});
}

}  // namespace goober::host
