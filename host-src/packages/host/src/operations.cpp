// The operator MCP endpoint (/goober-operations-mcp) and its tools
// (design/machine/005.FirstVersion.md §9).

#include <iostream>
#include <algorithm>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/files.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "build_id.h"
#include "jobs.h"
#include "goober/host/host.h"
#include "goober/site/includes.h"
#include "goober/host/operator_tokens.h"
#include "goober/json/json.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;

namespace {

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

int64_t int_arg(const bj::object& args, const char* key, int64_t fallback) {
  // A number, or a string of digits: agents told "ids are strings in JSON" send them either way.
  const auto* v = args.if_contains(key);
  if (!v) return fallback;
  if (v->is_int64()) return v->as_int64();
  if (v->is_uint64()) return int64_t(v->as_uint64());
  if (v->is_string()) {
    std::string_view t = v->as_string();
    bool neg = !t.empty() && t.front() == '-';
    if (neg) t.remove_prefix(1);
    if (t.empty() || t.size() > 18 || t.find_first_not_of("0123456789") != std::string_view::npos) return fallback;
    int64_t n = 0;
    for (char c : t) n = n * 10 + (c - '0');
    return neg ? -n : n;
  }
  return fallback;
}

bool looks_like_text(std::string_view data) {
  for (unsigned char c : data) {
    if (c == 0 || (c < 0x09) || (c > 0x0d && c < 0x20)) return false;
  }
  return true;
}

bj::array parse_lines(const std::vector<std::string>& lines) {
  bj::array out;
  for (const auto& line : lines) {
    std::string err;
    auto v = json::parse(line, &err);
    out.push_back(v ? *v : bj::value(line));
  }
  return out;
}

Response json_response(int status, std::string body) {
  Response res;
  res.status = status;
  if (!body.empty()) res.headers.emplace_back("content-type", "application/json");
  res.headers.emplace_back("cache-control", "no-store");
  res.body = std::move(body);
  return res;
}

}  // namespace

std::string& Host::current_operator() {
  thread_local std::string name;
  return name;
}

std::string Host::bearer_operator(const Request& req) const {
  auto cfg = config_.get();
  std::string auth = req.header("authorization");
  if (!cfg->operations_token.empty() && constant_time_equal(auth, "Bearer " + cfg->operations_token)) return "config";
  if (auth.starts_with("Bearer ")) {
    if (auto t = find_operator_token(cfg->storage, std::string_view(auth).substr(7))) return "token:" + t->name;
    // An OAuth access token from the operator issuer (design/machine/023.OperatorOAuth.md), held
    // by an address still in identity.operators: taking it out of the config ends its access.
    if (auto b = identity_->bearer(identity::kHostRealm, std::string_view(auth).substr(7), base::now_unix_ms())) {
      auto it = cfg->identity.operators.find(b->user.email);
      if (it != cfg->identity.operators.end() && !it->second.empty()) return "oauth:" + b->user.email + ":" + b->client_id;
    }
  }
  return "";
}

void Host::handle_operations(const Request& req, const Finish& finish) {
  std::string operator_name = bearer_operator(req);
  if (operator_name.empty()) {
    // Wrong tokens are slowed per address; right ones aren't counted at all.
    if (int64_t wait = limiter_.take("operator-auth", rate_key(client_address(req)), RateRule{10, 5}, base::now_unix_ms())) {
      auto res = json_response(429, R"({"error":"too many requests without a valid token"})");
      res.headers.emplace_back("retry-after", std::to_string(wait));
      return finish(std::move(res), R"({"why":"rate limited","bucket":"operator-auth"})");
    }
    if (!req.header("authorization").empty()) {
      audit("operator.auth_failed", "", "goober-operations-mcp", "refused",
            bj::object{{"ip", client_address(req)}, {"request_id", req.header("goober-request-id")}});
    }
    auto res = json_response(
        401, R"({"error":"a bearer token is required: sign in with OAuth as a host operator, or mint one with goober-host token create"})");
    // Where an MCP client learns to sign in (RFC 9728): the operator issuer.
    bool sent = base::to_lower(req.header("authorization")).starts_with("bearer ");
    res.headers.emplace_back("www-authenticate", std::string("Bearer") + (sent ? " error=\"invalid_token\"," : "") +
                                                     " resource_metadata=\"" + origin_of(req) +
                                                     "/.well-known/oauth-protected-resource" +
                                                     std::string(kOperationsMcpPath) + "\"");
    return finish(std::move(res), R"({"why":"operations token"})");
  }
  if (req.method != "post") {
    auto res = json_response(405, "");
    res.headers.emplace_back("allow", "POST");
    return finish(std::move(res), "");
  }
  OperatorContext as(*this, operator_name, req.header("goober-request-id"));
  saw_agent(operator_name);  // the panel lists connected agents, to turn approvals on for one
  // A host:observer only looks (design/machine/028.Panel.md § 2).
  // A focused operator only reaches their tenants (design/machine/030.OperatorScope.md).
  auto policy = operator_policy(operator_name, !may_change(operator_roles(operator_name)));
  // Every call, one at a time even in a batch: the audit log (032 § 4) and the change log's
  // actions (028 § 5), by whom, from where, on what, and how it went.
  bj::object extra{{"operations", "mcp"}, {"operator", operator_name}};
  std::string request_id = req.header("goober-request-id"), ip = client_address(req);
  int calls = 0;
  policy.after = [&](const std::string& tool, const bj::object& args, const mcp::ToolResult& out, bool held) {
    ++calls;
    bj::object named;
    for (const char* k : {"tenant", "path", "project", "box", "name"}) {
      if (const auto* v = args.if_contains(k); v && v->is_string() && v->as_string().size() <= 200) named[k] = v->as_string();
    }
    std::string outcome = held ? "held" : out.is_error ? "failed" : "ok";
    audit("operator.tool", operator_name, tool, outcome,
          bj::object{{"ip", ip}, {"request_id", request_id}, {"via", "mcp"}, {"args", named}});
    if (changes_) {
      try {
        changes_->record_action(operations::Action{base::now_unix_ms(), operator_name, tool, request_id, named, out.is_error});
      } catch (const std::exception& e) {
        std::cerr << "goober-host: change log: " << e.what() << "\n";
      }
    }
    // The access log names the call too: the first one's tool and arguments, and how many.
    if (calls == 1) {
      extra["tool"] = tool;
      for (const auto& kv : named) extra[std::string("arg_") + std::string(kv.key())] = kv.value();
      if (out.is_error) extra["tool_error"] = true;
      if (held) extra["held"] = true;
    }
  };
  // A write answers with the change ids it made (028 § 5), so a reviewer can pin them without
  // looking them up: the change log's rows with this request's id, after the call began.
  if (changes_) {
    auto prepare = policy.prepare;
    auto filter = policy.filter;
    auto before = std::make_shared<int64_t>(0);
    policy.prepare = [this, prepare, before](const mcp::Tool& tool, bj::object& args) {
      if (prepare) prepare(tool, args);
      try {
        *before = changes_->last_id();
      } catch (const std::exception&) {
        *before = -1;
      }
    };
    policy.filter = [this, filter, before, request_id](const std::string& tool, bj::value& result) -> std::optional<std::string> {
      if (filter) {
        if (auto refused = filter(tool, result)) return refused;
      }
      if (*before < 0 || !result.is_object()) return std::nullopt;
      try {
        auto ids = changes_->ids_since(request_id, *before);
        if (!ids.empty()) {
          bj::array a;
          for (int64_t id : ids) a.push_back(id);
          if (ids.size() == 1) result.as_object()["change_id"] = ids[0];
          result.as_object()["change_ids"] = std::move(a);
        }
      } catch (const std::exception&) {
      }
      return std::nullopt;
    };
  }
  auto reply = mcp_->handle(req.body, policy);
  if (calls > 1) extra["calls"] = calls;
  std::string perr;
  if (auto call = json::parse(req.body, &perr); call && call->is_object()) {
    if (const auto* m = call->as_object().if_contains("method"); m && m->is_string()) extra["mcp_method"] = m->as_string();
  }
  finish(json_response(reply.status, std::move(reply.body)), json::dump(extra));
}

