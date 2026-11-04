// Heat studies in the host (design/machine/048.HeatStudy.md): how a finished request on a studied
// domain is keyed by endpoint (heat_observe, called beside the access log and live statistics),
// the timer thread's tick, and the tools. The accumulators and the report are
// goober/operations/heat.h.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/router/path.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;
using server::http::Request;

namespace {

std::string text_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

std::string_view view_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string_view(v->as_string()) : std::string_view();
}

}  // namespace

operations::HeatStudies::ClientHash Host::heat_client_hash() {
  return [this](std::string_view client) {
    return std::strtoull(address_hash_for_logs(std::string(client)).c_str(), nullptr, 16);
  };
}

void Host::heat_observe(const Request& req, const bj::object& line, const std::string& domain, int64_t bytes_out) {
  // Not a visitor: a try, the panel, or the host's own calls (timers, outbox results, identity
  // events, internal calls), whose paths don't start with "/". Nor the host's own paths: CORS
  // preflights (every OPTIONS: no box route takes one) and /goober/*.
  if (line.contains("try") || line.contains("panel") || !req.path.starts_with("/") || req.method == "options") return;
  auto path = router::normalize_path(req.path);
  std::string_view p = path ? std::string_view(path->path) : std::string_view(req.path);
  if (p == "/goober" || p.starts_with("/goober/") || p == kOperationsMcpPath || p == kMailHookPath) return;
  operations::HeatSample s;
  s.at_ms = base::now_unix_ms();
  s.end_us = base::monotonic_us();
  if (const auto* us = line.if_contains("us"); us && us->is_int64()) s.us = us->as_int64();
  if (const auto* st = line.if_contains("status"); st && st->is_int64()) s.status = int(st->as_int64());
  s.bytes_in = int64_t(req.body.size());
  s.bytes_out = std::max<int64_t>(0, bytes_out);
  s.method = req.method == "head" ? std::string_view("get") : std::string_view(req.method);
  // Held for the call: the box route's names are views into it.
  std::shared_ptr<const boxes::ActiveSet> active;
  if (!path) {
    s.what = "(refused)";
    s.method = {};
  } else if (p == "/mcp") {
    s.api = operations::HeatApi::kMcp;
    s.what = view_of(line, "tool");
    if (s.what.empty()) s.what = "(rpc)";
    s.method = {};
  } else if (p.starts_with("/identity/") || p == "/oauth" || p.starts_with("/oauth/") || p.starts_with("/.well-known/")) {
    s.api = operations::HeatApi::kIdentity;
    // Invite links are capabilities: one endpoint, never a token.
    s.what = p.starts_with("/identity/invite/") ? std::string_view("/identity/invite/{rest*}") : p;
  } else if (p == "/attachment" || p.starts_with("/attachment/")) {
    s.what = "/attachment/{file}";
  } else {
    // A box route, by the same match the dispatcher made: its declared pattern, whoever answered
    // (the box, or the host refusing before it ran).
    active = boxes_->active();
    const auto& table = active->table_for(domain);
    std::string verb(s.method);
    const boxes::RouteRef* ref = table.match(path->segments, verb).value;
    if (!ref && verb == "get") ref = table.match(path->segments, "subscribe").value;
    if (ref && ref->route && ref->box) {
      s.api = operations::HeatApi::kBox;
      s.box = ref->box->manifest.box;
      s.what = ref->route->pattern.source;
    } else if (s.status >= 400) {
      // What nothing answers, and what the host refused, each together, so a scan can't push
      // the site's pages out (as the live statistics do).
      s.what = s.status == 404 ? "(not found)" : "(refused)";
      s.method = {};
    } else {
      s.what = p.substr(0, 200);
    }
  }
  // The client as the rate limiter counts it (an IPv6 /64 is one); the study hashes it with the
  // log key only the first time it sees it on an endpoint.
  std::string client = rate_key(client_address(req));
  s.client = client;
  heat_->record(domain, s);
}

void Host::heat_tick(int64_t now_ms) {
  if (!heat_ || heat_->running() == 0) return;
  for (const auto& row : heat_->tick(now_ms, config_.get()->disk.studies_bytes)) {
    audit("study.stop", "host", text_of(row, "domain"), "ok",
          bj::object{{"id", row.at("id")}, {"ended_by", "time"}, {"requests", row.at("requests")}});
  }
}

void Host::heat_stop_all() {
  if (!heat_ || heat_->running() == 0) return;
  for (const auto& row : heat_->stop_all(base::now_unix_ms())) {
    audit("study.stop", "host", text_of(row, "domain"), "ok",
          bj::object{{"id", row.at("id")}, {"ended_by", "host_stopping"}, {"requests", row.at("requests")}});
  }
}

