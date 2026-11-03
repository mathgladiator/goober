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

std::string upper(std::string s) {
  for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string text_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

// A host path's endpoint: its first two segments, and the rest as {rest*}, so capability links
// (unsubscribe, list confirmations, invites) and per-item paths are one endpoint, never a token.
std::string fold_host_path(const std::string& path, size_t keep) {
  size_t at = 0;
  for (size_t i = 0; i < keep; ++i) {
    at = path.find('/', at + 1);
    if (at == std::string::npos) return path;
  }
  return path.substr(0, at) + "/{rest*}";
}

}  // namespace

void Host::heat_observe(const Request& req, const bj::object& line, const std::string& domain, int64_t bytes_out) {
  // Not a visitor: a try, the panel, or the host's own calls (timers, outbox results, identity
  // events, internal calls), whose paths don't start with "/".
  if (line.contains("try") || line.contains("panel") || !req.path.starts_with("/")) return;
  std::string raw = req.path.substr(0, req.path.find('?'));
  auto path = router::normalize_path(raw);
  std::string p = path ? path->path : "";
  if (p == kOperationsMcpPath || p == kMailHookPath) return;
  int status = 0;
  if (const auto* st = line.if_contains("status"); st && st->is_int64()) status = int(st->as_int64());
  operations::HeatSample s;
  s.at_ms = base::now_unix_ms();
  s.end_us = base::monotonic_us();
  if (const auto* us = line.if_contains("us"); us && us->is_int64()) s.us = us->as_int64();
  if (const auto* f = line.if_contains("fuel"); f && f->is_int64()) s.fuel = f->as_int64();
  s.status = status;
  s.bytes_in = int64_t(req.body.size());
  s.bytes_out = std::max<int64_t>(0, bytes_out);
  std::string method = upper(req.method == "head" ? "get" : req.method);
  s.method = method;
  bool bearer = base::to_lower(req.header("authorization")).starts_with("bearer ");
  if (!path) {
    s.api = "content";
    s.endpoint = "(refused)";
  } else if (p == "/mcp") {
    s.api = "mcp";
    std::string tool = text_of(line, "tool");
    s.endpoint = tool.empty() ? "(rpc)" : tool;
    s.method.clear();
  } else if (p.starts_with("/identity/") || p == "/oauth" || p.starts_with("/oauth/") || p.starts_with("/.well-known/")) {
    s.api = "identity";
    s.endpoint = method + " " + (p.starts_with("/identity/invite/") ? std::string("/identity/invite/{rest*}") : p);
  } else if (p == "/goober" || p.starts_with("/goober/")) {
    s.api = "host";
    s.endpoint = method + " " + fold_host_path(p, 2);
  } else if (req.method == "options") {
    s.api = "host";
    s.endpoint = "(preflight)";
  } else if (p == "/attachment" || p.starts_with("/attachment/")) {
    s.api = "content";
    s.endpoint = method + " /attachment/{file}";
  } else {
    // A box route, by the same match the dispatcher made: its declared pattern, whoever answered
    // (the box, or the host refusing before it ran).
    auto active = boxes_->active();
    const auto& table = active->table_for(domain);
    std::string verb = req.method == "head" ? "get" : req.method;
    const boxes::RouteRef* ref = table.match(path->segments, verb).value;
    if (!ref && verb == "get") ref = table.match(path->segments, "subscribe").value;
    if (ref && ref->route && ref->box) {
      s.api = "box:" + ref->box->manifest.box;
      s.endpoint = operations::endpoint_match(method, ref->route->pattern.source);
    } else {
      // Content, by its path; what nothing answers, and what the host refused, each together, so
      // a scan can't push the site's pages out (as the live statistics do).
      s.api = "content";
      s.endpoint = status == 404 ? "(not found)" : status >= 400 ? "(refused)" : method + " " + p.substr(0, 200);
    }
  }
  // The client as the rate limiter counts it (an IPv6 /64 is one), as a keyed hash: the same
  // hash the audit log gives an IPv4 address (client_h).
  std::string h = address_hash_for_logs(rate_key(client_address(req)));
  s.client = std::strtoull(h.c_str(), nullptr, 16);
  s.cls = operations::classify_client(req.header("user-agent"), bearer || p == "/mcp");
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
  auto int_of = [](const bj::object& args, const char* k) -> std::optional<int64_t> {
    const auto* v = args.if_contains(k);
    if (!v) return std::nullopt;
    if (v->is_int64()) return v->as_int64();
    if (v->is_uint64()) return int64_t(v->as_uint64());
    if (v->is_double()) return int64_t(v->as_double());
    return std::nullopt;
  };

  mcp_->add_tool(
      {"heat_study_start",
       "Starts a heat study of one domain: for `minutes` (5 to 1440, default 60), every request on it is counted by "
       "endpoint (a box route's declared pattern like \"GET /posts/{id}\", an MCP tool's name, a page's path): "
       "requests per minute, each client's busiest minute, peak concurrency, latency, heat (time serving, and "
       "fuel), bytes in and out, statuses, and client classes. The report ranks endpoints by heat, requests, and "
       "bytes, and suggests controls in observe mode, ready to send with control_set. It changes nothing that is "
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
         int64_t minutes = int_of(args, "minutes").value_or(operations::HeatStudies::kDefaultMinutes);
         std::string err;
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
         auto id = int_of(args, "id");
         if (!id) return ToolResult::fail("id is required");
         std::string err;
         auto row = heat_->stop(*id, current_operator(), base::now_unix_ms(), &err);
         audit("study.stop", current_operator(), row ? text_of(*row, "domain") : std::string(), row ? "ok" : "refused",
               bj::object{{"id", *id}, {"ended_by", "operator"}, {"error", row ? "" : err}});
         if (!row) return ToolResult::fail(err);
         return ToolResult::ok(std::move(*row));
       }});

  mcp_->add_tool(
      {"heat_study_get",
       "Read tool. One heat study: while it runs, its progress so far (remaining_ms); once it ends, its report. "
       "Totals, requests per minute, clients (distinct, each one's busiest minute as p50/p99/max), peak "
       "concurrency, the busiest clients by keyed hash, and endpoints ranked by heat (time serving), each with "
       "requests, latency, bytes, statuses, client classes, and `suggest`: control_set arguments in mode "
       "\"observe\" with the reason for each number (or `none` with why). Send a suggestion as it is, watch "
       "control_report, and enforce once it holds.",
       object_schema({{"id", id_prop}}, {"id"}),
       [this, int_of](const bj::object& args) {
         if (!heat_) return ToolResult::fail("heat studies are off: see host_health's startup notes");
         auto id = int_of(args, "id");
         if (!id) return ToolResult::fail("id is required");
         std::string err;
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