void Host::loop_beat(const std::string& loop, const std::string& error) {
  std::lock_guard lock(loops_mu_);
  auto& st = loops_[loop];
  st.last_ms = base::now_unix_ms();
  if (error.empty()) {
    st.last_ok_ms = st.last_ms;
  } else {
    st.last_error = error.substr(0, 300);
  }
}

void Host::loop_trouble(const std::string& loop, const std::string& what) {
  std::lock_guard lock(loops_mu_);
  auto& st = loops_[loop];
  st.trouble_ms = base::now_unix_ms();
  st.trouble = what.substr(0, 300);
}

std::pair<int, bj::object> Host::readiness(bool probe_storage) {
  auto cfg = config_.get();
  bj::object checks;
  bool ok = true;
  auto check = [&](const char* name, bool good, bj::object more = {}) {
    more["ok"] = good;
    checks[name] = std::move(more);
    ok = ok && good;
  };
  // Storage: the host can write where it keeps everything (a probe file, made and removed).
  {
    bool good = true;
    std::string why;
    if (probe_storage) {
      auto probe = cfg->storage / "tmp" / ("ready-" + std::to_string(base::os_random_u64()));
      std::error_code ec;
      fs::create_directories(probe.parent_path(), ec);
      good = base::write_file_durable(probe, "ready");
      fs::remove(probe, ec);
      if (!good) why = "can't write under " + cfg->storage.string();
    }
    check("storage", good, why.empty() ? bj::object{} : bj::object{{"why", why}});
  }
  // Identity and OAuth: the host realm answers (its database opens and reads).
  // Identity and OAuth: the host realm and every tenant's realm answer (their databases open and
  // read), since a site's /mcp signs in and refreshes in its own realm.
  {
    std::set<std::string> realms{std::string(identity::kHostRealm)};
    for (const auto& [d, e] : cfg->domains) realms.insert(e.tenant);
    bj::array failing;
    for (const auto& realm : realms) {
      try {
        identity_->clients(realm);
      } catch (const std::exception& e) {
        failing.push_back(bj::value(realm + ": " + e.what()));
      }
    }
    bool good = failing.empty();
    check("identity", good, bj::object{{"realms", int64_t(realms.size())}});
    if (!good) checks["identity"].as_object()["why"] = std::move(failing);
  }
  // Every box with a version set active should be served; one that isn't (a load failure, a
  // route conflict at startup) is named. It doesn't fail the check: readiness is the deploy's
  // gate, and one box a new host can't load would otherwise roll back every deploy.
  auto active = boxes_->active();
  auto unloaded = boxes_->unloaded();
  {
    bj::array names;
    for (const auto& b : unloaded) names.push_back(bj::value(b));
    bj::object detail{{"loaded", active->boxes.size()}};
    if (!names.empty()) detail["not_loaded"] = std::move(names);
    check("boxes", true, std::move(detail));
  }
  // Stores that wouldn't open at startup (their features are off until fixed and restarted).
  {
    bj::array off;
    for (const auto& s : stores_off_) off.push_back(bj::value(s));
    bool good = off.empty();
    check("stores", good, good ? bj::object{} : bj::object{{"off", std::move(off)}});
  }
  // The background loops: each must have come round lately, and not be failing whole rounds.
  {
    int64_t now = base::now_unix_ms();
    bj::object loops;
    bool good = true;
    std::lock_guard lock(loops_mu_);
    for (const auto& [name, st] : loops_) {
      // The nightly upload can hold its loop for a long while.
      int64_t stale = name == "backups" ? 6 * 3600 * 1000 : 10 * 60 * 1000;
      bool fresh = now - st.last_ms < stale;
      bool failing = st.last_ok_ms < st.last_ms && now - st.last_ok_ms >= 10 * 60 * 1000;
      good = good && fresh && !failing;
      bj::object o{{"ok", fresh && !failing}, {"ago_ms", now - st.last_ms}};
      if (failing) o["failing_since_ms"] = now - st.last_ok_ms;
      if (st.trouble_ms && now - st.trouble_ms < 3600 * 1000) o["trouble_ago_ms"] = now - st.trouble_ms;
      loops[name] = std::move(o);
    }
    checks["loops"] = bj::object{{"ok", good}, {"loops", std::move(loops)}};
    ok = ok && good;
  }
  check("mcp", mcp_ != nullptr);
  // The panel's stores are optional: off is reported, never a failure.
  checks["change_log"] = bj::object{{"ok", true}, {"on", changes_ != nullptr}};
  checks["traffic"] = bj::object{{"ok", true}, {"on", traffic_ != nullptr}};
  checks["queue"] = bj::object{{"ok", pool_->queued() < max_queued_}, {"queued", pool_->queued()}};
  ok = ok && pool_->queued() < max_queued_;
  // The last deploy's outside check, in brief (the whole of it is /goober/deploy).
  bj::value deploy = nullptr;
  if (auto text = base::read_file(cfg->storage / "operations" / "deploy-check.json")) {
    std::string err;
    if (auto d = json::parse(*text, &err); d && d->is_object()) {
      const auto& o = d->as_object();
      bj::object brief;
      for (const char* k : {"at", "build", "ok", "failed"}) {
        if (auto* v = o.if_contains(k)) brief[k] = *v;
      }
      deploy = std::move(brief);
    }
  }
  bj::object body{{"ok", ok},
                  {"build", std::string(kBuildCommit)},
                  {"deploy", std::move(deploy)},
                  {"version", std::string(kVersion)},
                  {"started_at", base::format_utc_ms(started_ms_)},
                  {"uptime_ms", base::now_unix_ms() - started_ms_},
                  {"checks", std::move(checks)},
                  {"startup_notes", startup_notes_.size()}};
  return {ok ? 200 : 503, std::move(body)};
}

