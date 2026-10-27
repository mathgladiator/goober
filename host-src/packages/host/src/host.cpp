#include "goober/host/host.h"

#include <algorithm>
#include <ctime>
#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/json/json.h"
#include "goober/router/path.h"
#include "http_util.h"

namespace goober::host {
namespace bj = boost::json;
using server::http::Request;
using server::http::Responder;
using server::http::Response;

namespace {

std::string new_request_id() {
  uint8_t bytes[8];
  base::os_random_fill(bytes);
  return base::hex_encode(std::string_view(reinterpret_cast<char*>(bytes), sizeof(bytes)));
}

const char* reason(int status) {
  switch (status) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return status >= 500 ? "Server Error" : "Client Error";
  }
}

// True when the caller would rather have HTML than JSON: text/html is listed, and
// before application/json if both are.
bool prefers_html(std::string_view accept) {
  auto html = accept.find("text/html");
  if (html == std::string_view::npos) return false;
  auto json = accept.find("application/json");
  return json == std::string_view::npos || html < json;
}

// Request headers a box never sees: credentials belong to the host.
bool hidden_from_box(std::string_view name) { return name == "authorization" || name == "cookie"; }

}  // namespace

Host::Host(std::shared_ptr<const config::HostConfig> config, std::filesystem::path config_file)
    : config_file_(std::move(config_file)), config_(config) {
  started_ms_ = base::now_unix_ms();
  wal_ = std::make_unique<wal::HostWal>(config->storage);
  const auto& rec = wal_->recovery();
  if (rec.replayed_bundles || rec.discarded_bundles || rec.torn_tail) {
    startup_notes_.push_back("host WAL recovery: replayed " + std::to_string(rec.replayed_bundles) +
                             ", discarded " + std::to_string(rec.discarded_bundles) +
                             (rec.torn_tail ? ", torn tail dropped" : ""));
  }
  content_ = std::make_unique<content::ContentStore>(*wal_);
  site_ = std::make_unique<site::Site>(*content_);
  attachments_ = std::make_unique<attachments::Store>(*wal_);
  schemas_ = std::make_unique<db::SchemaStore>(*wal_);
  databases_ = std::make_unique<db::Databases>(config->storage);
  engine_ = std::make_unique<runtime::wasm::Engine>();
  boxes_ = std::make_unique<boxes::BoxRegistry>(*wal_, *engine_);
  boxes_->set_manifest_check([this](const manifest::Manifest& m, std::string* error) { return check_data(m, error); });
  identity_ = std::make_unique<identity::Identity>(config->storage / "identity",
                                                   [this](const identity::Mail& m) { send_mail(m); });
  for (auto& w : boxes_->load_all()) startup_notes_.push_back(std::move(w));
  mcp_ = std::make_unique<mcp::McpServer>(
      "goober-operations", std::string(kVersion),
      "Operate a goober host. Build sites as a CMS: call site_guide first, then write pages (page_put, as "
      "markdown, an HTML fragment in a template, or a whole HTML document), templates (template_put), and images "
      "(attachment_put, from base64 or a URL). Deploy WASM boxes (version_upload), query tenant databases "
      "(db_list, db_query), and read logs.");
  register_tools();
  register_content_tools();
  register_data_tools();
  pool_ = std::make_unique<WorkerPool>(config->box_threads, config->pin_boxes);
}

Host::~Host() { stop(); }

uint16_t Host::start() {
  auto cfg = config_.get();
  server::http::ServerOptions opts;
  opts.address = cfg->listen_address;
  opts.port = cfg->listen_port;
  opts.threads = cfg->network_threads;
  opts.body_max_bytes = uint64_t(cfg->body_max_bytes);
  server_ = std::make_unique<server::http::Server>(
      opts, [this](Request req, Responder respond) { handle(std::move(req), std::move(respond)); });
  server_->start();
  return server_->port();
}

void Host::stop() {
  if (server_) server_->stop();
  server_.reset();
}

std::string Host::reload() {
  if (config_file_.empty()) return "no config file to reload";
  std::string err;
  auto next = config::load_config(config_file_, &err);
  if (!next) return "reload refused: " + err;
  if (config_.get()->local) next = config::local_mode(next);
  std::string notes = config_.replace(next);
  return "config reloaded. " + notes;
}