void Host::register_heat_study_tools() {
  using mcp::integer_property;
  using mcp::object_schema;
  using mcp::string_property;
  auto id_prop = integer_property("The study's id, from heat_study_start or heat_study_list.");
  // The one reading of an integer argument, the same as the scope rule's (030 § 3): absent,
  // a whole number, or present and not one, which is refused.
  auto int_of = [](const bj::object& args, const char* k, std::string* error) -> std::optional<int64_t> {
    auto a = mcp::integer_arg(args, k);
    if (a.present && !a.ok) *error = std::string(k) + " must be a whole number";
    return a.ok ? std::optional<int64_t>(a.value) : std::nullopt;
  };

  mcp_->add_tool(
      {"heat_study_start",
       "Starts a heat study of one domain: for `minutes` (5 to 1440, default 60), every request on it is counted by "
       "endpoint (a box route's declared pattern like \"GET /posts/{id}\", an MCP tool's name, a page's path): "
       "requests per minute, each client's busiest minute, peak concurrency, latency, heat (time serving), bytes "
       "in and out, and statuses. The host's own paths (/goober/*, CORS preflights) aren't counted. The report "
       "ranks endpoints by heat and suggests controls in observe mode, ready to send with control_set. It changes nothing that is "
       "served, so it never waits for approval; one per domain and 4 on the host at once. Clients are keyed "
       "hashes; no address or user id is kept. Audited. Answers {id, domain, ends_ms, ...}; read it with "
       "heat_study_get.",
       object_schema({{"domain", string_property("The domain to study, like www.example.com (domain_list).")},
                      {"minutes", integer_property("How long, 5 to 1440; 60 when left out. Pick a busy hour.")},
                      {"why", string_property("Why the study runs, for the audit log and the report.")}},
                     {"domain", "why"}),
       [this, int_of](const bj::object& args) {
         if (!heat_) return ToolResult::fail("heat studies are off: see host_health's startup notes");
         std::string domain = base::to_lower(text_of(args, "domain"));
         std::string why = text_of(args, "why");
         if (why.empty()) return ToolResult::fail("why is required: say what the study is for");
         auto cfg = config_.get();
         auto it = cfg->domains.find(domain);
         if (it == cfg->domains.end()) {
           return ToolResult::fail(domain.empty() ? std::string("domain is required")
                                                  : domain + " isn't a domain this host serves (domain_list names them)");
         }
         std::string err;
         int64_t minutes = int_of(args, "minutes", &err).value_or(operations::HeatStudies::kDefaultMinutes);
         if (!err.empty()) return ToolResult::fail(err);
         auto row = heat_->start(domain, it->second.tenant, int(std::clamp<int64_t>(minutes, -1, 100000)), why,
                                 current_operator(), base::now_unix_ms(), &err);
         audit("study.start", current_operator(), domain, row ? "ok" : "refused",
               bj::object{{"id", row ? row->at("id") : bj::value(nullptr)}, {"minutes", minutes}, {"why", why},
                          {"error", row ? "" : err}});
         if (!row) return ToolResult::fail(err);
         (*row)["next"] = "heat_study_get {id} shows progress; the report is kept 30 days after the study ends";
         return ToolResult::ok(std::move(*row));
       }});

  mcp_->add_tool(
      {"heat_study_stop",
       "Ends a running heat study early; what it gathered is kept as its report (heat_study_get). Never waits for "
       "approval: it changes nothing that is served. Audited.",
       object_schema({{"id", id_prop}}, {"id"}),
       [this, int_of](const bj::object& args) {
         if (!heat_) return ToolResult::fail("heat studies are off: see host_health's startup notes");
         std::string err;
         auto id = int_of(args, "id", &err);
         if (!err.empty()) return ToolResult::fail(err);
         if (!id) return ToolResult::fail("id is required");
         auto row = heat_->stop(*id, current_operator(), base::now_unix_ms(), &err);
         audit("study.stop", current_operator(), row ? text_of(*row, "domain") : std::string(), row ? "ok" : "refused",
               bj::object{{"id", *id}, {"ended_by", "operator"}, {"error", row ? "" : err}});
         if (!row) return ToolResult::fail(err);
         return ToolResult::ok(std::move(*row));
       }});

  mcp_->add_tool(
      {"heat_study_get",
       "Read tool. One heat study: while it runs, its progress so far (remaining_ms); once it ends, its report. "
       "Totals, requests per minute, clients (distinct, and the ten busiest by keyed hash), and endpoints ranked "
       "by heat (time serving), each with its share of heat, requests, and bytes, its busiest minute, each "
       "client's busiest minute as p50/p99/max, peak concurrency, latency, statuses, and `suggest`: control_set arguments in mode "
       "\"observe\" with the reason for each number (or `none` with why). Send a suggestion as it is, watch "
       "control_report, and enforce once it holds.",
       object_schema({{"id", id_prop}}, {"id"}),
       [this, int_of](const bj::object& args) {
         if (!heat_) return ToolResult::fail("heat studies are off: see host_health's startup notes");
         std::string err;
         auto id = int_of(args, "id", &err);
         if (!err.empty()) return ToolResult::fail(err);
         if (!id) return ToolResult::fail("id is required");
         auto rep = heat_->get(*id, base::now_unix_ms(), &err);
         if (!rep) return ToolResult::fail(err);
         return ToolResult::ok(std::move(*rep));
       }});

  mcp_->add_tool(
      {"heat_study_list",
       "Read tool. Heat studies, newest first: running ones with remaining_ms, finished ones (kept 30 days) with "
       "their state (done, stopped, interrupted), requests, and hottest endpoint. Only one domain's with `domain`.",
       object_schema({{"domain", string_property("Only this domain's studies.")}}, {}),
       [this](const bj::object& args) {
         if (!heat_) return ToolResult::fail("heat studies are off: see host_health's startup notes");
         return ToolResult::ok(heat_->list(base::to_lower(text_of(args, "domain")), base::now_unix_ms()));
       }});
}

}  // namespace goober::host
