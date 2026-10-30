// Logs, live performance, and auditing agents (design/machine/032.Logs.md): every log line
// through log_line (addresses hashed, the envelope and chain written by the logs package), the
// audit log, hourly housekeeping, and the tools: log_search, log_verify, log_status, uri_stats,
// uri_tree, uri_flow, and agent_audit.

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/logs/logs.h"
#include "goober/site/site.h"
#include "backups.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;

namespace {

constexpr int64_t kDay = 24LL * 3600 * 1000;
constexpr int64_t kLocalKeepMs = 7 * kDay;
constexpr int64_t kQueueKeepMs = 30 * kDay;

std::string field(const bj::object& o, const char* k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

int64_t int_field(const bj::object& o, const char* k, int64_t fallback = 0) {
  auto* v = o.if_contains(k);
  if (v && v->is_int64()) return v->as_int64();
  if (v && v->is_uint64()) return int64_t(v->as_uint64());
  return fallback;
}

bool is_delete(const std::string& tool) {
  return tool.find("delete") != std::string::npos || tool.find("remove") != std::string::npos ||
         tool.find("revoke") != std::string::npos || tool.find("discard") != std::string::npos;
}

int64_t window_ms(const std::string& w, int64_t fallback) {
  if (w == "5m") return 5 * 60000;
  if (w == "15m") return 15 * 60000;
  if (w == "1h") return 3600000;
  if (w == "24h") return kDay;
  if (w == "3d") return 3 * kDay;
  if (w == "7d") return 7 * kDay;
  return fallback;
}

}  // namespace

std::string Host::address_hash_for_logs(const std::string& address) {
  return base::hmac_sha256_hex(reply_key(), "goober-log-address\n" + base::to_lower(address)).substr(0, 16);
}

void Host::log_line(const std::string& log, bj::object line) {
  if (!logs_) return;
  // The envelope's own time is the write's; an event's time stays as its own field.
  // An event's own time is kept only when it differs from the write's (a box's call time, say).
  int64_t now = base::now_unix_ms();
  if (auto* at = line.if_contains("at"); at && at->is_string()) {
    auto t = logs::parse_at(std::string(at->as_string()));
    if (!t || std::llabs(*t - now) >= 1000) line["event_at"] = *at;
    line.erase("at");
  }
  // Addresses become keyed hashes, with the domain kept (032 § 1).
  for (const char* k : {"email", "to", "from", "sender", "recipient", "envelope_from", "rcpt"}) {
    auto* v = line.if_contains(k);
    if (!v || !v->is_string()) continue;
    std::string s(v->as_string());
    auto atsign = s.rfind('@');
    if (atsign == std::string::npos) continue;
    std::string addr = s;
    if (auto lt = s.find('<'); lt != std::string::npos) addr = s.substr(lt + 1, s.find('>', lt) - lt - 1);
    line[std::string(k) + "_h"] = address_hash_for_logs(addr);
    line[std::string(k) + "_domain"] = base::to_lower(addr.substr(addr.rfind('@') + 1));
    line.erase(k);
  }
  try {
    logs_->write(log, line, now);
  } catch (const std::exception& e) {
    std::cerr << "goober-host: log " << log << ": " << e.what() << "\n";
  }
}

void Host::audit(const std::string& action, const std::string& actor, const std::string& target, const std::string& outcome,
                 bj::object more) {
  bj::object line{{"action", action}, {"actor", actor}, {"target", target}, {"outcome", outcome}};
  for (auto& kv : more) {
    if (!line.contains(kv.key())) line[kv.key()] = std::move(kv.value());
  }
  log_line("audit", std::move(line));
}

void Host::logs_housekeeping(int64_t now_ms) {
  if (!logs_) return;
  logs_->rotate_stale(now_ms);
  auto cfg = config_.get();
  std::error_code ec;
  fs::path dir = cfg->storage / "logs";
  // Numbered files past 7 days go; their archive links (if not yet uploaded) keep the bytes.
  for (const auto& name : logs_->names()) {
    for (const auto& f : logs::files_of(dir, name)) {
      if (f.extension() == ".log") continue;  // the current file
      auto when = fs::last_write_time(f, ec);
      if (ec) continue;
      auto age = std::chrono::duration_cast<std::chrono::milliseconds>(fs::file_time_type::clock::now() - when).count();
      if (age > kLocalKeepMs) fs::remove(f, ec);
    }
  }
  // The archive queue: without backups nothing goes up, so it keeps 7 days; with them, a file
  // that can't go up is kept 30 days, then dropped with an error.
  fs::path queue = dir / "archive";
  int64_t keep = cfg->backup.enabled() && !cfg->sandbox ? kQueueKeepMs : kLocalKeepMs;
  for (auto it = fs::directory_iterator(queue, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    auto when = fs::last_write_time(it->path(), ec);
    if (ec) continue;
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(fs::file_time_type::clock::now() - when).count();
    if (age > keep) {
      if (keep == kQueueKeepMs) std::cerr << "goober-host: logs: " << it->path().filename() << " was never archived; dropped\n";
      fs::remove(it->path(), ec);
    }
  }
}

void Host::register_log_tools() {
  using mcp::boolean_property;
  using mcp::integer_property;
  using mcp::object_schema;
  using mcp::string_property;
  auto known_tenant = [this](const std::string& t) {
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == t) return true;
    }
    return false;
  };