// The dev mailer (005 §11, Q45): each mail becomes a MIME file under
// storage/mail/outbox/ and a line in logs/mail.log. In local mode it is also
// printed, so the person playing sees their sign-in code in the terminal.
void Host::send_mail(const identity::Mail& mail) {
  auto cfg = config_.get();
  int64_t now = base::now_unix_ms();
  uint8_t rnd[4];
  base::os_random_fill(rnd);
  std::string name = std::to_string(now) + "-" + base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), 4)) + ".eml";
  std::time_t secs = now / 1000;
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char date[64];
  std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &tm);
  std::string eml = "From: goober <no-reply@" + cfg->platform_domain + ">\r\nTo: " + mail.to + "\r\nSubject: " +
                    mail.subject + "\r\nDate: " + date +
                    "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n" + mail.text;
  auto file = cfg->storage / "mail" / "outbox" / name;
  base::write_file_durable(file, eml);
  base::append_line(cfg->storage / "logs" / "mail.log",
                    json::dump(bj::object{{"at", base::format_utc_ms(now)},
                                          {"to", mail.to},
                                          {"subject", mail.subject},
                                          {"file", "mail/outbox/" + name}}));
  if (cfg->local) {
    std::string first = mail.text.substr(0, mail.text.find('\n'));
    std::cerr << "goober-host: mail to " << mail.to << ": " << first << "\n";
  }
}

void Host::access_log(const Request& req, int status, int64_t started_us, const std::string& domain,
                      const std::string& extra) {
  bj::object line{{"at", base::format_utc_ms(base::now_unix_ms())},
                  {"request_id", req.header("goober-request-id")},
                  {"domain", domain},
                  {"method", req.method},
                  {"path", req.path},
                  {"status", status},
                  {"us", base::monotonic_us() - started_us}};
  if (!extra.empty()) {
    std::string err;
    if (auto v = json::parse(extra, &err); v && v->is_object()) {
      for (auto& kv : v->get_object()) line[kv.key()] = kv.value();
    }
  }
  base::append_line(config_.get()->storage / "logs" / "access.log", json::dump(line));
}

Response Host::error_page(const std::string& tenant, int status, const std::string& request_id) const {
  Response res;
  res.status = status;
  std::string file = "error-message/" + std::to_string(status) + ".html";
  std::optional<content::File> page;
  if (!tenant.empty()) page = content_->get(tenant, file);
  if (!page) page = content_->get(content::kHostTree, file);
  res.headers.emplace_back("content-type", "text/html; charset=utf-8");
  res.headers.emplace_back("cache-control", "no-store");
  res.headers.emplace_back("x-content-type-options", "nosniff");
  if (page) {
    res.body = std::move(page->data);
  } else if (status == 401) {
    res.body = "<!doctype html><html><head><meta charset=\"utf-8\"><title>Uh Oh!</title>"
               "<style>body{font-family:system-ui,sans-serif;max-width:40rem;margin:4rem auto;padding:0 1rem}"
               "code{color:#666}</style></head><body><h1>Uh Oh!</h1><p>401 Unauthorized: this page needs you to "
               "<a href=\"/identity/sign-in\">sign in</a>.</p><p><code>request " + request_id +
               "</code></p></body></html>\n";
  } else {
    res.body = "<!doctype html><html><head><meta charset=\"utf-8\"><title>Uh Oh!</title>"
               "<style>body{font-family:system-ui,sans-serif;max-width:40rem;margin:4rem auto;padding:0 1rem}"
               "code{color:#666}</style></head><body><h1>Uh Oh!</h1><p>" +
               std::to_string(status) + " " + reason(status) + "</p><p><code>request " + request_id +
               "</code></p></body></html>\n";
  }
  return res;
}

