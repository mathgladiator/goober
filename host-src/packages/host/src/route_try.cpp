// route_try: try a box version before it's live (design/machine/005.FirstVersion.md, "Trying a
// version"). One call, as someone or as nobody, to an HTTP route, an MCP tool, or a mail route of
// any stored version of a box, dark ones included, through the same checks a real call gets. It
// reaches no one: an HTTP GET or HEAD runs read-only, as every GET does; anything else runs on
// copies of the tenant's databases, as a job's dry run does, and the copies are thrown away.

#include <sqlite3.h>

#include <filesystem>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"
#include "goober/mail_inbound/mime.h"
#include "goober/router/table.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;

namespace {

constexpr size_t kMaxBody = 256 * 1024;

std::optional<std::string> text_arg(const bj::object& args, const char* key) {
  const auto* v = args.if_contains(key);
  if (!v || !v->is_string()) return std::nullopt;
  return std::string(v->as_string());
}

// Rows per table in every database under a copy's root, by "database/table".
std::map<std::string, int64_t> row_counts(const fs::path& root) {
  std::map<std::string, int64_t> out;
  std::error_code ec;
  if (!fs::exists(root, ec)) return out;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->path().extension() != ".db") continue;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(it->path().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      sqlite3_close(db);
      continue;
    }
    std::vector<std::string> tables;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' "
                               "ESCAPE '\\' AND name NOT LIKE 'goober\\_%' ESCAPE '\\'",
                           -1, &s, nullptr) == SQLITE_OK) {
      while (sqlite3_step(s) == SQLITE_ROW) tables.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)));
    }
    sqlite3_finalize(s);
    for (const auto& t : tables) {
      std::string q = "SELECT count(*) FROM \"" + t + "\"";
      if (sqlite3_prepare_v2(db, q.c_str(), -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) {
        out[it->path().stem().string() + "/" + t] = sqlite3_column_int64(s, 0);
      }
      sqlite3_finalize(s);
    }
    sqlite3_close(db);
  }
  return out;
}

// What a write would have queued for delivery, from the copies' outboxes: the service and
// method, the verb, where it would go (scheme, host, and path; never the query), and the body's
// size. No credentials, headers, or bodies.
bj::array outbox_in(const fs::path& root) {
  bj::array out;
  std::error_code ec;
  if (!fs::exists(root, ec)) return out;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->path().extension() != ".db") continue;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(it->path().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      sqlite3_close(db);
      continue;
    }
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT service, method, verb, url, length(body) FROM goober_outbox ORDER BY id", -1, &s,
                           nullptr) == SQLITE_OK) {
      while (sqlite3_step(s) == SQLITE_ROW) {
        auto col = [&](int i) {
          const unsigned char* t = sqlite3_column_text(s, i);
          return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
        };
        std::string url = col(3);
        url = url.substr(0, url.find_first_of("?#"));
        out.push_back(bj::object{{"service", col(0)}, {"method", col(1)}, {"verb", col(2)}, {"to", url},
                                 {"body_bytes", sqlite3_column_int64(s, 4)}});
      }
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
  }
  return out;
}

}  // namespace