  mcp_->add_tool(
      {"log_search",
       "The log dive, like grep over the last days (032 § 5): newest first, in one log (access, egress, mail, "
       "agents, db, audit, boxes/$box) or all. pattern is a substring and regex an ECMAScript regex over the raw "
       "line; fields must equal the line's own ({\"status\": \"500\", \"tenant\": \"shop\"}); email finds lines "
       "naming that address (logs keep addresses only as hashes). since and until are times like "
       "2026-10-29T12:00:00Z or ago like 3h, 2d (default: the last 24 hours; local files reach 7 days). Up to "
       "limit lines (200, at most 1000); pass next back as before for more. Every search is audited.",
       object_schema({{"log", string_property("A log name, or all (the default).")},
                      {"pattern", string_property("A substring of the line.")},
                      {"regex", string_property("An ECMAScript regex over the line.")},
                      {"fields", mcp::object_property("Top-level fields that must equal, as text.")},
                      {"email", string_property("An address: lines naming it, by its hash.")},
                      {"since", string_property("A time, or ago like 3h or 2d. Default 24h.")},
                      {"until", string_property("A time, or ago. Default now.")},
                      {"limit", integer_property("1 to 1000, default 200.")},
                      {"before", string_property("The next cursor of an earlier answer.")}},
                     {}),
       [this](const bj::object& args) {
         int64_t now = base::now_unix_ms();
         auto time_of = [&](const std::string& s, int64_t fallback) -> std::optional<int64_t> {
           if (s.empty()) return fallback;
           if (auto at = logs::parse_at(s.size() == 20 ? s.substr(0, 19) + ".000Z" : s)) return at;
           if (s.size() >= 2) {
             char unit = s.back();
             int64_t n = std::atoll(s.substr(0, s.size() - 1).c_str());
             int64_t per = unit == 'm' ? 60000 : unit == 'h' ? 3600000 : unit == 'd' ? kDay : 0;
             if (n > 0 && per) return now - n * per;
           }
           return std::nullopt;
         };
         logs::Query q;
         auto since = time_of(field(args, "since"), now - kDay), until = time_of(field(args, "until"), now);
         if (!since || !until) return ToolResult::fail("since and until are times like 2026-10-29T12:00:00Z, or ago like 3h");
         q.since_ms = *since;
         q.until_ms = *until;
         q.pattern = field(args, "pattern");
         // A search ends within 10 seconds, whatever it was asked; a regex that can backtrack for
         // ever is refused before it runs.
         q.deadline_us = base::monotonic_us() + 10'000'000;
         if (auto rx = field(args, "regex"); !rx.empty()) {
           std::string why;
           if (!logs::safe_regex(rx, &why)) return ToolResult::fail("regex: " + why);
           try {
             q.regex = std::regex(rx, std::regex::ECMAScript);
           } catch (const std::exception&) {
             return ToolResult::fail("regex doesn't parse");
           }
         }
         if (auto* f = args.if_contains("fields"); f && f->is_object()) {
           for (const auto& kv : f->as_object()) {
             q.fields[std::string(kv.key())] = kv.value().is_string() ? std::string(kv.value().as_string()) : json::dump(kv.value());
           }
         }
         if (auto email = field(args, "email"); !email.empty()) q.hashes.push_back(address_hash_for_logs(email));
         q.limit = size_t(std::clamp<int64_t>(int_field(args, "limit", 200), 1, 1000));
         if (auto before = field(args, "before"); !before.empty()) {
           // log:seq:at_ms
           auto a = before.find(':'), b = before.rfind(':');
           if (a == std::string::npos || a == b) return ToolResult::fail("before is the next value of an earlier answer");
           q.before_log = before.substr(0, a);
           q.before_seq = std::atoll(before.substr(a + 1, b - a - 1).c_str());
           q.before_at_ms = std::atoll(before.substr(b + 1).c_str());
         }
         std::string log = field(args, "log");
         std::vector<std::string> names;
         if (log.empty() || log == "all") {
           names = logs_->names();
         } else {
           if (!logs::valid_name(log)) return ToolResult::fail("log is access, egress, mail, agents, db, audit, boxes/$box, or all");
           names = {log};
         }
         // A focused operator (030) sees their tenants' lines, and their own audit lines.
         std::string me = current_operator();
         auto scope = operator_scope(me);
         if (!scope.all) {
           q.allow = [scope, me](const std::string& log_name, const bj::object& o) {
             std::string t = field(o, "tenant");
             if (!t.empty()) return scope.tenants.count(t) > 0;
             return log_name == "audit" && field(o, "actor") == me;
           };
         }
         auto found = logs::search(logs_->dir(), names, q);
         bj::array lines;
         for (const auto& h : found.hits) lines.push_back(bj::value(h.line));
         bj::object out{{"lines", std::move(lines)}, {"count", int64_t(found.hits.size())},
                        {"bytes_read", int64_t(found.bytes_read)}, {"truncated", found.truncated}};
         if (found.truncated && !found.hits.empty()) {
           const auto& last = found.hits.back();
           out["next"] = last.log + ":" + std::to_string(last.seq) + ":" + std::to_string(last.at_ms);
         }
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool({"log_verify",
                  "Walks a log's local files and recomputes every line's hash chain (032 § 3): a deleted, inserted, or "
                  "edited line breaks it, and the answer says where.",
                  object_schema({{"log", string_property("A log name; default every log.")}}, {}),
                  [this](const bj::object& args) {
                    std::string log = field(args, "log");
                    std::vector<std::string> names = log.empty() ? logs_->names() : std::vector<std::string>{log};
                    bj::array out;
                    bool ok = true;
                    for (const auto& n : names) {
                      if (!logs::valid_name(n)) return ToolResult::fail("not a log: " + n);
                      auto v = logs::verify(logs_->dir(), n);
                      ok = ok && v.ok;
                      bj::object o{{"log", n}, {"ok", v.ok}, {"lines", v.lines}, {"files", v.files}, {"unchained", v.unchained}};
                      if (!v.first_seq.empty()) o["seqs"] = v.first_seq + ".." + v.last_seq;
                      if (!v.ok) o["broken"] = v.broken;
                      out.push_back(std::move(o));
                    }
                    return ToolResult::ok(bj::object{{"ok", ok}, {"logs", std::move(out)}});
                  }});

  mcp_->add_tool({"log_status",
                  "The logs: each one's current size and files on the machine, the archive queue waiting for upload, "
                  "and the last upload or error (032 § 2).",
                  object_schema({}, {}), [this](const bj::object&) {
                    std::error_code ec;
                    bj::array logs;
                    for (const auto& n : logs_->names()) {
                      auto files = logs::files_of(logs_->dir(), n);
                      int64_t bytes = 0;
                      for (const auto& f : files) bytes += int64_t(fs::file_size(f, ec));
                      logs.push_back(bj::object{{"log", n}, {"files", int64_t(files.size())}, {"bytes", bytes}});
                    }
                    bj::array queue;
                    for (auto it = fs::directory_iterator(logs_->dir() / "archive", ec); !ec && it != fs::directory_iterator();
                         it.increment(ec)) {
                      if (it->path().extension() == ".log") queue.push_back(bj::value(it->path().filename().string()));
                    }
                    auto cfg = config_.get();
                    bj::object archive{{"enabled", cfg->backup.enabled() && !cfg->sandbox}, {"queued", std::move(queue)}};
                    if (backups_) {
                      auto st = backups_->status(false, base::now_unix_ms());
                      for (const char* k : {"last_logs_at", "last_logs_error"}) {
                        if (auto* v = st.if_contains(k)) archive[k] = *v;
                      }
                    }
                    return ToolResult::ok(bj::object{{"logs", std::move(logs)}, {"archive", std::move(archive)},
                                                     {"local_days", 7}, {"archive_days", 400}});
                  }});

  mcp_->add_tool(
      {"uri_stats",
       "Live performance per path, kept in memory as requests happen (032 § 6): count, 4xx, 5xx, p50, p95, p99, and "
       "max over the last 5m, 15m, or 1h, for a tenant or all. Sort by p99 (default), count, or errors.",
       object_schema({{"tenant", string_property("A tenant; default all.")},
                      {"window", mcp::enum_property("5m, 15m, or 1h (default).", {"5m", "15m", "1h"})},
                      {"prefix", string_property("Only paths starting with this, like /places.")},
                      {"sort", mcp::enum_property("p99, count, or errors.", {"p99", "count", "errors"})},
                      {"limit", integer_property("1 to 1000, default 100.")}},
                     {}),
       [this](const bj::object& args) {
         int minutes = int(window_ms(field(args, "window"), 3600000) / 60000);
         auto rows = live_.stats(base::now_unix_ms(), minutes, field(args, "tenant"), field(args, "prefix"));
         std::string sort = field(args, "sort");
         std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
           if (sort == "count") return a.n > b.n;
           if (sort == "errors") return a.n5 != b.n5 ? a.n5 > b.n5 : a.n4 > b.n4;
           return a.p99_us > b.p99_us;
         });
         size_t limit = size_t(std::clamp<int64_t>(int_field(args, "limit", 100), 1, 1000));
         bj::array out;
         for (size_t i = 0; i < rows.size() && i < limit; ++i) {
           const auto& r = rows[i];
           bj::object o{{"tenant", r.tenant}, {"path", r.path}, {"n", r.n}, {"n4", r.n4}, {"n5", r.n5},
                        {"p50_us", r.p50_us}, {"p95_us", r.p95_us}, {"p99_us", r.p99_us}, {"max_us", r.max_us}};
           if (!r.route.empty()) o["route"] = r.route;
           if (!r.box.empty()) o["box"] = r.box;
           out.push_back(std::move(o));
         }
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool(
      {"uri_tree",
       "A tenant's whole site as a tree of path segments (032 § 6), for the panel's sitemap: every listed page, "
       "static file, box route, and path seen in the window, each node with its kind, count, 5xx, and p99. A page "
       "nobody visits still shows.",
       object_schema({{"tenant", string_property("A tenant name from the host config.")},
                      {"window", mcp::enum_property("5m, 15m, or 1h (default).", {"5m", "15m", "1h"})}},
                     {"tenant"}),
       [this, known_tenant](const bj::object& args) {
         std::string tenant = field(args, "tenant");
         if (!known_tenant(tenant)) return ToolResult::fail("no domain in the host config serves tenant " + tenant);
         int minutes = int(window_ms(field(args, "window"), 3600000) / 60000);
         struct Node {
           std::string kind;
           int64_t n = 0, n5 = 0, p99 = 0;
           std::map<std::string, Node> kids;
         };
         Node root;
         root.kind = "dir";
         auto at = [&](const std::string& path) -> Node& {
           Node* cur = &root;
           size_t i = 1;
           while (i <= path.size()) {
             size_t j = path.find('/', i);
             if (j == std::string::npos) j = path.size();
             std::string seg = path.substr(i, j - i);
             if (!seg.empty()) {
               auto& next = cur->kids[seg];
               if (next.kind.empty()) next.kind = "dir";
               cur = &next;
             }
             i = j + 1;
           }
           return *cur;
         };
         for (const auto& p : site_->listed_pages(tenant)) at(p.url).kind = "page";
         int files = 0;
         for (const auto& f : content_->list(tenant, "")) {
           if (site::is_private_path(f) || site::page_url(f) || ++files > 5000) continue;
           at("/" + f).kind = "file";
         }
         auto cfg = config_.get();
         for (const auto& [name, box] : boxes_->active()->boxes) {
           bool serves = false;
           for (const auto& [t, d] : tenants_of(*cfg, box->manifest)) serves |= t == tenant;
           if (!serves) continue;
           for (const auto& r : box->manifest.routes) {
             auto& node = at(r.pattern.source);
             if (node.kind == "dir") node.kind = "box:" + name;
           }
         }
         for (const auto& r : live_.stats(base::now_unix_ms(), minutes, tenant, "")) {
           auto& node = at(r.path);
           if (node.kind == "dir") node.kind = r.path.starts_with("/goober") || r.path.starts_with("/identity") ? "host" : r.box.empty() ? "unknown" : "box:" + r.box;
           node.n += r.n;
           node.n5 += r.n5;
           node.p99 = std::max(node.p99, r.p99_us);
         }
         // Counts roll up to their parents; a parent's p99 is its slowest child's.
         std::function<bj::object(const std::string&, const std::string&, Node&)> emit = [&](const std::string& name,
                                                                                             const std::string& path, Node& node) {
           bj::array kids;
           int64_t n = node.n, n5 = node.n5, p99 = node.p99;
           for (auto& [seg, kid] : node.kids) {
             auto k = emit(seg, path == "/" ? "/" + seg : path + "/" + seg, kid);
             n += k.at("total").as_int64();
             n5 += k.at("total_5xx").as_int64();
             p99 = std::max(p99, k.at("p99_us").as_int64());
             kids.push_back(std::move(k));
           }
           bj::object o{{"name", name}, {"path", path}, {"kind", node.kind}, {"n", node.n}, {"total", n}, {"total_5xx", n5},
                        {"p99_us", p99}};
           if (!kids.empty()) o["children"] = std::move(kids);
           return o;
         };
         return ToolResult::ok(emit("/", "/", root));
       }});

  mcp_->add_tool({"uri_flow",
                  "Where visitors to a path came from and went next, within the site, from same-site Referers over the "
                  "window (032 § 6).",
                  object_schema({{"tenant", string_property("A tenant name from the host config.")},
                                 {"path", string_property("A path, like /updates; empty for every flow.")},
                                 {"window", mcp::enum_property("5m, 15m, or 1h (default).", {"5m", "15m", "1h"})}},
                                {"tenant"}),
                  [this, known_tenant](const bj::object& args) {
                    std::string tenant = field(args, "tenant"), path = field(args, "path");
                    if (!known_tenant(tenant)) return ToolResult::fail("no domain in the host config serves tenant " + tenant);
                    int minutes = int(window_ms(field(args, "window"), 3600000) / 60000);
                    bj::array in, out;
                    for (const auto& f : live_.flows(base::now_unix_ms(), minutes, tenant, path)) {
                      bj::object o{{"from", f.from}, {"to", f.to}, {"n", f.n}};
                      if (!path.empty() && f.to == path) in.push_back(std::move(o));
                      else out.push_back(std::move(o));
                      if (in.size() + out.size() >= 200) break;
                    }
                    return ToolResult::ok(bj::object{{"path", path}, {"in", std::move(in)}, {"out", std::move(out)}});
                  }});

  mcp_->add_tool(
      {"agent_audit",
       "Are the agents behaving? Per operator, token, and site agent over the window (1h, 24h, 7d): calls, writes, "
       "failures, deletes, distinct targets, the busiest 5 minutes, calls per bucket for a sparkline, and flags with "
       "their evidence: burst (more than 60 calls in 5 minutes or 30 writes in 10), failing (more than a quarter of "
       "20 or more fail), deleting (more than 10 in an hour), looping (the same call more than 10 times in 10 "
       "minutes).",
       object_schema({{"window", mcp::enum_property("1h, 24h (default), or 7d.", {"1h", "24h", "7d"})}}, {}),
       [this](const bj::object& args) {
         int64_t now = base::now_unix_ms();
         int64_t span = window_ms(field(args, "window"), kDay);
         int64_t bucket = span <= 3600000 ? 300000 : span <= kDay ? 3600000 : 6 * 3600000;
         int buckets = int(span / bucket);
         logs::Query q;
         q.since_ms = now - span;
         q.until_ms = now;
         q.limit = 100000;
         q.max_read = 512ull << 20;
         auto found = logs::search(logs_->dir(), {"audit", "agents"}, q);
         struct Call {
           int64_t at;
           std::string tool, key;
           bool write, failed, del;
         };
         std::map<std::string, std::vector<Call>> by;
         for (const auto& h : found.hits) {
           std::string err;
           auto v = json::parse(h.line, &err);
           if (!v || !v->is_object()) continue;
           const auto& o = v->as_object();
           if (h.log == "audit") {
             if (field(o, "action") != "operator.tool") continue;
             std::string tool = field(o, "target");
             by[field(o, "actor")].push_back(Call{h.at_ms, tool, tool + " " + json::dump(o.contains("args") ? o.at("args") : bj::value()),
                                                  !is_read_tool(tool), field(o, "outcome") != "ok", is_delete(tool)});
           } else {
             std::string tool = field(o, "tool");
             int64_t st = int_field(o, "status", 200);
             std::string who = "site:" + field(o, "tenant") + ":" + field(o, "client_id") + ":user " + field(o, "user_id");
             by[who].push_back(Call{h.at_ms, tool, tool + " " + field(o, "request_digest"), true, st >= 400, is_delete(tool)});
           }
         }
         bj::array out;
         for (auto& [who, calls] : by) {
           std::sort(calls.begin(), calls.end(), [](const Call& a, const Call& b) { return a.at < b.at; });
           int64_t writes = 0, failed = 0, deletes = 0;
           std::set<std::string> targets;
           std::vector<int64_t> spark(size_t(std::max(buckets, 1)), 0);
           for (const auto& c : calls) {
             writes += c.write;
             failed += c.failed;
             deletes += c.del;
             targets.insert(c.key);
             int64_t b = (c.at - (now - span)) / bucket;
             if (b >= 0 && b < int64_t(spark.size())) ++spark[size_t(b)];
           }
           // The busiest windows, by a sliding count.
           auto most_in = [&](int64_t ms, auto pred) {
             int64_t best = 0;
             size_t lo = 0, count = 0;
             std::vector<const Call*> in;
             for (size_t hi = 0; hi < calls.size(); ++hi) {
               if (pred(calls[hi])) in.push_back(&calls[hi]);
               while (!in.empty() && in.front()->at < calls[hi].at - ms) in.erase(in.begin());
               best = std::max<int64_t>(best, int64_t(in.size()));
             }
             (void)lo;
             (void)count;
             return best;
           };
           int64_t peak5 = most_in(300000, [](const Call&) { return true; });
           int64_t writes10 = most_in(600000, [](const Call& c) { return c.write; });
           int64_t deletes60 = most_in(3600000, [](const Call& c) { return c.del; });
           int64_t loop = 0;
           std::string loop_key;
           {
             std::map<std::string, std::vector<int64_t>> same;
             for (const auto& c : calls) same[c.key].push_back(c.at);
             for (const auto& [k, ts] : same) {
               size_t lo = 0;
               for (size_t hi = 0; hi < ts.size(); ++hi) {
                 while (ts[lo] < ts[hi] - 600000) ++lo;
                 if (int64_t(hi - lo + 1) > loop) {
                   loop = int64_t(hi - lo + 1);
                   loop_key = k;
                 }
               }
             }
           }
           bj::array flags;
           if (peak5 > 60 || writes10 > 30) {
             flags.push_back(bj::object{{"flag", "burst"}, {"calls_in_5m", peak5}, {"writes_in_10m", writes10}});
           }
           if (calls.size() >= 20 && failed * 4 > int64_t(calls.size())) {
             flags.push_back(bj::object{{"flag", "failing"}, {"failed", failed}, {"of", int64_t(calls.size())}});
           }
           if (deletes60 > 10) flags.push_back(bj::object{{"flag", "deleting"}, {"deletes_in_1h", deletes60}});
           if (loop > 10) flags.push_back(bj::object{{"flag", "looping"}, {"times_in_10m", loop}, {"call", loop_key.substr(0, 200)}});
           bj::array sp;
           for (auto x : spark) sp.push_back(x);
           out.push_back(bj::object{{"actor", who},          {"calls", int64_t(calls.size())}, {"writes", writes},
                                    {"failed", failed},      {"deletes", deletes},             {"targets", int64_t(targets.size())},
                                    {"peak_5m", peak5},      {"spark", std::move(sp)},         {"bucket_ms", bucket},
                                    {"flags", std::move(flags)}, {"last", base::format_utc_ms(calls.back().at)}});
         }
         std::sort(out.begin(), out.end(), [](const bj::value& a, const bj::value& b) {
           auto fa = a.at("flags").as_array().size(), fb = b.at("flags").as_array().size();
           return fa != fb ? fa > fb : a.at("calls").as_int64() > b.at("calls").as_int64();
         });
         return ToolResult::ok(bj::object{{"window_ms", span}, {"actors", std::move(out)}, {"truncated", found.truncated}});
       }});
}

}  // namespace goober::host
