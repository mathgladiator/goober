// route_try: try a box version before it's live (design/machine/005.FirstVersion.md, "Trying a
// version"). One request, as someone or as nobody, against any stored version of a box, dark
// ones included, through the same gate, checks, template, and headers a real request gets. It
// reaches no one: a GET or HEAD runs read-only, as every GET does; any other method runs on
// copies of the tenant's databases, as a job's dry run does, and the copies are thrown away.

#include <sqlite3.h>

#include <filesystem>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"
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
       "Read tool: try a box version before it goes live. Sends one request to an HTTP route of any stored version "
       "of a box (the active one by default, or a version stored dark with version_upload {activate: false}), as "
       "\"signed_out\" (nobody) or as {\"email\": ...} (an account in the domain's realm, looked up, not signed in: "
       "its roles meet the route's requirements exactly as for a real request). The route is found in THAT "
       "version's manifest, and the request goes through what a real one does: the gate (401, or a 303 to sign-in "
       "for a browser's Accept), the declared body, query, and headers, the box, the route's template by Accept, "
       "partials, the ETag, and Vary. It reaches no one and changes no real data: a GET or HEAD runs read-only "
       "against live data, as every GET does; any other method runs on copies of the tenant's databases, thrown "
       "away after (calls to other boxes refused, deliveries kept in the copies, nothing published). Nothing is "
       "cached, and it isn't counted as traffic; the access log marks it \"try\". Answers the status, headers, body "
       "(the first 256 KiB), the box's log lines, the statements run, what each table's grant was used for, and "
       "for a write the rows each table held before and after in the copies and what would have been queued for "
       "delivery (service, method, where to; no secrets). One try at a time per operator. Observers may try GETs "
       "only. Then version_promote to make the version live.",
       mcp::object_schema(
           {{"domain", mcp::string_property("A tenant domain from the host config, like example.com: picks the tenant.")},
            {"box", mcp::string_property("The box, as box_list or version_list names it.")},
            {"version", mcp::string_property("A stored version (version_list); omit for the active one.")},
            {"method", mcp::string_property("get (the default), head, post, put, patch, or delete.")},
            {"path", mcp::string_property("The path with any query, like /orders/42?full=1.")},
            {"headers", mcp::any_property("Request headers, as an object: accept, content-type, if-none-match, and "
                                          "those the route declares; any other is left out, as for a real request.")},
            {"body", mcp::string_property("The request body as text, like a JSON document or a form.")},
            {"as", mcp::any_property("\"signed_out\" (the default), or {\"email\": \"ada@example.com\"}.")}},
           {"domain", "box", "path"}),
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

         std::string method = base::to_lower(text_arg(args, "method").value_or("get"));
         static const std::set<std::string> kMethods = {"get", "head", "post", "put", "patch", "delete"};
         if (!kMethods.count(method)) return ToolResult::fail("method is get, head, post, put, patch, or delete");
         bool read = method == "get" || method == "head";
         // An observer only looks: a write's try runs box code on copies, which is still more than looking.
         if (!read && !op.empty()) {
           auto roles = operator_roles(op);
           bool may = std::find(roles.begin(), roles.end(), "host:owner") != roles.end() ||
                      std::find(roles.begin(), roles.end(), "host:operator") != roles.end();
           if (!roles.empty() && !may) return ToolResult::fail("an observer may try GETs and HEADs only");
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

         // The route, in that version's own manifest.
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

         // Who: nobody, or an account looked up in the realm (never signed in).
         TryRun run;
         std::string as_text = "signed_out";
         if (const auto* as = args.if_contains("as"); as && !(as->is_string() && as->as_string() == "signed_out")) {
           const auto* email = as->is_object() ? as->as_object().if_contains("email") : nullptr;
           if (!email || !email->is_string()) return ToolResult::fail("as is \"signed_out\" or {\"email\": \"...\"}");
           auto user = identity_->active_user(tenant, std::string(email->as_string()));
           if (!user) return ToolResult::fail("no active account " + std::string(email->as_string()) + " in " + tenant + "'s realm");
           run.who = SignedIn{*user, false, user->roles};
           as_text = user->email;
         }

         // The request, as a browser or a client would send it.
         Request req;
         req.method = method;
         req.path = path->path;
         req.query = query;
         req.target = req.path + (query.empty() ? "" : "?" + query);
         req.body = text_arg(args, "body").value_or("");
         req.remote = "route_try";
         uint8_t rnd[8];
         base::os_random_fill(rnd);
         std::string request_id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
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

         // A write's databases: copies of the tenant's, made now and thrown away after.
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
             std::error_code ec;
             fs::remove_all(root, ec);
           }
         } cleanup{read ? fs::path() : copies_root};

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
         if (box->version != (active->boxes.count(box_name) ? active->boxes.at(box_name)->version : "")) {
           line["try_version"] = box->version;
         }
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
           out["note"] = "a write runs on copies of the tenant's databases, thrown away now: no real data changed and "
                         "nothing was delivered. changes compares row counts, so an update shows under used, not here.";
         } else {
           out["note"] = "a GET or HEAD runs read-only against live data, as every GET does: nothing changed.";
         }
         return ToolResult::ok(std::move(out));
       }});
}

}  // namespace goober::host