void Host::register_try_tools() {
  mcp_->add_tool(
      {"route_try",
       "Read tool: try a box version before it goes live. One call to any stored version of a box (the active one by "
       "default, or a version stored dark with version_upload {activate: false}), found in THAT version's manifest, "
       "and run through what a real call does. Give exactly one of: path (an HTTP route: method, headers, body; the "
       "gate's 401 or 303, the declared inputs, the template by Accept, partials, ETag, Vary), tool (an MCP tool, with "
       "arguments checked against its input type, exactly as the site's /mcp would), or mail (a mail route: {to, from, "
       "subject, text, headers?}, built into the message the MTA hook would hand over, and answered with the SMTP "
       "reply the sender would get). as is \"signed_out\" (nobody; the default, and the only way mail arrives) or "
       "{\"email\": ...}, an account in the realm, looked up and never signed in, whose roles meet the requirements "
       "exactly as for a real call; a tool always acts for somebody. A try's mail sender is never verified: verifying "
       "takes a real DKIM signature, so from_user_id is null and sender_why says why. It reaches no one and changes no "
       "real data: an HTTP GET or HEAD runs read-only against live data, as every GET does; anything else (writes, "
       "tools, mail) runs on copies of the tenant's databases, thrown away after (calls to other boxes refused, "
       "deliveries kept in the copies, nothing published). Nothing is cached, it isn't counted as traffic, and the "
       "access log marks it \"try\". Answers the status (or the tool's result, or the SMTP reply), the box's log "
       "lines, the statements run, what each table's grant was used for, and for a run on copies the rows each table "
       "held before and after and what would have been queued for delivery (service, method, where to; no secrets). "
       "One try at a time per operator. Observers, and the panel (look-only), may try HTTP GETs and HEADs only; a try "
       "never waits for approval, since it changes nothing real. Then version_promote to make the version live.",
       mcp::object_schema(
           {{"domain", mcp::string_property("A tenant domain from the host config, like example.com: picks the tenant.")},
            {"box", mcp::string_property("The box, as box_list or version_list names it.")},
            {"version", mcp::string_property("A stored version (version_list); omit for the active one.")},
            {"path", mcp::string_property("An HTTP route: the path with any query, like /orders/42?full=1.")},
            {"method", mcp::string_property("For path: get (the default), head, post, put, patch, or delete.")},
            {"headers", mcp::any_property("For path: request headers, as an object: accept, content-type, "
                                          "if-none-match, and those the route declares; any other is left out, as "
                                          "for a real request.")},
            {"body", mcp::string_property("For path: the request body as text, like a JSON document or a form.")},
            {"tool", mcp::string_property("An MCP tool's name (routes.mcp), instead of path.")},
            {"arguments", mcp::any_property("For tool: its arguments, as an object.")},
            {"mail", mcp::any_property("A mail route, instead of path: {\"to\": \"support@example.com\", \"from\": "
                                       "\"Ada <ada@example.org>\", \"subject\": \"...\", \"text\": \"...\", "
                                       "\"headers\"?: {\"In-Reply-To\": \"<...>\"}}.")},
            {"as", mcp::any_property("\"signed_out\" (the default), or {\"email\": \"ada@example.com\"}.")}},
           {"domain", "box"}),
       [this](const bj::object& args) {
         std::string op = current_operator();
         {
           std::lock_guard lock(trying_mu_);
           if (!trying_.insert(op).second) return ToolResult::fail("a route_try of yours is still running; one at a time");
         }
         struct Done {
           Host* host;
           std::string op;
           ~Done() {
             std::lock_guard lock(host->trying_mu_);
             host->trying_.erase(op);
           }
         } done{this, op};

         bool http_try = args.contains("path"), tool_try = args.contains("tool"), mail_try = args.contains("mail");
         if (int(http_try) + int(tool_try) + int(mail_try) != 1) {
           return ToolResult::fail("give exactly one of path (an HTTP route), tool (an MCP tool), or mail (a mail route)");
         }
         std::string method = base::to_lower(text_arg(args, "method").value_or("get"));
         static const std::set<std::string> kMethods = {"get", "head", "post", "put", "patch", "delete"};
         if (http_try && !kMethods.count(method)) return ToolResult::fail("method is get, head, post, put, patch, or delete");
         // Only an HTTP GET or HEAD runs on live data; a tool or a mail route may write, so it runs on copies.
         bool read = http_try && (method == "get" || method == "head");
         // An observer only looks: a try on copies runs box code that may write, which is more than looking.
         if (!read && !op.empty()) {
           auto roles = operator_roles(op);
           bool may = std::find(roles.begin(), roles.end(), "host:owner") != roles.end() ||
                      std::find(roles.begin(), roles.end(), "host:operator") != roles.end();
           if (!roles.empty() && !may) return ToolResult::fail("an observer may try HTTP GETs and HEADs only");
         }

         auto cfg = config_.get();
         auto resolved = cfg->resolve_host(text_arg(args, "domain").value_or(""));
         if (!resolved) return ToolResult::fail("domain isn't a tenant domain in the host config (domain_list)");
         std::string domain = resolved->first, tenant = resolved->second.tenant;

         // The version: the active one, or any stored one.
         std::string box_name = text_arg(args, "box").value_or("");
         std::string version = text_arg(args, "version").value_or("");
         auto active = boxes_->active();
         std::shared_ptr<const boxes::ActiveBox> box;
         if (auto it = active->boxes.find(box_name); it != active->boxes.end() && (version.empty() || version == it->second->version)) {
           box = it->second;
         } else if (version.empty()) {
           return ToolResult::fail("box " + box_name + " has no active version: name one (version_list)");
         } else {
           std::string err;
           box = boxes_->load(box_name, version, &err);
           if (!box) return ToolResult::fail("can't load " + box_name + " version " + version + ": " + err);
         }
         if (!box->manifest.applies_to(domain)) {
           return ToolResult::fail(box_name + " version " + box->version + " doesn't serve " + domain + " (its domains)");
         }
         bool active_version = active->boxes.count(box_name) && active->boxes.at(box_name)->version == box->version;

         // Who: nobody, or an account looked up in the realm (never signed in).
         TryRun run;
         std::string as_text = "signed_out";
         if (const auto* as = args.if_contains("as"); as && !(as->is_string() && as->as_string() == "signed_out")) {
           const auto* email = as->is_object() ? as->as_object().if_contains("email") : nullptr;
           if (!email || !email->is_string()) return ToolResult::fail("as is \"signed_out\" or {\"email\": \"...\"}");
           if (mail_try) return ToolResult::fail("mail always arrives as nobody: a mail route's caller is the sender, not an account");
           auto user = identity_->active_user(tenant, std::string(email->as_string()));
           if (!user) return ToolResult::fail("no active account " + std::string(email->as_string()) + " in " + tenant + "'s realm");
           run.who = SignedIn{*user, false, user->roles};
           as_text = user->email;
         }

         uint8_t rnd[8];
         base::os_random_fill(rnd);
         std::string request_id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));

         // Anything but a GET or HEAD runs on copies of the tenant's databases, made now and thrown away after.
         fs::path copies_root = cfg->storage / "tmp" / "tries" / request_id;
         std::unique_ptr<db::Databases> copies;
         std::map<std::string, int64_t> before;
         if (!read) {
           copies = std::make_unique<db::Databases>(copies_root);
           copies->set_id_source(next_id_);
           for (const auto& d : box->manifest.data) {
             if (!d.applies_schema()) continue;
             std::string t = d.tenant_for(tenant), database = d.database_for(tenant);
             if (!databases_->exists(t, database) || copies->exists(t, database)) continue;
             std::error_code ec;
             fs::create_directories(copies->file(t, database).parent_path(), ec);
             std::string err;
             if (!copy_database(databases_->file(t, database), copies->file(t, database), &err)) {
               fs::remove_all(copies_root, ec);
               return ToolResult::fail(err);
             }
           }
           before = row_counts(copies_root);
           run.copies = copies.get();
         }
         struct Cleanup {
           fs::path root;
           ~Cleanup() {
             if (root.empty()) return;
             std::error_code ec;
             fs::remove_all(root, ec);
           }
         } cleanup{read ? fs::path() : copies_root};

         // What every try answers beyond its own result: the box's log, its statements, its use
         // of each grant, and on copies what changed and what would have been queued.
         auto details = [&](bj::object& out) {
           bj::array logs;
           for (const auto& l : run.logs) logs.push_back(bj::object{{"level", l.level}, {"text", l.text}});
           out["logs"] = std::move(logs);
           if (const auto* sql = run.extra.if_contains("sql")) out["statements"] = *sql;
           bj::object used;
           for (const auto& [strategy, tables] : run.used) {
             bj::object per;
             for (const auto& [t, u] : tables) {
               bj::array r(u.read.begin(), u.read.end()), up(u.update.begin(), u.update.end());
               per[t] = bj::object{{"read", std::move(r)}, {"update", std::move(up)}, {"insert", u.insert}, {"delete", u.remove}};
             }
             used[strategy] = std::move(per);
           }
           out["used"] = std::move(used);
           if (!read) {
             auto after = row_counts(copies_root);
             bj::object changes;
             std::set<std::string> names;
             for (const auto& [k, n] : before) names.insert(k);
             for (const auto& [k, n] : after) names.insert(k);
             for (const auto& k : names) {
               int64_t b = before.count(k) ? before.at(k) : 0, a = after.count(k) ? after.at(k) : 0;
               if (a != b || !before.count(k)) changes[k] = bj::object{{"rows_before", b}, {"rows_after", a}};
             }
             out["changes"] = std::move(changes);
             out["would_queue"] = outbox_in(copies_root);
             out["note"] = "this ran on copies of the tenant's databases, thrown away now: no real data changed and "
                           "nothing was delivered. changes compares row counts, so an update shows under used, not here.";
           } else {
             out["note"] = "a GET or HEAD runs read-only against live data, as every GET does: nothing changed.";
           }
         };
         // A tool's or a mail route's call, run on the copies, as a dry run runs: no calls to other
         // boxes, nothing published, nothing delivered, and kept out of the statistics.
         auto call_on_copies = [&](const boxes::RouteRef& ref, const bj::object& doc, std::optional<int64_t> timeout_ms,
                                   bool no_response_ok) {
           CallOptions options;
           options.timeout_ms = timeout_ms;
           options.no_response_ok = no_response_ok;
           options.databases = copies.get();
           options.dry_run = true;
           auto call = call_box(ref, doc, tenant, request_id, run.who, options);
           run.logs = call.result.logs;
           run.extra = call.extra;
           run.used = call.used;
           return call;
         };
         // The try's own access-log line: marked, by whom, and on which version.
         auto log_try = [&](const std::string& what, int status, int64_t started, bj::object line) {
           server::http::Request logged;
           logged.method = what;
           logged.path = what + ":" + text_arg(args, what == "tool" ? "tool" : "box").value_or("");
           logged.headers.emplace_back("goober-request-id", request_id);
           line["try"] = true;
           line["operator"] = op;
           if (!active_version) line["try_version"] = box->version;
           access_log(logged, status, started, domain, json::dump(line));
         };

         if (tool_try) {
           std::string name = text_arg(args, "tool").value_or("");
           const manifest::HttpRoute* route = nullptr;
           for (const auto& t : box->manifest.tools) {
             if (t.name == name) route = &t;
           }
           if (!route) return ToolResult::fail("version " + box->version + " has no tool " + name + " (routes.mcp)");
           if (!run.who) return ToolResult::fail("a tool always acts for somebody: pass as {\"email\": ...}");
           manifest::Caller caller{std::to_string(run.who->user.user_id), false, run.who->roles};
           bj::object out{{"box", box_name}, {"version", box->version}, {"tool", name}, {"as", as_text}};
           if (manifest::check_access(*route, &caller, {}, tenant) != manifest::Access::kAllowed) {
             out["listed"] = false;
             out["note"] = "this account's roles don't meet the tool's requirements: the site's /mcp wouldn't list it, "
                           "and a call would be refused. The box didn't run.";
             return ToolResult::ok(std::move(out));
           }
           bj::object given;
           if (const auto* a = args.if_contains("arguments"); a && a->is_object()) given = a->as_object();
           else if (a && !a->is_null()) return ToolResult::fail("arguments is an object");
           bj::value input = given;
           std::string input_type = types::json_type_name(route->input);
           if (!input_type.empty()) {
             std::string err;
             auto checked = box->types ? box->types->check(input_type, given, &err) : std::nullopt;
             if (!checked) {
               out["error"] = input_type + " " + (box->types ? err : "has no types document");
               out["note"] = "the arguments don't match the tool's input type; the site's /mcp refuses them the same "
                             "way, before the box runs";
               return ToolResult::ok(std::move(out));
             }
             input = std::move(*checked);
           } else if (!given.empty()) {
             out["error"] = "this tool takes no input";
             return ToolResult::ok(std::move(out));
           }
           bj::object doc{{"request_id", request_id}, {"method", "tool"},     {"path", "mcp:" + name},
                          {"params", bj::object{}},   {"query", bj::object{}}, {"headers", bj::object{}},
                          {"body", json::dump(input)}, {"domain", domain},    {"tenant", tenant},
                          {"user", user_json(run.who)}};
           int64_t started = base::monotonic_us();
           auto call = call_on_copies(boxes::RouteRef{box, route}, doc, route->timeout_ms, false);
           const auto& r = call.result;
           bool ok = r.outcome == runtime::wasm::CallResult::Outcome::kOk;
           out["status"] = ok ? r.status : 500;
           out["outcome"] = runtime::wasm::outcome_name(r.outcome);
           // What the agent would see, by the site's /mcp's rules (050 § The result vocabulary).
           std::string err;
           auto parsed = r.body.empty() ? std::optional<bj::value>(bj::object{}) : json::parse(r.body, &err);
           if (!ok || r.status >= 500) {
             out["is_error"] = true;
             out["result"] = "the tool failed; request " + request_id;
           } else if (r.status >= 400) {
             out["is_error"] = true;
             std::string message = r.body;
             if (parsed && parsed->is_object() && parsed->as_object().contains("error") &&
                 parsed->as_object().at("error").is_string()) {
               message = std::string(parsed->as_object().at("error").as_string());
             }
             out["result"] = message.empty() ? "refused (" + std::to_string(r.status) + ")" : message;
           } else {
             out["is_error"] = false;
             out["result"] = parsed ? *parsed : bj::value(r.body);
           }
           log_try("tool", ok ? r.status : 500, started, bj::object{{"tool", name}, {"box", box_name}});
           details(out);
           return ToolResult::ok(std::move(out));
         }

         if (mail_try) {
           const auto* m = args.if_contains("mail");
           if (!m || !m->is_object()) return ToolResult::fail("mail is {\"to\", \"from\", \"subject\", \"text\", \"headers\"?}");
           const auto& mo = m->as_object();
           auto field = [&](const char* k) {
             const auto* v = mo.if_contains(k);
             return v && v->is_string() ? std::string(v->as_string()) : std::string();
           };
           std::string to = base::to_lower(field("to")), from_header = field("from");
           if (to.empty() || from_header.empty()) return ToolResult::fail("mail needs to and from");
           bj::object out{{"box", box_name}, {"version", box->version}, {"to", to}};
           // The hook's own matching, in this version alone: a forged or expired reply token is
           // refused here as it would be at RCPT.
           std::string why;
           auto match = match_mail(to, &why, box);
           if (!match) {
             out["outcome"] = "no_route";
             out["why"] = why;
             out["smtp"] = bj::object{{"status", 550}, {"enhanced_status", "5.1.1"}, {"message", "No such recipient here"}};
             out["note"] = "refused at RCPT: the box didn't run";
             return ToolResult::ok(std::move(out));
           }
           if (match->tenant != tenant) {
             return ToolResult::fail(to + " belongs to tenant " + match->tenant + ", not " + domain + "'s");
           }
           const auto& route = *match->ref.route;
           out["route"] = route.name;
           // The message as a sender would write it: the given headers, then those every message has.
           mail::inbound::Headers headers;
           if (const auto* hs = mo.if_contains("headers"); hs && hs->is_object()) {
             for (const auto& kv : hs->as_object()) {
               if (!kv.value().is_string()) continue;
               std::string k(kv.key());
               std::string lower = base::to_lower(k);
               if (lower == "from" || lower == "to" || lower == "subject" || lower == "message-id" || lower == "date") continue;
               std::string v(kv.value().as_string());
               if (v.find_first_of("\r\n") != std::string::npos || k.find_first_of(":\r\n ") != std::string::npos) continue;
               headers.emplace_back(k, v);
             }
           }
           if (from_header.find_first_of("\r\n") != std::string::npos) return ToolResult::fail("from is one line");
           std::string subject = field("subject");
           for (auto& c : subject) {
             if (c == '\r' || c == '\n') c = ' ';
           }
           headers.emplace_back("From", from_header);
           headers.emplace_back("To", to);
           headers.emplace_back("Subject", subject);
           headers.emplace_back("Date", base::format_utc_ms(base::now_unix_ms()));
           headers.emplace_back("Message-ID", "<try." + request_id + "@" + domain + ">");
           headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
           std::string body = field("text");
           auto message = mail::inbound::parse_message(headers, body);
           std::string envelope = message.from ? base::to_lower(message.from->address) : "";
           if (envelope.empty()) return ToolResult::fail("from must hold an address, like Ada <ada@example.org>");
           int64_t size = int64_t(body.size());
           for (const auto& [k, v] : headers) size += int64_t(k.size() + v.size() + 4);
           auto rejected = [&](int status, const std::string& enhanced, const std::string& message_text, const char* outcome) {
             out["outcome"] = outcome;
             out["smtp"] = bj::object{{"status", status}, {"enhanced_status", enhanced}, {"message", message_text}};
             out["note"] = "the hook answers this before the box runs";
             return ToolResult::ok(std::move(out));
           };
           if (size > std::min<int64_t>(*route.max_body_bytes, cfg->mail.inbound.max_bytes)) {
             return rejected(552, "5.3.4", "Message too big for this address", "too_big");
           }
           bool automatic = mail::inbound::is_automatic(headers, envelope);
           if (automatic && !route.mail_automatic) {
             out["outcome"] = "automatic_dropped";
             out["smtp"] = bj::object{{"status", 250}, {"enhanced_status", "2.0.0"}, {"message", "Delivered"}};
             out["note"] = "an automatic message (a bounce, an auto-reply) is taken and dropped: the box didn't run";
             return ToolResult::ok(std::move(out));
           }
           bj::object log;
           auto doc = mail_route_doc(*match, headers, body, message, envelope, size, automatic, bj::array{}, request_id, &log);
           int64_t started = base::monotonic_us();
           auto call = call_on_copies(match->ref, doc, route.timeout_ms, true);
           std::string outcome;
           auto reply = mail_hook_reply(call.result, &outcome);
           out["outcome"] = outcome;
           out["box_outcome"] = runtime::wasm::outcome_name(call.result.outcome);
           out["status"] = call.result.status;
           out["smtp"] = reply.contains("response") ? reply.at("response") : bj::value(nullptr);
           out["sender_verified"] = false;
           out["sender_why"] = log.contains("sender_verified") && log.at("sender_verified").as_bool() ? bj::value(nullptr)
                                                                                                       : bj::value("a try's mail carries no DKIM signature, so its sender is never verified");
           log_try("receive", call.result.status ? call.result.status : 200, started,
                   bj::object{{"box", box_name}, {"route", route.name}, {"mail_try", outcome}});
           details(out);
           return ToolResult::ok(std::move(out));
         }

         // An HTTP route, in that version's own manifest.
         std::string target = text_arg(args, "path").value_or("");
         std::string query;
         if (auto q = target.find('?'); q != std::string::npos) {
           query = target.substr(q + 1);
           target.resize(q);
         }
         auto path = router::normalize_path(target);
         if (!path) return ToolResult::fail("not a path the host would accept");
         router::RouteTable<boxes::RouteRef> table;
         for (const auto& r : box->manifest.routes) table.add(r.pattern, r.verb, boxes::RouteRef{box, &r});
         auto match = table.match(path->segments, method == "head" ? "get" : method);
         if (!match.value) {
           return ToolResult::fail(match.method_not_allowed
                                       ? "version " + box->version + " has a route at " + path->path + ", but not for " + method
                                       : "version " + box->version + " has no route at " + path->path);
         }
         const auto& route = *match.value->route;

         // The request, as a browser or a client would send it.
         Request req;
         req.method = method;
         req.path = path->path;
         req.query = query;
         req.target = req.path + (query.empty() ? "" : "?" + query);
         req.body = text_arg(args, "body").value_or("");
         req.remote = "route_try";
         req.headers.emplace_back("goober-request-id", request_id);
         req.headers.emplace_back("host", domain);
         if (const auto* h = args.if_contains("headers"); h && h->is_object()) {
           for (const auto& kv : h->as_object()) {
             std::string k = base::to_lower(kv.key());
             if (!kv.value().is_string()) continue;
             bool allowed = k == "accept" || k == "content-type" || k == "if-none-match" ||
                            std::find(route.request_headers.begin(), route.request_headers.end(), k) != route.request_headers.end();
             if (allowed) req.headers.emplace_back(k, std::string(kv.value().as_string()));
           }
         }

         int64_t started = base::monotonic_us();
         Response res;
         std::string extra_text;
         serve_box(req, [&](Response r, std::string extra) {
                     res = std::move(r);
                     extra_text = std::move(extra);
                   },
                   *match.value, match.params, domain, tenant, nullptr, 0, &run);
         // The access log says it was a try, and by whom, whether the box ran or the gate answered.
         bj::object line;
         std::string err;
         if (auto v = json::parse(extra_text, &err); v && v->is_object()) line = v->as_object();
         line["try"] = true;
         line["operator"] = op;
         if (!active_version) line["try_version"] = box->version;
         access_log(req, res.status, started, domain, json::dump(line));

         bj::object out{{"box", box_name},       {"version", box->version}, {"route", route.name},
                        {"method", method},      {"path", req.target},     {"as", as_text},
                        {"status", res.status}};
         bj::array headers;
         for (const auto& [k, v] : res.headers) headers.push_back(bj::array{bj::value(k), bj::value(v)});
         out["headers"] = std::move(headers);
         std::string body = method == "head" ? "" : res.body;
         if (body.size() > kMaxBody) {
           out["body_truncated"] = int64_t(body.size());
           body.resize(kMaxBody);
         }
         if (base::valid_utf8(body) == body) {
           out["body"] = body;
         } else {
           out["body_base64"] = base::base64_encode(body);
         }
         details(out);
         return ToolResult::ok(std::move(out));
       }});
}

}  // namespace goober::host