void Host::handle(Request req, Responder respond) {
  ++requests_;
  int64_t started = base::monotonic_us();
  std::string request_id = new_request_id();
  req.headers.erase(std::remove_if(req.headers.begin(), req.headers.end(),
                                   [](const auto& h) { return h.first == "goober-request-id"; }),
                    req.headers.end());
  req.headers.emplace_back("goober-request-id", request_id);
  auto cfg = config_.get();
  std::string host_header = base::to_lower(req.header("host"));
  auto resolved = cfg->resolve_host(host_header);
  // The configured domain (local aliases resolve to it), or the raw host for the log.
  std::string domain = resolved ? resolved->first
                                : host_header.substr(0, host_header.rfind(':') == std::string::npos
                                                            ? host_header.size()
                                                            : host_header.rfind(':'));

  // Every response is logged and stamped with its request id.
  Finish finish = [this, req, respond, started, domain, request_id](Response res, std::string extra) {
    res.headers.emplace_back("goober-request-id", request_id);
    access_log(req, res.status, started, domain, extra);
    respond(std::move(res));
  };

  auto path = router::normalize_path(req.path);
  if (path && path->path == kOperationsMcpPath) {
    pool_->post([this, req, finish] { handle_operations(req, finish); });
    return;
  }
  if (!resolved) return finish(error_page("", 404, request_id), R"({"why":"unknown domain"})");
  auto tenant = std::optional<std::string>(resolved->second.tenant);
  if (!path) return finish(error_page(*tenant, 400, request_id), R"({"why":"bad path"})");
  if (path->path.starts_with("/identity/")) {
    pool_->post([this, req, finish, p = path->path, domain, tenant = *tenant] {
      try {
        handle_identity(req, finish, p, domain, tenant);
      } catch (const std::exception& e) {
        std::cerr << "goober-host: identity: " << e.what() << "\n";
        finish(error_page(tenant, 500, req.header("goober-request-id")), R"({"why":"identity failed"})");
      }
    });
    return;
  }

  if (path->path == "/attachment" || path->path.starts_with("/attachment/")) {
    pool_->post([this, req, finish, p = path->path, tenant = *tenant] {
      try {
        handle_attachment(req, finish, p, tenant);
      } catch (const std::exception& e) {
        std::cerr << "goober-host: attachment: " << e.what() << "\n";
        finish(error_page(tenant, 500, req.header("goober-request-id")), R"({"why":"attachment failed"})");
      }
    });
    return;
  }

  auto active = boxes_->active();
  const auto& table = active->table_for(domain);
  std::string verb = req.method == "head" ? "get" : req.method;
  auto match = table.match(path->segments, verb);
  if (match.value) {
    auto ref = *match.value;
    pool_->post([this, req = std::move(req), finish, ref, params = std::move(match.params), domain,
                 tenant = *tenant, active]() mutable {
      serve_box(std::move(req), finish, ref, std::move(params), domain, tenant, active);
    });
    return;
  }
  if (match.method_not_allowed || (req.method != "get" && req.method != "head")) {
    return finish(error_page(*tenant, 405, request_id), "");
  }
  pool_->post([this, req, finish, tenant = *tenant, p = path->path] {
    try {
      serve_content(req, finish, tenant, p);
    } catch (const std::exception& e) {
      std::cerr << "goober-host: content: " << e.what() << "\n";
      finish(error_page(tenant, 500, req.header("goober-request-id")), R"({"why":"content failed"})");
    }
  });
}

std::optional<SignedIn> Host::caller(const Request& req, const std::string& tenant) { return signed_in(req, tenant); }

bj::value Host::user_json(const std::optional<SignedIn>& who) {
  if (!who) return nullptr;
  bj::array roles;
  for (const auto& r : who->roles) roles.push_back(bj::value(r));
  return bj::object{{"user_id", std::to_string(who->user.user_id)},
                    {"email", who->user.email},
                    {"roles", std::move(roles)},
                    {"host", who->host}};
}

int Host::authorize(const bj::value& requirements, const std::optional<SignedIn>& who, const std::string& tenant) {
  manifest::HttpRoute route;
  std::string why;
  // A page's requirements may substitute {!tenant} and {!user_id}; a malformed list denies everyone.
  if (!manifest::parse_requirements(requirements, router::RoutePattern{}, &route.requirements, &why)) return 403;
  std::optional<manifest::Caller> c;
  if (who) c = manifest::Caller{std::to_string(who->user.user_id), who->host, who->roles};
  switch (manifest::check_access(route, c ? &*c : nullptr, {}, tenant)) {
    case manifest::Access::kAllowed: return 200;
    case manifest::Access::kSignIn: return 401;
    case manifest::Access::kForbidden: return 403;
    case manifest::Access::kBadValue: return 400;
  }
  return 403;
}

Response Host::sign_in_or_401(const Request& req, const std::string& tenant) const {
  if ((req.method == "get" || req.method == "head") && req.header("accept").find("text/html") != std::string::npos) {
    Response res;
    res.status = 303;
    std::string back = req.path + (req.query.empty() ? "" : "?" + req.query);
    res.headers.emplace_back("location", "/identity/sign-in?return=" + base::percent_encode(back));
    res.headers.emplace_back("cache-control", "no-store");
    return res;
  }
  return error_page(tenant, 401, req.header("goober-request-id"));
}