void Host::register_tools() {
  using mcp::integer_property;
  using mcp::object_schema;
  using mcp::string_property;

  auto tenant_prop = string_property("A tenant name from the host config, like playground, or _host for host-wide "
                                     "content such as error-message/404.html.");

  mcp_->add_tool({"host_health",
                  "Read tool. The host's state; start here when something seems wrong. Returns build and version, "
                  "ready (the same checks as /goober/ready), uptime_ms, requests, queued_jobs, storage, boxes and "
                  "boxes_loaded (each live box with its version), mail (how mail leaves), smtp_listener (whether mail "
                  "in's listener on port 25 answers, with its banner), backups (healthy, last_nightly_at, "
                  "last_increment_at, last_error, key_warning), tenants_without_postal_address with postal_address_how "
                  "(a person sets it; no tool does), and startup_notes.",
                  object_schema({}, {}), [this](const bj::object&) {
                    bj::array notes;
                    for (const auto& n : startup_notes_) notes.push_back(bj::value(n));
                    auto cfg = config_.get();
                    auto backup = backup_status(false);
                    bj::object backups{{"enabled", backup.at("enabled")}};
                    for (const char* k : {"healthy", "last_nightly_at", "last_increment_at", "last_error", "key_warning"}) {
                      if (auto* v = backup.if_contains(k)) backups[k] = *v;
                    }
                    // Each box and the version answering, so a deploy can be checked by name.
                    bj::array loaded;
                    for (const auto& [name, b] : boxes_->active()->boxes) {
                      loaded.push_back(bj::object{{"box", name}, {"version", b->version}});
                    }
                    // Tenants whose outreach, lists, and invites are refused until the config
                    // names a postal address (029 § 8).
                    bj::array no_postal;
                    std::set<std::string> seen;
                    for (const auto& [d, e] : cfg->domains) {
                      if (seen.insert(e.tenant).second && cfg->tenant(e.tenant).postal_address.empty()) {
                        no_postal.push_back(bj::value(e.tenant));
                      }
                    }
                    bj::value postal_how = no_postal.empty() ? bj::value(nullptr)
                                                             : bj::value(postal_address_how("$tenant"));
                    return ToolResult::ok(bj::object{{"version", std::string(kVersion)},
                                                     {"build", kBuildCommit},
                                                     {"ready", readiness(false).second},
                                                     {"tenants_without_postal_address", std::move(no_postal)},
                                                     {"postal_address_how", std::move(postal_how)},
                                                     {"smtp_listener", smtp_listener()},
                                                     {"boxes_loaded", std::move(loaded)},
                                                     {"uptime_ms", base::now_unix_ms() - started_ms_},
                                                     {"requests", requests_.load()},
                                                     {"queued_jobs", pool_->queued()},
                                                     {"storage", cfg->storage.string()},
                                                     {"boxes", boxes_->active()->boxes.size()},
                                                     {"sandbox", cfg->sandbox},
                                                     {"mail", cfg->sandbox ? "dev (sandbox)" : cfg->identity.mailer},
                                                     {"backups", std::move(backups)},
                                                     {"startup_notes", std::move(notes)}});
                  }});

  mcp_->add_tool({"ids_status",
                  "Read tool. The host's id service (design/machine/038.Ids.md), which mints every new user, invite, "
                  "upload, and row keyed by type id. Returns counter (the first id never handed out), next and "
                  "range_end (the range this host is using; null before its first new id since it started), and the "
                  "newest ranges; ids are strings in JSON. An empty ranges list means no id has been minted yet, and a "
                  "note says so. Nothing can be reserved: a box's or a job's inserts get their ids as they happen. Ids "
                  "from before the service (earlier users, rowid-keyed tables) are kept, below the counter.",
                  object_schema({}, {}), [this](const bj::object&) {
                    if (!ids_) return ToolResult::fail("the id service is off: see host_health's startup notes");
                    auto [next, end] = ids_->current();
                    bj::array ranges;
                    for (const auto& r : ids_->ranges(10)) {
                      ranges.push_back(bj::object{{"start", std::to_string(r.start)}, {"length", r.length},
                                                  {"to", r.vended_to}, {"at", base::format_utc_ms(r.vended_ms)}});
                    }
                    bool none_yet = ranges.empty();
                    bj::object out{{"counter", std::to_string(ids_->counter())},
                                   {"next", end ? bj::value(std::to_string(next)) : bj::value(nullptr)},
                                   {"range_end", end ? bj::value(std::to_string(end)) : bj::value(nullptr)},
                                   {"ranges", std::move(ranges)}};
                    // What an empty answer means, since it looks like nothing is working.
                    if (none_yet) {
                      out["note"] = "no id minted yet: the first new user, invite, upload, or row keyed by id takes a "
                                    "range of " + std::to_string(ids::kRangeLength) + " from the counter. Nothing needs "
                                    "reserving: a job's inserts get their ids as they happen. Ids from before the service "
                                    "(users who signed up earlier, rowid-keyed tables) are kept, below the counter.";
                    } else if (!end) {
                      out["note"] = "no range in use since the host started; the next new id takes one";
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"domain_list",
                  "Read tool. Every domain in the host config: {domain, tenant, owner} for a tenant's own domain, "
                  "and {domain, tenant, redirect_to} for a redirect domain, which the host answers with a 301 to "
                  "redirect_to. Domains are set in host.json by a person (or the deploy); no tool adds one.",
                  object_schema({}, {}), [this](const bj::object&) {
                    bj::array out;
                    auto cfg = config_.get();
                    for (const auto& [domain, entry] : cfg->domains) {
                      out.push_back(bj::object{{"domain", domain}, {"tenant", entry.tenant}, {"owner", entry.owner}});
                    }
                    // Redirect domains (015): the host answers each with a 301 to its primary.
                    for (const auto& [domain, to] : cfg->redirects) {
                      out.push_back(bj::object{{"domain", domain}, {"tenant", cfg->domains.at(to).tenant}, {"redirect_to", to}});
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool(
      {"content_put",
       "Changing tool. Writes one static file for a tenant through the host WAL; it goes live at once (there is no "
       "draft) and replaces any file at that path. Give text, or base64 for binary. The path is relative, like "
       "index.html or css/site.css; / serves index.html. The type comes from the extension (.csv is text/csv, .atom "
       "application/atom+xml, .webmanifest, .json, .xml, .svg, ...). Both text and base64 are stored byte for byte, "
       "but a tool-call layer may change text on its way here (CRLF becoming LF): when the exact bytes matter, give "
       "sha256 and the write is refused unless what arrived matches. Returns path, bytes, sha256, content_type, and "
       "file_etag (the stored file's; etag is the same value). Pass if_etag (content_get's or list_etags' file_etag, "
       "or \"none\" for a new file) so you never overwrite a teammate's change made since you read it.",
       object_schema({{"tenant", tenant_prop},
                      {"path", string_property("A relative path, like index.html.")},
                      {"text", string_property("The file's contents as text.")},
                      {"base64", string_property("The file's contents as base64, for binary files.")},
                      {"sha256", string_property("The SHA-256 (hex) the bytes must have, or nothing is written.")},
                      {"if_etag", string_property("Write only if the stored file has this file_etag; \"none\" writes "
                                                  "only a new file.")}},
                     {"tenant", "path"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto path = arg(args, "path");
         if (!tenant || !path) return ToolResult::fail("tenant and path are required");
         if (auto want = arg(args, "if_etag")) {
           auto got = content_->get(*tenant, *path);
           std::string current = got ? got->etag : "none";
           if (*want != current && "\"" + *want + "\"" != current) {
             return ToolResult::fail("nothing written: if_etag is " + *want + " but the file's file_etag is " + current +
                                     " (someone changed it since you read it)");
           }
         }
         auto text = arg(args, "text");
         auto b64 = arg(args, "base64");
         if (bool(text) == bool(b64)) return ToolResult::fail("give exactly one of text or base64");
         std::string data;
         if (text) {
           data = *text;
         } else {
           auto decoded = base::base64_decode(*b64);
           if (!decoded) return ToolResult::fail("base64 is not valid");
           data = std::move(*decoded);
         }
         std::string sha = base::sha256_hex(data);
         if (auto want = arg(args, "sha256"); want && base::to_lower(*want) != sha) {
           size_t crs = std::count(data.begin(), data.end(), '\r');
           return ToolResult::fail("nothing written: the " + std::to_string(data.size()) + " bytes that arrived have sha256 " +
                                   sha + ", not " + *want + (text ? " (" + std::to_string(crs) +
                                   " CR bytes arrived; if CRLF line endings were lost on the way, send base64)" : ""));
         }
         std::string err;
         if (!content_->put(*tenant, *path, data, &err)) return ToolResult::fail(err);
         auto f = content_->get(*tenant, *path);
         return ToolResult::ok(bj::object{{"tenant", *tenant},
                                          {"path", *path},
                                          {"bytes", data.size()},
                                          {"sha256", sha},
                                          {"content_type", f ? f->content_type : ""},
                                          {"file_etag", f ? f->etag : ""},
                                          {"etag", f ? f->etag : ""}});
       }});

  mcp_->add_tool({"content_get",
                  "Read tool. One stored file as it is on disk (before any template or partial): text as text, "
                  "anything else as base64, with content_type, bytes, and file_etag (etag is the same value). What "
                  "visitors get, and its body_etag, is url_check's or page_render's to say.",
                  object_schema({{"tenant", tenant_prop}, {"path", string_property("A relative path.")}},
                                {"tenant", "path"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto path = arg(args, "path");
                    if (!tenant || !path) return ToolResult::fail("tenant and path are required");
                    auto f = content_->get(*tenant, *path);
                    if (!f) return ToolResult::fail("no such file");
                    bj::object out{{"tenant", *tenant}, {"path", *path}, {"content_type", f->content_type},
                                   {"file_etag", f->etag}, {"etag", f->etag}, {"bytes", f->data.size()}};
                    if (looks_like_text(f->data)) {
                      out["text"] = f->data;
                    } else {
                      out["base64"] = base::base64_encode(f->data);
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"content_list",
                  "Read tool. The paths of a tenant's stored files (pages, templates, partials, and other files), "
                  "optionally under a prefix, as a list of strings.",
                  object_schema({{"tenant", tenant_prop}, {"prefix", string_property("Only paths starting with this.")}},
                                {"tenant"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    if (!tenant) return ToolResult::fail("tenant is required");
                    bj::array out;
                    for (const auto& p : content_->list(*tenant, arg(args, "prefix").value_or(""))) {
                      out.push_back(bj::value(p));
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"content_delete",
                  "Changing tool. Deletes one stored file through the host WAL; it stops being served at once. "
                  "Returns deleted (the path).",
                  object_schema({{"tenant", tenant_prop}, {"path", string_property("A relative path.")}},
                                {"tenant", "path"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto path = arg(args, "path");
                    if (!tenant || !path) return ToolResult::fail("tenant and path are required");
                    std::string err;
                    if (!content_->remove(*tenant, *path, &err)) return ToolResult::fail(err);
                    return ToolResult::ok(bj::object{{"deleted", *path}});
                  }});

  mcp_->add_tool(
      {"version_upload",
       "Changing tool. Uploads a box version: a manifest (object or JSON text) and its module (wat or wasm_base64), "
       "plus types when the manifest names a types document (\"types\": \"x.openapi.json\": the OpenAPI "
       "components.schemas its json:$Type bodies, responses, and tool inputs refer to, in goober's strict subset). "
       "The host checks the manifest, the module's imports (goober-abi@1 only), one export goober_route_$name per "
       "route, the types, the data strategies against stored schemas, and conflicts with the live boxes. Three modes: "
       "(1) by default the version is stored and goes live at once, replacing the box's live version (there is no "
       "separate review stage; an agent whose approvals are on waits for a person first, as with every changing "
       "tool); (2) activate: false stores it dark: kept, not serving, until version_promote {box, version} makes it "
       "go live. A dark version whose route names or mail addresses clash with a live box is stored with a warning "
       "and checked again at promote: that is how a route moves between boxes (box_disable the old box, then "
       "version_promote the new; roll back with box_disable the new, box_enable the old); (3) dry_run: true checks "
       "everything and stores nothing, changes nothing (activate is ignored); schemas lets it use schemas not stored "
       "yet. Returns version, active, and warnings; a dry run returns dry_run, ok, stored: false, active: false, and "
       "schemas_checked and schema_warnings when schemas were given. Every mode also returns review (what this "
       "version changes from the box's live version, as version_diff gives it, each change classed widens, narrows, "
       "or neutral) and widens: the changes that give the box more reach, as sentences (\"strategy s may now delete "
       "from members\", \"route GET /x is now public\"). Read widens before going live; a person approving a held "
       "upload sees it too. After it goes live, version_health compares it with the version it replaced.",
       object_schema({{"manifest", mcp::any_property("The manifest, as a JSON object or as JSON text.")},
                      {"dry_run", mcp::boolean_property("Check only: store nothing, activate nothing.")},
                      {"schemas", mcp::any_property("With dry_run: schemas the manifest uses that aren't stored yet "
                                                    "(a list of schema objects or JSON texts), checked as schema_check "
                                                    "checks them and used as if stored. Nothing is stored.")},
                      {"activate", mcp::boolean_property("false stores the version without making it active (every "
                                                         "check still runs); version_promote makes it live. Default true.")},
                      {"wat", string_property("The module as WebAssembly text.")},
                      {"wasm_base64", string_property("The module as base64 binary WASM.")},
                      {"types", mcp::any_property("The types document (OpenAPI 3.x), as a JSON object or text.")}},
                     {"manifest"}),
       [this](const bj::object& args) {
         auto it = args.find("manifest");
         if (it == args.end()) return ToolResult::fail("manifest is required");
         std::string manifest = it->value().is_string() ? std::string(it->value().get_string())
                                                         : json::pretty(it->value());
         auto wat = arg(args, "wat");
         auto b64 = arg(args, "wasm_base64");
         if (bool(wat) == bool(b64)) return ToolResult::fail("give exactly one of wat or wasm_base64");
         std::string module;
         if (wat) {
           module = *wat;
         } else {
           auto decoded = base::base64_decode(*b64);
           if (!decoded) return ToolResult::fail("wasm_base64 is not valid base64");
           module = std::move(*decoded);
         }
         std::string types;
         if (auto t = args.find("types"); t != args.end() && !t->value().is_null()) {
           types = t->value().is_string() ? std::string(t->value().get_string()) : json::pretty(t->value());
         }
         std::string err;
         bool dry_run = false;
         if (auto d = args.find("dry_run"); d != args.end() && d->value().is_bool()) dry_run = d->value().get_bool();
         // Schemas not stored yet, for a dry run only: checked as schema_check checks them, then
         // used for the manifest's data checks as if stored, as a source build uses its own.
         std::map<std::string, db::Schema> pending;
         bj::array schema_warnings;
         if (auto s = args.find("schemas"); s != args.end() && !s->value().is_null()) {
           if (!dry_run) return ToolResult::fail("schemas are for dry_run; store them with schema_put before a real upload");
           if (!s->value().is_array() || s->value().as_array().size() > 20) {
             return ToolResult::fail("schemas is a list of up to 20 schemas, as JSON objects or text");
           }
           for (const auto& v : s->value().as_array()) {
             std::string text = v.is_string() ? std::string(v.get_string()) : json::dump(v);
             auto checked = schemas_->check(text, &err);
             if (!checked) return ToolResult::fail("schemas: " + err);
             for (const auto& w : checked->warnings) schema_warnings.push_back(bj::value(checked->name + ": " + w));
             pending[checked->name] = *checked;
           }
         }
         // What this version changes from the live one, for whoever reads the answer (and the
         // person approving it, when it is held): version_diff's review, against the live version.
         auto attach_review = [&](bj::object& out) {
           std::string rerr;
           if (auto r = review_upload(args, &rerr)) {
             out["widens"] = r->at("widens");
             out["review"] = std::move(*r);
           }
         };
         if (dry_run) {
           pending_schemas_ = pending.empty() ? nullptr : &pending;
           struct Clear {
             ~Clear() { pending_schemas_ = nullptr; }
           } clear;
           if (!boxes_->check(manifest, module, types, &err)) return ToolResult::fail(err);
           bj::object out{{"dry_run", true}, {"ok", true}, {"stored", false}, {"active", false}};
           attach_review(out);
           if (!pending.empty()) {
             bj::array names;
             for (const auto& [name, _] : pending) names.push_back(bj::value(name));
             out["schemas_checked"] = std::move(names);
             if (!schema_warnings.empty()) out["schema_warnings"] = std::move(schema_warnings);
           }
           return ToolResult::ok(std::move(out));
         }
         // activate: false stores a version dark: every check runs, and version_promote makes it live.
         bool activate = true;
         if (auto a = args.find("activate"); a != args.end() && a->value().is_bool()) activate = a->value().get_bool();
         // The review is against the version that was live before this one: taken first.
         bj::object reviewed;
         attach_review(reviewed);
         // The live version this one replaces ran live: going back to it later counts as tried.
         std::string box_name, replaced, perr;
         if (auto m = json::parse(manifest, &perr); m && m->is_object() && m->as_object().contains("box") &&
                                                    m->as_object().at("box").is_string()) {
           box_name = std::string(m->as_object().at("box").as_string());
         }
         if (auto live = boxes_->active(); live->boxes.count(box_name)) replaced = live->boxes.at(box_name)->version;
         auto result = activate ? boxes_->upload(manifest, module, types, &err) : boxes_->store(manifest, module, types, &err);
         if (!result) return ToolResult::fail(err);
         if (activate && job_runs_) job_runs_->replaced(box_name, replaced, result->version, current_operator(), base::now_unix_ms());
         bj::array warnings;
         for (const auto& w : result->warnings) warnings.push_back(bj::value(w));
         bj::object out{{"version", result->version}, {"active", activate}, {"warnings", warnings}};
         for (auto& kv : reviewed) out[kv.key()] = std::move(kv.value());
         if (auto* r = out.if_contains("review"); r && r->is_object()) r->as_object()["to"] = result->version;
         if (!activate) out["next"] = "stored, not live: version_promote {box, version} makes it active";
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool({"version_list",
                  "Read tool. A box's stored versions, each {version, active, stored_at}: active is true for the one "
                  "live now, false for older versions and versions stored dark (activate: false); versions are named "
                  "by hash, so stored_at says which is newest. A box taken out by box_disable marks the version it had "
                  "with disabled and why. source {project, rev, job} names the source deploy that uploaded a version. "
                  "version_diff compares two of them.",
                  object_schema({{"box", string_property("The box name.")}}, {"box"}),
                  [this](const bj::object& args) {
                    auto box = arg(args, "box");
                    if (!box) return ToolResult::fail("box is required");
                    bj::array out;
                    auto disabled = boxes_->disabled(*box);
                    for (const auto& v : boxes_->versions(*box)) {
                      bj::object o{{"version", v.version}, {"active", v.active}};
                      // Versions are named by hash; when each was stored says which is newest.
                      if (v.stored_ms) o["stored_at"] = base::format_utc_ms(v.stored_ms);
                      if (disabled && disabled->version == v.version) {
                        o["disabled"] = true;  // box_disable took it out; box_enable puts it back
                        if (!disabled->why.empty()) o["why"] = disabled->why;
                      }
                      if (source_) {
                        if (auto d = source_->deploy_of_version(*box, v.version)) {
                          o["source"] = bj::object{{"project", d->project}, {"rev", d->rev}, {"job", d->job}};
                        }
                      }
                      out.push_back(std::move(o));
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"version_get",
                  "Read tool. One stored version's manifest (live, dark, or old): {box, version, manifest}. The whole "
                  "manifest, with the tools, timers, mail routes, jobs, data grants, and flags that box_list doesn't show.",
                  object_schema({{"box", string_property("The box name.")}, {"version", string_property("The version.")}},
                                {"box", "version"}),
                  [this](const bj::object& args) {
                    auto box = arg(args, "box");
                    auto version = arg(args, "version");
                    if (!box || !version) return ToolResult::fail("box and version are required");
                    auto text = boxes_->manifest_text(*box, *version);
                    if (!text) return ToolResult::fail("no such version");
                    std::string err;
                    auto v = json::parse(*text, &err);
                    return ToolResult::ok(bj::object{{"box", *box}, {"version", *version},
                                                     {"manifest", v ? *v : bj::value(*text)}});
                  }});

  mcp_->add_tool(
      {"box_disable",
       "Changing tool. Takes a box out of service at once: none of its routes, tools, timers, mail routes, or jobs "
       "answer (a path it served falls through to the site's pages and files, or 404). Its versions and data stay, "
       "and it frees its route names and mail addresses, so another box may take them. box_enable puts it back; "
       "version_promote, or uploading a live version, also brings it back. To take out one route, upload a version "
       "without it instead. Returns box, version (the one it had), and why.",
       object_schema({{"box", string_property("The box name.")},
                      {"why", string_property("Why, in a sentence: kept with the box and shown by version_list.")}},
                     {"box", "why"}),
       [this](const bj::object& args) {
         auto box = arg(args, "box");
         auto why = arg(args, "why").value_or("");
         if (!box) return ToolResult::fail("box is required");
         if (why.empty()) return ToolResult::fail("why is required: say why the box is out of service");
         std::string err;
         auto version = boxes_->disable(*box, why, &err);
         if (!version) return ToolResult::fail(err);
         return ToolResult::ok(bj::object{{"box", *box}, {"disabled", true}, {"version", *version}, {"why", why},
                                          {"next", "box_enable {box} serves this version again"}});
       }});
  mcp_->add_tool(
      {"box_enable",
       "Changing tool. Puts a disabled box back in service at once: the version it had, or the version given, "
       "checked against the live boxes as any promotion is. Refused while another box holds its route names or "
       "mail addresses (box_disable that box first: the rollback of a move). Returns box and active (the version "
       "now live).",
       object_schema({{"box", string_property("The box name.")},
                      {"version", string_property("A version to serve instead of the one it had.")}},
                     {"box"}),
       [this](const bj::object& args) {
         auto box = arg(args, "box");
         if (!box) return ToolResult::fail("box is required");
         auto disabled = boxes_->disabled(*box);
         auto version = arg(args, "version");
         if (!version) {
           if (!disabled || disabled->version.empty()) {
             return ToolResult::fail("box " + *box + " isn't disabled; version_promote {box, version} picks a version");
           }
           version = disabled->version;
         }
         std::string err;
         if (!boxes_->promote(*box, *version, &err)) {
           // Another box took its names or addresses while it was off (a route moved): that box
           // goes out first, which is the rollback of the move.
           if (err.find("already used by box") != std::string::npos || err.find("already claimed by box") != std::string::npos) {
             err += "; to roll back a move, box_disable that box first, then box_enable " + *box;
           }
           return ToolResult::fail(err);
         }
         return ToolResult::ok(bj::object{{"box", *box}, {"active", *version}});
       }});

  mcp_->add_tool({"version_promote",
                  "Changing tool. Makes a stored version of a box go live at once, replacing its live version: a "
                  "version stored dark (activate: false), or an older one, which is how a box rolls back. Checked "
                  "against the live boxes like an upload; a disabled box comes back with it. Returns box, active, "
                  "tried (what touched this version before: route_try calls by kind and tenant, a job_dry_run, or "
                  "having run live before), and warnings: a version nothing tried still goes live, with a warning "
                  "saying so. Try first (route_try, job_dry_run), then promote, then version_health.",
                  object_schema({{"box", string_property("The box name.")}, {"version", string_property("The version.")}},
                                {"box", "version"}),
                  [this](const bj::object& args) {
                    auto box = arg(args, "box");
                    auto version = arg(args, "version");
                    if (!box || !version) return ToolResult::fail("box and version are required");
                    std::string replaced;
                    if (auto active = boxes_->active(); active->boxes.count(*box)) replaced = active->boxes.at(*box)->version;
                    std::string err;
                    if (!boxes_->promote(*box, *version, &err)) return ToolResult::fail(err);
                    bj::object out{{"box", *box}, {"active", *version}};
                    if (job_runs_) {
                      // The version it replaced ran live, so going back to it later counts as tried.
                      job_runs_->replaced(*box, replaced, *version, current_operator(), base::now_unix_ms());
                      auto tried = job_runs_->tries(*box, *version);
                      if (tried.empty() && replaced != *version) {
                        out["warnings"] = bj::array{
                            "version " + *version + " of " + *box + " went live untried: no route_try, no job_dry_run, "
                            "and it never ran live before. Watch version_health now, and next time try a version "
                            "(route_try on it while it's stored dark) before promoting it."};
                      }
                      out["tried"] = std::move(tried);
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool(
      {"route_check",
       "Read tool. Which handler answers an HTTP request to a domain: the host itself (its reserved paths; a "
       "redirect domain's 301), a box route (its box, version, route name, pattern, parameters, template, and who "
       "may call it), the site's pages and files (the file, its kind, and its partials), or nothing (404, 405). "
       "For a mail address, use mail_route_check. Lists every "
       "other box route whose path also fits, so a shadowed route shows up. Every box whose domains cover the domain "
       "(\"*\" covers all of them) shares one route table there: the same verb and shape (literals, with every "
       "parameter as {}) in two boxes is refused at upload, and otherwise a literal segment beats a parameter at the "
       "first place two patterns differ, across boxes.",
       object_schema({{"domain", string_property("A domain from the host config, like example.com.")},
                      {"path", string_property("The request path, like /places/joes; a ?query is set aside and reported.")},
                      {"verb", string_property("get (the default), post, put, patch, or delete.")}},
                     {"domain", "path"}),
       [this](const bj::object& args) {
         auto domain = base::to_lower(arg(args, "domain").value_or(""));
         auto cfg = config_.get();
         if (auto to = cfg->redirects.find(domain); to != cfg->redirects.end()) {
           return ToolResult::ok(bj::object{{"domain", domain},
                                            {"tenant", cfg->domains.at(to->second).tenant},
                                            {"served_by", "host"},
                                            {"handler", "a redirect domain"},
                                            {"redirect_to", to->second},
                                            {"note", "every path gets a 301 (308 for a write) to the same path on " +
                                                         to->second + "; ask route_check about that domain"}});
         }
         auto entry = cfg->domains.find(domain);
         if (entry == cfg->domains.end()) return ToolResult::fail(domain + " isn't a domain in the host config");
         const std::string& tenant = entry->second.tenant;
         std::string verb = base::to_lower(arg(args, "verb").value_or("get"));
         if (verb == "head") verb = "get";
         // A URL's query and fragment don't choose the route; the query is reported as parsed.
         std::string given = arg(args, "path").value_or("");
         given = given.substr(0, given.find('#'));
         std::string query;
         if (auto q = given.find('?'); q != std::string::npos) {
           query = given.substr(q + 1);
           given.resize(q);
         }
         auto path = router::normalize_path(given);
         if (!path) return ToolResult::fail("not a path the host would accept");
         const std::string& p = path->path;
         bj::object out{{"domain", domain}, {"tenant", tenant}, {"verb", verb}, {"path", p}};
         if (!query.empty()) {
           bj::object q;
           for (const auto& [k, v] : router::parse_query(query)) q[k] = v;
           out["query"] = std::move(q);
         }
         // In the order the host checks them (Host::handle).
         std::string host_part;
         if (p == kMailHookPath) host_part = "the mail hook (Stalwart only)";
         else if (p == "/goober/health") host_part = "the health check";
         else if (p == kOperationsMcpPath) host_part = "the operator MCP endpoint";
         else if (p == "/mcp") host_part = "the site's MCP endpoint (box tools)";
         else if (p == "/oauth" || p.starts_with("/oauth/") || p.starts_with("/.well-known/")) host_part = "OAuth";
         else if (p.starts_with("/identity/")) host_part = "sign-in and accounts";
         else if (p == "/attachment" || p.starts_with("/attachment/")) host_part = "attachments";
         if (!host_part.empty()) {
           out["served_by"] = "host";
           out["handler"] = host_part;
           out["note"] = "a host-owned path: no box route or page is consulted";
           return ToolResult::ok(std::move(out));
         }
         auto active = boxes_->active();
         const auto& table = active->table_for(domain);
         auto describe = [](const boxes::RouteRef& ref) {
           bj::array reqs;
           for (const auto& clause : ref.route->requirements) {
             bj::array c;
             for (const auto& role : clause) c.push_back(bj::value(role));
             reqs.push_back(std::move(c));
           }
           return bj::object{{"box", ref.box->manifest.box},
                             {"version", ref.box->version},
                             {"route", ref.route->name},
                             {"verb", ref.route->verb},
                             {"pattern", ref.route->pattern.source},
                             {"public", ref.route->is_public},
                             {"requirements", std::move(reqs)},
                             {"template", ref.route->template_name},
                             // Shared partials: always in a template's HTML, in the box's own when includes is on.
                             {"includes", ref.route->includes || !ref.route->template_name.empty()}};
         };
         auto m = table.match(path->segments, verb);
         bj::array also, other_verbs;
         for (const auto& [v, ref] : table.fitting(path->segments)) {
           if (m.value && ref == m.value) continue;
           (v == verb ? also : other_verbs).push_back(describe(*ref));
         }
         out["also_fits"] = std::move(also);
         out["other_verbs"] = std::move(other_verbs);
         if (m.value) {
           out["served_by"] = "box";
           auto route = describe(*m.value);
           bj::object params;
           for (const auto& [k, v] : m.params) params[k] = v;
           route["params"] = std::move(params);
           out["route"] = std::move(route);
           return ToolResult::ok(std::move(out));
         }
         if (m.method_not_allowed || verb != "get") {
           out["served_by"] = "nobody";
           out["status"] = 405;
           out["note"] = m.method_not_allowed ? "box routes fit this path, but not with this verb"
                                              : "pages answer only get";
           return ToolResult::ok(std::move(out));
         }
         // Pages and files, resolved as a visit is: name.md, name.html, name/index.md, name/index.html, the file.
         std::string rel = p.substr(1);
         while (!rel.empty() && rel.back() == '/') rel.pop_back();
         std::vector<std::string> candidates = rel.empty() ? std::vector<std::string>{"index.md", "index.html"}
                                                           : std::vector<std::string>{rel + ".md", rel + ".html",
                                                                                      rel + "/index.md", rel + "/index.html"};
         out["served_by"] = "content";
         for (const auto& c : candidates) {
           if (auto page = site_->load(tenant, c)) {
             out["file"] = page->path;
             out["kind"] = site::kind_name(page->kind);
             bj::array partials;
             for (const auto& n : site::include_names(page->body)) partials.push_back(bj::value(n));
             out["partials"] = std::move(partials);
             return ToolResult::ok(std::move(out));
           }
         }
         if (!rel.empty() && content_->get(tenant, rel)) {
           out["file"] = rel;
           out["kind"] = "file";
           return ToolResult::ok(std::move(out));
         }
         out["status"] = 404;
         out["note"] = "no box route, page, or file answers it";
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool(
      {"mcp_tools_preview",
       "Read tool. What a domain's /mcp lists right now: every box tool there, with its box, active version, requirements, and "
       "typed input, and, for a person (email), whether they see it and why not. Use it when an MCP client shows "
       "tools that don't match a deploy: /mcp builds its list from the active versions on every request, so a "
       "different list means a different domain, a person whose roles hide the tool, or the client's own copy.",
       object_schema({{"domain", string_property("A domain from the host config, like example.com.")},
                      {"email", string_property("A person of that domain's realm, to see the list as they do.")}},
                     {"domain"}),
       [this](const bj::object& args) {
         auto domain = base::to_lower(arg(args, "domain").value_or(""));
         auto cfg = config_.get();
         auto entry = cfg->domains.find(domain);
         if (entry == cfg->domains.end()) return ToolResult::fail(domain + " isn't a domain in the host config");
         const std::string& tenant = entry->second.tenant;
         std::optional<manifest::Caller> caller;
         std::string email = base::to_lower(arg(args, "email").value_or(""));
         if (!email.empty()) {
           auto realm = identity_->describe(tenant, base::now_unix_ms());
           for (const auto& u : realm.at("users").as_array()) {
             if (u.at("email").as_string() != email) continue;
             std::vector<std::string> roles;
             for (const auto& r : u.at("roles").as_array()) roles.emplace_back(r.as_string());
             caller = manifest::Caller{std::string(u.at("user_id").as_string()), false, roles};
           }
           if (!caller) return ToolResult::fail(email + " has no account in tenant " + tenant + "'s realm");
         }
         auto active = boxes_->active();
         bj::array tools;
         for (const auto& [name, ref] : active->tools_for(domain)) {
           bj::array reqs;
           for (const auto& clause : ref.route->requirements) {
             bj::array c;
             for (const auto& role : clause) c.push_back(bj::value(role));
             reqs.push_back(std::move(c));
           }
           bj::object t{{"name", name},
                        {"box", ref.box->manifest.box},
                        {"version", ref.box->version},
                        {"requirements", std::move(reqs)},
                        {"input", ref.route->input.empty() ? bj::value(nullptr) : bj::value(ref.route->input)}};
           if (caller) {
             auto access = manifest::check_access(*ref.route, &*caller, {}, tenant);
             t["listed"] = access == manifest::Access::kAllowed;
             if (access != manifest::Access::kAllowed) t["why_not"] = "their roles meet none of the requirements";
           }
           tools.push_back(std::move(t));
         }
         bj::object out{{"domain", domain}, {"tenant", tenant}, {"tools", std::move(tools)}};
         if (caller) {
           bj::array roles;
           for (const auto& r : caller->roles) roles.push_back(bj::value(r));
           out["as"] = bj::object{{"email", email}, {"roles", std::move(roles)}};
         }
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool({"box_list",
                  "Read tool. The live boxes, each with version, mission, domains, its HTTP routes {verb, path, name}, "
                  "and mail (the addresses it takes). Disabled boxes and versions stored dark aren't listed "
                  "(version_list shows them); tools, timers, jobs, and flags are in version_get's manifest.",
                  object_schema({}, {}),
                  [this](const bj::object&) {
                    bj::array out;
                    for (const auto& [name, box] : boxes_->active()->boxes) {
                      bj::array routes;
                      for (const auto& r : box->manifest.routes) {
                        routes.push_back(bj::object{{"verb", r.verb}, {"path", r.pattern.source}, {"name", r.name}});
                      }
                      bj::array domains;
                      for (const auto& d : box->manifest.domains) domains.push_back(bj::value(d));
                      out.push_back(bj::object{{"box", name},
                                               {"version", box->version},
                                               {"mission", box->manifest.mission},
                                               {"domains", domains},
                                               {"routes", routes}});
                      // Mail routes: the addresses the box takes (mail_route_check says who takes one).
                      if (!box->manifest.mail.empty()) {
                        bj::array mail;
                        for (const auto& r : box->manifest.mail) {
                          mail.push_back(bj::object{{"address", r.mail_address}, {"name", r.name}});
                        }
                        out.back().as_object()["mail"] = std::move(mail);
                      }
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"box_logs",
                  "Read tool. The newest lines a box wrote with goober_log, plus the host's notes about calls that "
                  "failed (level host), as a list, newest last.",
                  object_schema({{"box", string_property("The box name.")},
                                 {"limit", integer_property("How many lines, 1 to 1000. Default 50.")}},
                                {"box"}),
                  [this](const bj::object& args) {
                    auto box = arg(args, "box");
                    if (!box || !base::is_valid_name(*box)) return ToolResult::fail("a valid box name is required");
                    auto lines = base::tail_lines(config_.get()->storage / "logs" / "boxes" / (*box + ".log"),
                                                  size_t(std::clamp<int64_t>(int_arg(args, "limit", 50), 1, 1000)));
                    return ToolResult::ok(parse_lines(lines));
                  }});

  mcp_->add_tool({"access_log",
                  "Read tool. The newest lines of the host's access log (every request and operator tool call), as a "
                  "list, newest last. log_search searches further back.",
                  object_schema({{"limit", integer_property("How many lines, 1 to 1000. Default 50.")}}, {}),
                  [this](const bj::object& args) {
                    auto lines = base::tail_lines(config_.get()->storage / "logs" / "access.log",
                                                  size_t(std::clamp<int64_t>(int_arg(args, "limit", 50), 1, 1000)));
                    return ToolResult::ok(parse_lines(lines));
                  }});

  // Identity (013.Identity.md; design/machine/006.LocalIdentity.md). A realm is a tenant.
  auto realm_prop = string_property("A tenant name from the host config; its identity realm.");
  auto known_tenant = [this](const std::optional<std::string>& t) {
    if (!t) return false;
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == *t) return true;
    }
    return false;
  };
  mcp_->add_tool({"realm_get",
                  "Read tool. A tenant realm's users (user_id as a string, email, roles, live sessions), and the newest "
                  "identity audit records.",
                  object_schema({{"tenant", realm_prop}}, {"tenant"}),
                  [this, known_tenant](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    return ToolResult::ok(identity_->describe(*tenant, base::now_unix_ms()));
                  }});
  mcp_->add_tool(
      {"role_grant",
       "Changing tool. Grants a tenant role (like owner, admin, or admin/project_42) to an email in a tenant's realm, "
       "at once and audited. The user is created (with a new host id) if the address is new, so the role waits for "
       "their first sign-in. Host roles come only from the host config. Returns granted, to, tenant.",
       object_schema({{"tenant", realm_prop},
                      {"email", string_property("The person's email address.")},
                      {"role", string_property("role or role/scope, each part [A-Za-z0-9_]+.")}},
                     {"tenant", "email", "role"}),
       [this, known_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto email = arg(args, "email");
         auto role = arg(args, "role");
         if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
         if (!email || !role) return ToolResult::fail("email and role are required");
         std::string why;
         if (!identity_->grant(*tenant, *email, *role, "operator", base::now_unix_ms(), &why)) {
           return ToolResult::fail(why);
         }
         return ToolResult::ok(bj::object{{"granted", *role}, {"to", *email}, {"tenant", *tenant}});
       }});
  mcp_->add_tool({"role_revoke",
                  "Changing tool. Revokes one tenant role from an email in a tenant's realm, at once and audited. "
                  "Returns revoked, from, tenant.",
                  object_schema({{"tenant", realm_prop},
                                 {"email", string_property("The person's email address.")},
                                 {"role", string_property("The exact role string.")}},
                                {"tenant", "email", "role"}),
                  [this, known_tenant](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto email = arg(args, "email");
                    auto role = arg(args, "role");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    if (!email || !role) return ToolResult::fail("email and role are required");
                    std::string why;
                    if (!identity_->revoke(*tenant, *email, *role, "operator", base::now_unix_ms(), &why)) {
                      return ToolResult::fail(why);
                    }
                    return ToolResult::ok(bj::object{{"revoked", *role}, {"from", *email}, {"tenant", *tenant}});
                  }});
  mcp_->add_tool({"sessions_revoke",
                  "Changing tool. Ends every session and token a person holds in a tenant's realm: their browser "
                  "sessions and the tokens apps and agents hold for them, at their next request. Audited. Returns ended "
                  "(how many). user_delete deletes the account itself.",
                  object_schema({{"tenant", realm_prop}, {"email", string_property("The person's email address.")}},
                                {"tenant", "email"}),
                  [this, known_tenant](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto email = arg(args, "email");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    if (!email) return ToolResult::fail("email is required");
                    int n = identity_->revoke_sessions(*tenant, *email, "operator", base::now_unix_ms());
                    return ToolResult::ok(bj::object{{"ended", n}});
                  }});

  // OAuth clients (design/machine/009.OAuth.md §2). Clients in identity.clients exist in
  // every realm; these register a client in one realm.
  auto client_json = [](const identity::Client& c) {
    bj::array uris;
    for (const auto& u : c.redirect_uris) uris.push_back(bj::value(u));
    return bj::object{{"client_id", c.client_id},
                      {"name", c.name},
                      {"redirect_uris", std::move(uris)},
                      {"acts_as", c.person ? "person" : "agent"},
                      {"source", c.source}};
  };
  mcp_->add_tool({"client_list",
                  "Read tool. The OAuth clients registered in a tenant's realm, apps that act as the person and "
                  "agents: each with client_id, name, redirect_uris, acts_as, and source.",
                  object_schema({{"tenant", realm_prop}}, {"tenant"}),
                  [this, known_tenant, client_json](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    bj::array out;
                    for (const auto& c : identity_->clients(*tenant)) out.push_back(client_json(c));
                    return ToolResult::ok(out);
                  }});
  mcp_->add_tool(
      {"client_put",
       "Changing tool. Registers or replaces an OAuth client in a tenant's realm. A person client (acts_as person) is the realm's own "
       "app and acts as the person, so only register one you trust like your own phone. Agents act for the person "
       "and reach only routes that allow agents.",
       object_schema({{"tenant", realm_prop},
                      {"client_id", string_property("1 to 64 of [A-Za-z0-9._-].")},
                      {"name", string_property("Shown on the consent screen.")},
                      {"redirect_uris", bj::value(bj::object{{"type", "array"},
                                                   {"items", bj::object{{"type", "string"}}},
                                                   {"description", "https URIs, http://127.0.0.1/... loopback (any "
                                                                   "port matches), or a private-use scheme like "
                                                                   "com.example.app:/callback."}})},
                      {"acts_as", mcp::enum_property("Whether the client acts as the person or as an agent.",
                                                {"person", "agent"})}},
                     {"tenant", "client_id", "name", "redirect_uris", "acts_as"}),
       [this, known_tenant, client_json](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
         identity::Client c;
         c.client_id = arg(args, "client_id").value_or("");
         c.name = arg(args, "name").value_or("");
         c.person = arg(args, "acts_as").value_or("") == "person";
         c.source = "operator";
         if (auto* u = args.if_contains("redirect_uris"); u && u->is_array()) {
           for (const auto& v : u->as_array()) {
             if (v.is_string()) c.redirect_uris.emplace_back(v.as_string());
           }
         }
         std::string why;
         if (!identity_->put_client(*tenant, c, base::now_unix_ms(), &why)) return ToolResult::fail(why);
         return ToolResult::ok(client_json(*identity_->client(*tenant, c.client_id)));
       }});
  mcp_->add_tool({"client_remove",
                  "Changing tool. Removes an OAuth client from a tenant's realm, ending every token it holds, at once.",
                  object_schema({{"tenant", realm_prop}, {"client_id", string_property("The client to remove.")}},
                                {"tenant", "client_id"}),
                  [this, known_tenant](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto id = arg(args, "client_id");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    if (!id || !identity_->remove_client(*tenant, *id, base::now_unix_ms())) {
                      return ToolResult::fail("no such client");
                    }
                    return ToolResult::ok(bj::object{{"removed", *id}});
                  }});
  mcp_->add_tool(
      {"agent_token_mint",
       "Changing tool. Mints a long-lived access token for an agent client to act for one person, for agents that send a fixed "
       "bearer header and can't refresh, like Grok's remote MCP tool or a script. Use it as Authorization: Bearer "
       "<token> on https://<domain>/mcp (box tools) and on box routes that allow agents. The person is mailed, sees "
       "it on their account page, and can revoke it there. Prefer OAuth (dynamic registration or the device flow) "
       "for agents that can do it. Returns access_token (shown once; goober keeps only its hash) and expires_at.",
       object_schema({{"tenant", realm_prop},
                      {"client_id", string_property("An agent client from client_put (acts_as agent).")},
                      {"email", string_property("The person the agent acts for; they must have signed in once.")},
                      {"days", mcp::integer_property("How long it lives: 1 to 366 days.")}},
                     {"tenant", "client_id", "email", "days"}),
       [this, known_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
         auto* days = args.if_contains("days");
         if (!days || !days->is_int64() || days->as_int64() < 1 || days->as_int64() > 366) {
           return ToolResult::fail("days is 1 to 366");
         }
         std::string client_id = arg(args, "client_id").value_or(""), email = arg(args, "email").value_or("");
         int64_t now = base::now_unix_ms(), ttl = days->as_int64() * 24 * 3600 * 1000;
         std::string why;
         auto token = identity_->mint_agent_token(*tenant, client_id, email, ttl, now, &why);
         if (!token) return ToolResult::fail(why);
         auto client = identity_->client(*tenant, client_id);
         identity::Mail m{
             email, "An agent can act for you on " + *tenant,
             "An operator just gave the agent " + (client ? client->name : client_id) + " a token to act for you for " +
                 std::to_string(days->as_int64()) +
                 " days.\n\nIf you expected that, there is nothing to do. If not, open your account page and remove it.\n"};
         m.realm = *tenant;
         m.template_name = "agent-token";
         std::string site = *tenant;
         for (const auto& [domain, entry] : config_.get()->domains) {
           if (entry.tenant == *tenant) site = domain;
         }
         m.vars = bj::object{{"client", client ? client->name : client_id}, {"days", days->as_int64()}, {"site", site}};
         send_mail(m);
         return ToolResult::ok(bj::object{{"access_token", *token},
                                          {"expires_at", base::format_utc_ms(now + ttl)},
                                          {"note", "Shown once; goober keeps only its hash."}});
       }});
}

}  // namespace goober::host