void Host::serve_content(const Request& req, const Finish& finish, const std::string& tenant, const std::string& path) {
  std::string request_id = req.header("goober-request-id");
  // Who is asking matters only for pages with requirements, and for templates that greet them.
  std::optional<SignedIn> who = caller(req, tenant);
  site::Visit v;
  v.tenant = tenant;
  v.path = path;
  for (const auto& [k, val] : router::parse_query(req.query)) v.query[k] = val;
  v.accept = req.header("accept");
  v.origin = (config_.get()->identity.cookie_secure ? "https://" : "http://") + req.header("host");
  v.user = user_json(who);
  v.authorize = [&](const bj::value& requirements) { return authorize(requirements, who, tenant); };
  auto r = site_->visit(v);
  bj::object extra{{"content", site::kind_name(r.kind)}};
  if (!r.file.empty()) extra["file"] = r.file;
  if (who) extra["user_id"] = std::to_string(who->user.user_id);
  if (r.status == 401) return finish(sign_in_or_401(req, tenant), json::dump(extra));
  if (r.status >= 400) {
    if (!r.error.empty()) {
      extra["error"] = r.error;
      std::cerr << "goober-host: content " << tenant << path << ": " << r.error << "\n";
    }
    return finish(error_page(tenant, r.status, request_id), json::dump(extra));
  }
  Response res;
  res.status = r.status;
  if (r.status >= 300 && r.status < 400) {
    res.headers.emplace_back("location", r.location);
    res.headers.emplace_back("cache-control", "no-cache");
    return finish(std::move(res), json::dump(extra));
  }
  res.headers.emplace_back("etag", r.etag);
  res.headers.emplace_back("cache-control", r.restricted || who ? "private, no-cache" : "no-cache");
  res.headers.emplace_back("x-content-type-options", "nosniff");
  if (r.kind != site::Kind::kStatic) {
    res.headers.emplace_back("referrer-policy", "same-origin");
    res.headers.emplace_back("vary", "accept, cookie");
  }
  for (auto& h : r.headers) res.headers.push_back(std::move(h));
  if (req.header("if-none-match") == r.etag) {
    res.status = 304;
    return finish(std::move(res), json::dump(extra));
  }
  res.headers.emplace_back("content-type", r.content_type);
  res.body = std::move(r.body);
  res.head_only = req.method == "head";
  finish(std::move(res), json::dump(extra));
}

void Host::serve_box(Request req, Finish finish, boxes::RouteRef ref, std::map<std::string, std::string> params,
                     std::string domain, std::string tenant, std::shared_ptr<const boxes::ActiveSet>) {
  auto cfg = config_.get();
  std::string request_id = req.header("goober-request-id");
  const auto& m = ref.box->manifest;

  // Identity: who is calling, and may they use this route.
  std::optional<SignedIn> who;
  try {
    who = signed_in(req, tenant);
  } catch (const std::exception& e) {
    std::cerr << "goober-host: identity: " << e.what() << "\n";
    return finish(error_page(tenant, 500, request_id), R"({"why":"identity failed"})");
  }
  std::optional<manifest::Caller> caller;
  if (who) caller = manifest::Caller{std::to_string(who->user.user_id), who->host, who->roles};
  switch (manifest::check_access(*ref.route, caller ? &*caller : nullptr, params, tenant)) {
    case manifest::Access::kAllowed:
      break;
    case manifest::Access::kSignIn:
      return finish(sign_in_or_401(req, tenant), R"({"why":"sign in first"})");
    case manifest::Access::kForbidden:
      return finish(error_page(tenant, 403, request_id), R"({"why":"requirements not met"})");
    case manifest::Access::kBadValue:
      return finish(error_page(tenant, 400, request_id), R"({"why":"a substituted value holds / or :"})");
  }
  // A browser's state-changing request must come from this site (070.UI.md § Keeping browsers safe).
  if (req.method != "get" && req.method != "head" && !same_origin(req)) {
    return finish(error_page(tenant, 403, request_id), R"({"why":"cross-origin request"})");
  }

  bj::object headers;
  for (const auto& [k, v] : req.headers) {
    if (!hidden_from_box(k)) headers[k] = v;
  }
  bj::object query;
  for (const auto& [k, v] : router::parse_query(req.query)) query[k] = v;
  bj::object params_json;
  for (const auto& [k, v] : params) params_json[k] = v;
  bj::object doc{{"request_id", request_id}, {"method", req.method},       {"path", req.path},
                 {"params", params_json},   {"query", query},             {"headers", headers},
                 {"body", req.body},        {"domain", domain},           {"tenant", tenant},
                 {"user", user_json(who)}};

  runtime::wasm::CallInput input{json::dump(doc), base::now_unix_ms(), base::os_random_u64(), {}};
  // The box's databases: one transaction each, committed below when the box answers under 500.
  std::optional<db::Session> session;
  if (!m.data.empty()) {
    session.emplace(*databases_, tenant, m.data, *schemas_);
    input.sql = [&session](std::string_view strategy, std::string_view sql, std::string_view params) {
      return session->run(strategy, sql, params);
    };
  }
  runtime::wasm::CallLimits limits{cfg->timeout_ms.resolve(m.behavior.timeout_ms), cfg->fuel.resolve(m.behavior.fuel),
                                   cfg->memory_max_bytes.resolve(m.behavior.memory_max_bytes)};
  auto result = engine_->call(*ref.box->compiled, ref.route->export_name(), input, limits);

  // Logs are side effects, written after the call, never interleaved.
  auto log_file = cfg->storage / "logs" / "boxes" / (m.box + ".log");
  for (const auto& line : result.logs) {
    base::append_line(log_file, json::dump(bj::object{{"at", base::format_utc_ms(input.now_ms)},
                                                     {"request_id", request_id},
                                                     {"version", ref.box->version},
                                                     {"level", line.level},
                                                     {"text", line.text}}));
  }
  if (result.outcome != runtime::wasm::CallResult::Outcome::kOk) {
    base::append_line(log_file, json::dump(bj::object{{"at", base::format_utc_ms(input.now_ms)},
                                                     {"request_id", request_id},
                                                     {"version", ref.box->version},
                                                     {"level", "host"},
                                                     {"text", std::string(outcome_name(result.outcome)) + ": " +
                                                                  result.error}}));
  }

  bj::object extra{{"box", m.box},
                   {"version", ref.box->version},
                   {"route", ref.route->name},
                   {"outcome", outcome_name(result.outcome)},
                   {"fuel", result.fuel_used},
                   {"seed", std::to_string(input.seed)},
                   {"now_ms", input.now_ms}};
  if (who) extra["user_id"] = std::to_string(who->user.user_id);
  bool ok = result.outcome == runtime::wasm::CallResult::Outcome::kOk && result.status < 500;
  if (session && session->statements() > 0) {
    extra["sql"] = session->statements();
    std::string err;
    if (!ok) {
      session->rollback();
    } else if (!session->commit(&err)) {
      result.outcome = runtime::wasm::CallResult::Outcome::kHostError;
      result.error = err;
      extra["outcome"] = "commit_failed";
      base::append_line(log_file, json::dump(bj::object{{"at", base::format_utc_ms(input.now_ms)},
                                                       {"request_id", request_id},
                                                       {"version", ref.box->version},
                                                       {"level", "host"},
                                                       {"text", "commit failed: " + err}}));
    }
  }
  std::string extra_text = json::dump(extra);

  Response res;
  if (result.outcome == runtime::wasm::CallResult::Outcome::kOk) {
    res.status = result.status;
    res.headers = std::move(result.headers);
    res.headers.emplace_back("content-type",
                             result.content_type.empty() ? "application/octet-stream" : result.content_type);
    res.headers.emplace_back("x-content-type-options", "nosniff");
    res.body = std::move(result.body);
    res.head_only = req.method == "head";
    // One route, many audiences (007 §8): a browser gets the JSON through the route's template.
    if (!ref.route->template_name.empty() && result.content_type.starts_with("application/json") &&
        prefers_html(req.header("accept"))) {
      std::string err;
      auto data = json::parse(res.body, &err);
      std::optional<std::string> html;
      if (data && data->is_object()) {
        bj::object d = std::move(data->as_object());
        d["user"] = user_json(who);
        bj::object query_obj;
        for (const auto& [k, v] : router::parse_query(req.query)) query_obj[k] = v;
        d["request"] = bj::object{{"path", req.path}, {"query", std::move(query_obj)}};
        html = site_->render_named(tenant, ref.route->template_name, std::move(d), &m.templates, &err);
      } else if (err.empty()) {
        err = "the response is not a JSON object";
      }
      if (html) {
        std::erase_if(res.headers, [](const auto& h) { return h.first == "content-type"; });
        res.headers.emplace_back("content-type", "text/html; charset=utf-8");
        res.headers.emplace_back("vary", "accept");
        res.body = std::move(*html);
      } else {
        base::append_line(log_file, json::dump(bj::object{{"at", base::format_utc_ms(input.now_ms)},
                                                         {"request_id", request_id},
                                                         {"version", ref.box->version},
                                                         {"level", "host"},
                                                         {"text", "template " + ref.route->template_name + ": " + err}}));
        res = error_page(tenant, 500, request_id);
      }
    }
  } else {
    int status = result.outcome == runtime::wasm::CallResult::Outcome::kTimeout ? 504 : 500;
    res = error_page(tenant, status, request_id);
  }
  finish(std::move(res), std::move(extra_text));
}

}  // namespace goober::host
