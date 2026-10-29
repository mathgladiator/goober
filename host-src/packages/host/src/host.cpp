#include "build_id.h"
#include "goober/host/host.h"
#include "goober/calendar/zones.h"
#include "goober/libraries/libraries.h"
#include "goober/site/includes.h"

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <ctime>
#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "web_fetch.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/json/json.h"
#include "goober/router/path.h"
#include "http_util.h"
#include "keys.h"
#include "backups.h"
#include "builder.h"
#include "mailer.h"
#include "network.h"
#include "timer_state.h"

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
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
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

// The endpoints worth slowing down per client address: guessing codes, minting tokens,
// registering clients, uploading, and agents' calls. Everything else shares one budget.
std::optional<std::pair<const char*, RateRule>> sensitive(const std::string& path, const std::string& method) {
  if (method != "post") return std::nullopt;
  if (path == "/identity/email/start") return std::pair{"sign-in-start", RateRule{10, 5}};
  if (path == "/identity/email/verify") return std::pair{"sign-in-verify", RateRule{20, 10}};
  if (path == "/oauth/token") return std::pair{"token", RateRule{60, 30}};
  if (path == "/oauth/register") return std::pair{"register", RateRule{20.0 / 60, 5}};
  if (path == "/oauth/device") return std::pair{"device", RateRule{10, 5}};
  if (path == "/oauth/device/verify") return std::pair{"device-verify", RateRule{10, 5}};
  if (path == "/oauth/authorize") return std::pair{"authorize", RateRule{30, 10}};
  if (path == "/attachment/upload") return std::pair{"upload", RateRule{30, 10}};
  if (path == "/mcp") return std::pair{"mcp", RateRule{120, 60}};
  return std::nullopt;
}

// A Host header is a name or address and an optional port, nothing else: "site.com:443@evil.com"
// must not resolve to site.com and then lend its text to an absolute URL.
bool valid_host_header(std::string_view h) {
  if (h.empty() || h.size() > 260) return false;
  size_t end = h.size();
  if (h.front() == '[') {
    auto close = h.find(']');
    if (close == std::string_view::npos) return false;
    for (char c : h.substr(1, close - 1)) {
      if (!std::isxdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.') return false;
    }
    end = close + 1;
  } else {
    auto colon = h.find(':');
    end = colon == std::string_view::npos ? h.size() : colon;
    if (end == 0) return false;
    for (char c : h.substr(0, end)) {
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
      if (!ok) return false;
    }
  }
  if (end == h.size()) return true;
  if (h[end] != ':' || end + 1 == h.size() || h.size() - end - 1 > 5) return false;
  for (char c : h.substr(end + 1)) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// Request headers a box never sees: credentials belong to the host.
bool hidden_from_box(std::string_view name) { return name == "authorization" || name == "cookie"; }

}  // namespace

Host::Host(std::shared_ptr<const config::HostConfig> config, std::filesystem::path config_file)
    : config_file_(std::move(config_file)), config_(config) {
  started_ms_ = base::now_unix_ms();
  wal_ = std::make_unique<wal::HostWal>(config->storage);
  // The change log sees every bundle from here on (design/machine/028.Panel.md § 5). The
  // panel's stores are never a reason not to start: without them the host serves, and says so.
  try {
    changes_ = std::make_unique<operations::ChangeLog>(config->storage / "operations" / "changes.db", config->storage);
    wal_->set_observer([this](const wal::Bundle& b) { record_bundle(b); });
  } catch (const std::exception& e) {
    startup_notes_.push_back(std::string("change log off: ") + e.what());
  }
  try {
    traffic_ = std::make_unique<operations::Traffic>(
        config->storage / "operations" / "traffic.db", config->storage / "logs",
        [this](const std::string& domain) -> std::optional<std::string> {
          if (auto r = config_.get()->resolve_host(domain)) return r->second.tenant;
          return std::nullopt;
        });
  } catch (const std::exception& e) {
    startup_notes_.push_back(std::string("traffic rollups off: ") + e.what());
  }
  const auto& rec = wal_->recovery();
  for (const auto& f : rec.failed) startup_notes_.push_back("host WAL: set aside " + f + " (see wal/failed/)");
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
  boxes_->set_manifest_check(
      [this](const manifest::Manifest& m, std::string* error) { return check_data(m, error) && check_calls(m, error); });
  timers_ = std::make_unique<TimerState>();
  keys_ = std::make_unique<KeyStore>(config->storage);
  mail_ = std::make_unique<MailQueue>([this] { return config_.get(); }, [this] { return keys_->aws_credentials(); },
                                      [this](const identity::Mail& m) { write_dev_mail(m); });
  mail_templates_ = std::make_unique<mail::templates::Store>(config->storage / "mail" / "templates");
  identity_ = std::make_unique<identity::Identity>(config->storage / "identity",
                                                   [this](const identity::Mail& m) { send_mail(m); });
  backups_ = std::make_unique<BackupService>([this] { return config_.get(); }, *keys_, config_file_);
  for (auto& w : boxes_->load_all()) startup_notes_.push_back(std::move(w));
  mcp_ = std::make_unique<mcp::McpServer>(
      "goober-operations", std::string(kVersion),
      "Operate a goober host. Build products as boxes: call box_guide first. Build sites as a CMS: call "
      "site_guide first, then write pages (page_put, as "
      "markdown, an HTML fragment in a template, or a whole HTML document), templates (template_put), and images "
      "(attachment_put, from base64 or a URL). Keep a product's source in the host and build and deploy it "
      "from there (source_create, source_checkout, source_commit, source_deploy), or deploy WASM boxes directly "
      "(version_upload). Query tenant databases (db_list, db_query), read logs, and check backups (backup_status). "
      "See traffic, heat, and data flow (traffic, hot_boxes, box_graph), and what changed and who changed it "
      "(changes, change_get, operator_actions).");
  init_web();
  register_tools();
  register_content_tools();
  register_data_tools();
  register_mail_tools();
  register_mail_out_tools();
  register_web_tools();
  register_source_tools();
  register_panel_tools();
  register_pwa_tools();
  register_library_tools();
  register_scope_tools();
  pool_ = std::make_unique<WorkerPool>(config->box_threads, config->pin_boxes);
  max_queued_ = size_t(256) * size_t(std::max(1, config->box_threads));
}

Host::~Host() { stop(); }

void Host::backup_tick(int64_t now_ms) { backups_->tick(now_ms); }

std::optional<bj::object> Host::backup_run(const std::string& what, int64_t now_ms, std::string* error) {
  bool ok = false;
  bj::object out{{"ran", what}};
  if (what == "nightly") {
    ok = backups_->run_nightly(now_ms, error);
  } else if (what == "increment") {
    ok = backups_->run_increment(now_ms, error);
  } else if (what == "assets") {
    backups_->nudge();
    error->clear();
    out["uploaded"] = backups_->run_assets(now_ms, error);
    ok = error->empty();
  } else {
    *error = "what is nightly, increment, or assets";
  }
  if (!ok) return std::nullopt;
  return out;
}

bj::object Host::backup_status(bool with_points, int64_t now_ms) {
  return backups_->status(with_points, now_ms ? now_ms : base::now_unix_ms());
}

void Host::run_backups() {
  std::unique_lock lock(timer_thread_mu_);
  while (!timers_stopping_) {
    lock.unlock();
    try {
      backups_->tick(base::now_unix_ms());
    } catch (const std::exception& e) {
      std::cerr << "goober-host: backups: " << e.what() << "\n";
    }
    lock.lock();
    timer_cv_.wait_for(lock, std::chrono::seconds(5), [this] { return timers_stopping_; });
  }
}

uint16_t Host::start() {
  auto cfg = config_.get();
  server::http::ServerOptions opts;
  opts.address = cfg->listen_address;
  opts.port = cfg->listen_port;
  opts.threads = cfg->network_threads;
  opts.body_max_bytes = uint64_t(cfg->body_max_bytes);
  // Operators push sites and boxes as base64 JSON: their bodies may be larger, once their
  // token checks out, before a byte of the body is read.
  opts.body_limit_for = [this](std::string_view target, std::string_view authorization) -> uint64_t {
    // A whole message from Stalwart, as JSON: up to the largest a route takes, plus room for
    // JSON's escapes. The token is checked again, fully, by the handler.
    if (target.substr(0, target.find('?')) == kMailHookPath && authorization.starts_with("Bearer ")) {
      auto cfg = config_.get();
      return cfg->mail.inbound.enabled() ? uint64_t(cfg->mail.inbound.max_bytes) * 2 + (1 << 20) : 0;
    }
    // Attachment uploads: as large as attachment_put takes with an operator's token, and an image
    // of 1 MiB from a browser (whose session cookie the handler checks once the body is in).
    if (target.substr(0, target.find('?')) == "/attachment/upload") {
      server::http::Request probe;
      probe.headers.emplace_back("authorization", std::string(authorization));
      bool op = authorization.starts_with("Bearer ") && !bearer_operator(probe).empty();
      if (auto q = target.find('?'); !op && q != std::string_view::npos) {
        auto params = router::parse_query(std::string(target.substr(q + 1)));
        // A live ticket (not used up here: the handler takes it once the body is in).
        if (auto t = params.find("ticket"); t != params.end()) {
          std::lock_guard lock(upload_tickets_mu_);
          auto it = upload_tickets_.find(base::sha256_hex(t->second));
          op = it != upload_tickets_.end() && it->second.expires_ms > base::now_unix_ms();
        }
      }
      return (op ? kOperatorAttachmentMaxBytes : kBrowserAttachmentMaxBytes) + kMultipartOverheadBytes;
    }
    if (target.substr(0, target.find('?')) != kOperationsMcpPath || !authorization.starts_with("Bearer ")) return 0;
    server::http::Request probe;
    probe.headers.emplace_back("authorization", std::string(authorization));
    return bearer_operator(probe).empty() ? 0 : kOperatorBodyMaxBytes;
  };
  server_ = std::make_unique<server::http::Server>(
      opts, [this](Request req, Responder respond) { handle(std::move(req), std::move(respond)); });
  server_->start();
  // The panel's own listener (operations.listen): every request on it is the panel's.
  if (cfg->operations_listen_port) {
    server::http::ServerOptions panel_opts;
    panel_opts.address = cfg->operations_listen_address;
    panel_opts.port = *cfg->operations_listen_port;
    panel_opts.threads = 1;
    panel_opts.body_max_bytes = uint64_t(cfg->body_max_bytes);
    panel_opts.body_limit_for = opts.body_limit_for;
    panel_server_ = std::make_unique<server::http::Server>(
        panel_opts, [this](Request req, Responder respond) { handle(std::move(req), std::move(respond), true); });
    panel_server_->start();
  }
  timer_thread_ = std::thread([this] { run_timers(); });
  if (traffic_) traffic_thread_ = std::thread([this] { run_traffic(); });
  outbox_thread_ = std::thread([this] { run_outbox(); });
  crawl_thread_ = std::thread([this] { run_crawls(); });
  if (!backups_manual_) backup_thread_ = std::thread([this] { run_backups(); });
  if (cfg->backup.enabled() && !cfg->sandbox) {
    auto status = backups_->status(false, base::now_unix_ms());
    if (status.contains("key_warning")) startup_notes_.push_back(std::string(status.at("key_warning").as_string()));
  }
  return server_->port();
}

void Host::stop() {
  if (timer_thread_.joinable()) {
    {
      std::lock_guard lock(timer_thread_mu_);
      timers_stopping_ = true;
    }
    timer_cv_.notify_all();
    timer_thread_.join();
    if (outbox_thread_.joinable()) outbox_thread_.join();
    if (crawl_thread_.joinable()) crawl_thread_.join();
    if (backup_thread_.joinable()) backup_thread_.join();
    if (traffic_thread_.joinable()) traffic_thread_.join();
  }
  if (panel_server_) panel_server_->stop_accepting();
  if (!server_) {
    if (pool_) pool_->shutdown();
    return;
  }
  // Graceful: stop taking connections, let the requests already here finish (up to ten
  // seconds), then close what's left. systemd restarts land between requests, not inside one.
  server_->stop_accepting();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (pool_->busy() > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  // Before anything the jobs use is torn down: what never started is dropped, and what is
  // running finishes while the server can still answer it.
  pool_->shutdown();
  server_->stop();
  server_.reset();
  if (panel_server_) {
    panel_server_->stop();
    panel_server_.reset();
  }
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
void Host::drain_mail() { mail_->drain(); }

// Every mail is queued, so a sign-in answers in the same time whether or not one goes out.
std::string Host::issue_upload_ticket(UploadTicket t) {
  uint8_t raw[24];
  base::os_random_fill(raw);
  std::string ticket = base::hex_encode(std::string_view(reinterpret_cast<char*>(raw), sizeof raw));
  std::lock_guard lock(upload_tickets_mu_);
  int64_t now = base::now_unix_ms();
  for (auto it = upload_tickets_.begin(); it != upload_tickets_.end();) {
    if (it->second.expires_ms > now) {
      ++it;
      continue;
    }
    upload_part_bytes_ -= it->second.parts.size();
    it = upload_tickets_.erase(it);
  }
  // A bound on what's outstanding: the oldest goes first.
  while (upload_tickets_.size() >= 100) {
    auto oldest = std::min_element(upload_tickets_.begin(), upload_tickets_.end(),
                                   [](const auto& a, const auto& b) { return a.second.expires_ms < b.second.expires_ms; });
    upload_part_bytes_ -= oldest->second.parts.size();
    upload_tickets_.erase(oldest);
  }
  upload_tickets_[base::sha256_hex(ticket)] = std::move(t);
  return ticket;
}

std::optional<std::string> Host::upload_ticket_tenant(const std::string& ticket) {
  std::lock_guard lock(upload_tickets_mu_);
  auto it = upload_tickets_.find(base::sha256_hex(ticket));
  if (it == upload_tickets_.end() || it->second.expires_ms <= base::now_unix_ms()) return std::nullopt;
  return it->second.tenant;
}

std::optional<Host::UploadTicket> Host::upload_ticket(const std::string& ticket, const std::string& tenant, bool consume) {
  std::lock_guard lock(upload_tickets_mu_);
  auto it = upload_tickets_.find(base::sha256_hex(ticket));
  if (it == upload_tickets_.end() || it->second.expires_ms <= base::now_unix_ms() || it->second.tenant != tenant) {
    return std::nullopt;
  }
  UploadTicket t = it->second;
  if (consume) {
    upload_part_bytes_ -= it->second.parts.size();
    upload_tickets_.erase(it);
  }
  return t;
}

// A mail naming a template is rendered from the tenant's templates here, so the queue sends
// what was rendered; any other mail is wrapped in the tenant's layout for its HTML part.
void Host::send_mail(const identity::Mail& mail) {
  identity::Mail out = mail;
  std::string tenant = mail.realm.empty() ? std::string(mail::templates::kEveryTenant) : mail.realm;
  bj::object vars = mail.vars;
  vars["email"] = mail.to;
  if (!vars.contains("site")) vars["site"] = config_.get()->platform_domain;
  if (!mail.template_name.empty()) {
    std::string err;
    if (auto r = mail_templates_->render(tenant, mail.template_name, vars, &err)) {
      out.subject = std::move(r->subject);
      out.text = std::move(r->text);
      out.html = std::move(r->html);
    } else {
      // The fallback text still goes: a broken template must never stop a sign-in code.
      std::cerr << "goober-host: mail template " << mail.template_name << " for " << tenant << ": " << err << "\n";
    }
  } else if (out.html.empty()) {
    out.html = mail_templates_->wrap_text(tenant, out.subject, out.text, vars).html;
  }
  out.template_name.clear();
  mail_->send(std::move(out));
}

void Host::write_dev_mail(const identity::Mail& mail) {
  auto cfg = config_.get();
  int64_t now = base::now_unix_ms();
  uint8_t rnd[4];
  base::os_random_fill(rnd);
  std::string name = std::to_string(now) + "-" + base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), 4)) + ".eml";
  // The same message a real mailer would send, headers and all.
  std::string eml = mime_message("no-reply@" + cfg->platform_domain, mail, now,
                                 base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), 4)));
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
                  {"path", req.path.size() > 512 ? req.path.substr(0, 512) + "..." : req.path},
                  {"status", status},
                  {"us", base::monotonic_us() - started_us}};
  if (!extra.empty()) {
    std::string err;
    if (auto v = json::parse(extra, &err); v && v->is_object()) {
      // Extra fields never overwrite the line's own (status, path, and the rest).
      for (auto& kv : v->get_object()) {
        if (!line.contains(kv.key())) line[kv.key()] = kv.value();
      }
    }
  }
  base::append_line(config_.get()->storage / "logs" / "access.log", json::dump(line));
}

Response Host::error_page(const std::string& tenant, int status, const std::string& request_id) const {
  Response res;
  res.status = status;
  res.host_error = true;
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

uint16_t Host::panel_port() const { return panel_server_ ? panel_server_->port() : 0; }

void Host::handle(Request req, Responder respond, bool panel_listener) {
  ++requests_;
  int64_t started = base::monotonic_us();
  std::string request_id = new_request_id();
  req.headers.erase(std::remove_if(req.headers.begin(), req.headers.end(),
                                   [](const auto& h) { return h.first == "goober-request-id"; }),
                    req.headers.end());
  req.headers.emplace_back("goober-request-id", request_id);
  auto cfg = config_.get();
  std::string host_header = base::to_lower(req.header("host"));
  if (!valid_host_header(host_header)) {
    Response bad;
    bad.status = 400;
    bad.headers.emplace_back("content-type", "text/plain; charset=utf-8");
    bad.body = "Bad Request: the Host header is malformed\n";
    access_log(req, 400, started, "", R"({"why":"bad host header"})");
    bad.headers.emplace_back("goober-request-id", request_id);
    return respond(std::move(bad));
  }
  auto resolved = cfg->resolve_host(host_header);
  // The configured domain (local aliases resolve to it), or the raw host for the log.
  std::string domain = resolved ? resolved->first
                                : host_header.substr(0, host_header.rfind(':') == std::string::npos
                                                            ? host_header.size()
                                                            : host_header.rfind(':'));

  // Every response is logged and stamped with its request id.
  bool secure_transport = cfg->identity.cookie_secure && !cfg->local;
  Finish finish = [this, req, respond, started, domain, request_id, secure_transport](Response res, std::string extra) {
    res.headers.emplace_back("goober-request-id", request_id);
    bool bearer = base::to_lower(req.header("authorization")).starts_with("bearer ");
    // HSTS once the host is served over TLS: a person's browser never tries plain HTTP again.
    if (secure_transport) res.headers.emplace_back("strict-transport-security", "max-age=31536000");
    bool has_challenge = std::any_of(res.headers.begin(), res.headers.end(),
                                     [](const auto& h) { return h.first == "www-authenticate"; });
    if (res.status == 401 && !has_challenge) {
      // RFC 6750 §3: say how to authenticate, and whether the token sent was the problem.
      res.headers.emplace_back("www-authenticate", bearer ? "Bearer error=\"invalid_token\"" : "Bearer");
    }
    // An API caller gets the host's errors as JSON rather than an HTML page.
    std::string accept = req.header("accept");
    if (res.host_error && (bearer || (accept.find("application/json") != std::string::npos && !prefers_html(accept)))) {
      std::erase_if(res.headers, [](const auto& h) { return h.first == "content-type"; });
      res.headers.emplace_back("content-type", "application/json");
      res.body = json::dump(bj::object{{"error", reason(res.status)}, {"status", res.status}, {"request_id", request_id}});
    }
    access_log(req, res.status, started, domain, extra);
    respond(std::move(res));
  };

  auto path = router::normalize_path(req.path);
  // The panel's origin serves the panel alone (design/machine/028.Panel.md § 1).
  bool panel = is_panel_request(req, panel_listener);
  // Stalwart's MTA hook: checked in full by handle_mail_hook (loopback, no proxy, its token).
  if (!panel && path && path->path == kMailHookPath) {
    pool_->post([this, req, finish] {
      try {
        handle_mail_hook(req, finish);
      } catch (const std::exception& e) {
        std::cerr << "goober-host: mail in: " << e.what() << "\n";
        server::http::Response res;
        res.status = 500;
        finish(std::move(res), R"({"mail_in":"failed"})");
      }
    });
    return;
  }
  // Floods and guessing are slowed per client address, before any work is queued.
  {
    std::string client = rate_key(client_address(req));
    int64_t now = base::now_unix_ms();
    double rpm = double(cfg->requests_per_minute);
    // Operators are limited only when their token fails (handle_operations), so a big push
    // from the operator's own machine isn't cut off halfway.
    bool operations = path && path->path == kOperationsMcpPath;
    int64_t wait = operations ? 0 : limiter_.take("all", client, RateRule{rpm, std::max(10.0, rpm / 4)}, now);
    std::string bucket = "all";
    if (!wait && path) {
      if (auto rule = sensitive(path->path, req.method)) {
        wait = limiter_.take(rule->first, client, rule->second, now);
        bucket = rule->first;
      }
    }
    if (wait) {
      auto res = error_page(resolved ? resolved->second.tenant : "", 429, request_id);
      res.headers.emplace_back("retry-after", std::to_string(wait));
      return finish(std::move(res), json::dump(bj::object{{"why", "rate limited"}, {"bucket", bucket}, {"client", client}}));
    }
  }
  // For the proxy and monitors: answers without touching storage, whatever the Host (a
  // proxy's health check names the upstream address, which is no configured domain).
  if (path && path->path == "/goober/health") {
    Response res;
    res.headers.emplace_back("content-type", "application/json");
    res.headers.emplace_back("cache-control", "no-store");
    // The build's commit, so a deploy can be confirmed from anywhere with curl.
    res.body = std::string(R"({"ok":true,"build":")") + kBuildCommit + "\"}";
    res.head_only = req.method == "head";
    return finish(std::move(res), "");
  }
  // The last deploy's outside check (readiness.py, run by the server after the gate), for bots.
  if (path && path->path == "/goober/deploy") {
    Response res;
    auto text = base::read_file(cfg->storage / "operations" / "deploy-check.json");
    res.status = text ? 200 : 404;
    res.body = text ? *text : R"({"error":"no deploy has been checked from outside yet"})";
    res.headers.emplace_back("content-type", "application/json");
    res.headers.emplace_back("cache-control", "no-store");
    res.head_only = req.method == "head";
    return finish(std::move(res), "");
  }
  // Readiness, for deploys and bots: every subsystem's status, 503 unless all are fine.
  if (path && path->path == "/goober/ready") {
    pool_->post([this, req, finish] {
      Response res;
      try {
        auto [status, body] = readiness(true);
        res.status = status;
        res.body = json::dump(body);
      } catch (const std::exception& e) {
        res.status = 503;
        res.body = json::dump(bj::object{{"ok", false}, {"error", e.what()}});
      }
      res.headers.emplace_back("content-type", "application/json");
      res.headers.emplace_back("cache-control", "no-store");
      res.head_only = req.method == "head";
      finish(std::move(res), "");
    });
    return;
  }
  if (path && path->path == kOperationsMcpPath) {
    pool_->post([this, req, finish] { handle_operations(req, finish); });
    return;
  }
  if (panel) {
    if (!path) return finish(error_page("", 400, request_id), R"({"why":"bad path","panel":"page"})");
    pool_->post([this, req, finish, p = path->path, domain] {
      try {
        handle_panel(req, finish, p, domain);
      } catch (const std::exception& e) {
        std::cerr << "goober-host: panel: " << e.what() << "\n";
        finish(error_page("", 500, req.header("goober-request-id")), R"({"why":"panel failed","panel":"page"})");
      }
    });
    return;
  }
  if (!resolved) return finish(error_page("", 404, request_id), R"({"why":"unknown domain"})");
  auto tenant = std::optional<std::string>(resolved->second.tenant);
  if (!path) return finish(error_page(*tenant, 400, request_id), R"({"why":"bad path"})");
  if (path->path == "/mcp") {
    pool_->post([this, req, finish, domain, tenant = *tenant] {
      try {
        handle_tenant_mcp(req, finish, domain, tenant);
      } catch (const std::exception& e) {
        std::cerr << "goober-host: mcp: " << e.what() << "\n";
        finish(error_page(tenant, 500, req.header("goober-request-id")), R"({"why":"mcp failed"})");
      }
    });
    return;
  }
  if (path->path == "/oauth" || path->path.starts_with("/oauth/") || path->path.starts_with("/.well-known/")) {
    pool_->post([this, req, finish, p = path->path, domain, tenant = *tenant] {
      try {
        handle_oauth(req, finish, p, domain, tenant);
      } catch (const std::exception& e) {
        std::cerr << "goober-host: oauth: " << e.what() << "\n";
        finish(error_page(tenant, 500, req.header("goober-request-id")), R"({"why":"oauth failed"})");
      }
    });
    return;
  }
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

  if (path->path.starts_with("/goober/pwa/")) {
    pool_->post([this, req, finish, p = path->path, tenant = *tenant] {
      try {
        handle_pwa(req, finish, p, tenant);
      } catch (const std::exception& e) {
        std::cerr << "goober-host: pwa: " << e.what() << "\n";
        finish(error_page(tenant, 500, req.header("goober-request-id")), R"({"why":"pwa failed"})");
      }
    });
    return;
  }
  if (path->path.starts_with("/goober/unsubscribe/")) {
    pool_->post([this, req, finish, p = path->path, tenant = *tenant] {
      try {
        handle_unsubscribe(req, finish, p, tenant);
      } catch (const std::exception& e) {
        std::cerr << "goober-host: unsubscribe: " << e.what() << "\n";
        finish(error_page(tenant, 500, req.header("goober-request-id")), R"({"why":"unsubscribe failed"})");
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
  if (req.method == "options") {
    // A CORS preflight, answered by the host (037): which verbs at this path allow the origin.
    std::string origin = base::to_lower(req.header("origin"));
    std::vector<std::string> verbs;
    std::set<std::string> allow_headers = {"authorization", "content-type"};
    bool any_origin = false;
    for (const char* v : {"get", "post", "put", "patch", "delete"}) {
      auto m = table.match(path->segments, v);
      if (!m.value) continue;
      const auto& cors = m.value->route->cors;
      bool star = std::find(cors.begin(), cors.end(), "*") != cors.end();
      if (star || (!origin.empty() && std::find(cors.begin(), cors.end(), origin) != cors.end())) {
        verbs.push_back(base::to_lower(v) == "get" ? "GET, HEAD" : std::string(v));
        any_origin |= star;
        for (const auto& h : m.value->route->request_headers) allow_headers.insert(h);
      }
    }
    if (verbs.empty() || origin.empty()) return finish(error_page(*tenant, 403, request_id), R"({"why":"no cors"})");
    Response res;
    res.status = 204;
    std::string methods, headers;
    for (auto& v : verbs) {
      for (auto& c : v) c = char(std::toupper(static_cast<unsigned char>(c)));
      methods += (methods.empty() ? "" : ", ") + v;
    }
    for (const auto& h : allow_headers) headers += (headers.empty() ? "" : ", ") + h;
    res.headers.emplace_back("access-control-allow-origin", any_origin ? "*" : req.header("origin"));
    res.headers.emplace_back("access-control-allow-methods", methods);
    res.headers.emplace_back("access-control-allow-headers", headers);
    res.headers.emplace_back("access-control-max-age", "600");
    res.headers.emplace_back("vary", "origin");
    return finish(std::move(res), R"({"cors":"preflight"})");
  }
  std::string verb = req.method == "head" ? "get" : req.method;
  auto match = table.match(path->segments, verb);
  if (match.value) {
    auto ref = *match.value;
    // The queue holds a bounded amount of work; past it, the host says so rather than swell.
    bool queued = pool_->post(
        [this, req, finish, ref, params = std::move(match.params), domain, tenant = *tenant, active]() mutable {
          try {
            serve_box(req, finish, ref, std::move(params), domain, tenant, active);
          } catch (const std::exception& e) {
            std::cerr << "goober-host: box " << ref.box->manifest.box << ": " << e.what() << "\n";
            finish(error_page(tenant, 500, req.header("goober-request-id")), R"({"why":"box call failed"})");
          }
        },
        max_queued_);
    if (!queued) return finish(error_page(*tenant, 503, request_id), R"({"why":"queue full"})");
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

std::string Host::client_address(const Request& req) const {
  if (config_.get()->behind_proxy) {
    // The proxy appends the address it saw; anything before it came from the client.
    std::string xff = req.header("x-forwarded-for");
    auto comma = xff.rfind(',');
    std::string last = comma == std::string::npos ? xff : xff.substr(comma + 1);
    while (!last.empty() && last.front() == ' ') last.erase(0, 1);
    while (!last.empty() && last.back() == ' ') last.pop_back();
    if (!last.empty() && last.size() <= 64) return last;
  }
  return req.remote;
}

std::optional<SignedIn> Host::caller(const Request& req, const std::string& tenant) {
  auto who = signed_in(req, tenant);
  // Pages and templates aren't routes an agent opted into (050): an agent reads them as nobody.
  if (who && who->agent) return std::nullopt;
  return who;
}

bj::value Host::user_json(const std::optional<SignedIn>& who) {
  if (!who) return nullptr;
  bj::array roles;
  for (const auto& r : who->roles) roles.push_back(bj::value(r));
  bj::object u{{"user_id", std::to_string(who->user.user_id)},
               {"email", who->user.email},
               {"roles", std::move(roles)},
               {"host", who->host},
               {"agent", who->agent}};
  // {!client_id}: set only for an agent, so a box can tell who acts for the person (050).
  if (who->agent) u["client_id"] = who->client_id;
  return u;
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
  bool bearer = base::to_lower(req.header("authorization")).starts_with("bearer ");
  if (!bearer && (req.method == "get" || req.method == "head") &&
      req.header("accept").find("text/html") != std::string::npos) {
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
  for (auto& h : r.headers) {
    if (h.first == "x-goober-partial-missing") extra["partial_missing"] = h.second;
    res.headers.push_back(std::move(h));
  }
  if (req.header("if-none-match") == r.etag) {
    res.status = 304;
    return finish(std::move(res), json::dump(extra));
  }
  res.headers.emplace_back("content-type", r.content_type);
  res.body = std::move(r.body);
  res.head_only = req.method == "head";
  finish(std::move(res), json::dump(extra));
}

// Runs one route or tool of a box: its databases' transactions, its logs, and the check
// of a typed response. Commits when the box answered below 500, else rolls back.
Host::BoxCall Host::call_box(const boxes::RouteRef& ref, const bj::object& doc, const std::string& tenant,
                             const std::string& request_id, const std::optional<SignedIn>& who,
                             const CallOptions& options) {
  auto cfg = config_.get();
  const auto& m = ref.box->manifest;
  const auto& route = *ref.route;
  runtime::wasm::CallInput input{json::dump(doc), base::now_unix_ms(), base::os_random_u64(), {}};
  // The box's databases: one transaction each, committed below when the box answers under 500.
  std::optional<db::Session> session;
  if (!m.data.empty()) {
    session.emplace(*databases_, tenant, m.data, *schemas_);
    // The host writes too: the outbox, in the first strategy that applies a schema, and a
    // timer's at-field row. Those transactions take the write lock from the start, and the
    // outbox's database commits after the box's others (015 §4).
    bool outbox = false;
    for (const auto& s : m.external) outbox |= s.has_outbox();
    for (const auto& d : m.data) {
      if (!d.applies_schema()) continue;
      if (options.before_commit) session->expect_writes(d);
      if (outbox) {
        session->expect_writes(d);
        session->commit_last(d);
        outbox = false;
      }
    }
    input.sql = [&session](std::string_view strategy, std::string_view sql, std::string_view params) {
      return session->run(strategy, sql, params);
    };
  }
  // External services: calls go out through the host; effectful ones wait in `net.pending`
  // until the request commits (015).
  CallContext net;
  if (!m.external.empty()) {
    net.manifest = &m;
    net.tenant = tenant;
    net.domain = doc.contains("domain") ? std::string(doc.at("domain").as_string()) : "";
    net.request_id = request_id;
    net.user = user_json(who);
    net.who = who;
    net.route = ref.route->name;
    input.call = [this, &net](std::string_view service, std::string_view method, std::string_view request) {
      return external_call(net, service, method, request);
    };
  }
  // Host libraries (design/machine/831.Libraries.md): pure, on the request's clock, 10,000 calls
  // a request at most.
  int64_t library_calls = 0;
  bool used_tzdata = false;
  if (!m.libraries.empty()) {
    input.lib = [&m, &library_calls, &used_tzdata, now = input.now_ms](std::string_view alias, std::string_view fn,
                                                                         std::string_view arg) -> std::string {
      const manifest::Manifest::LibraryImport* import = nullptr;
      for (const auto& l : m.libraries) {
        if (l.alias == alias) import = &l;
      }
      if (!import) return json::dump(bj::object{{"error", "no library named " + std::string(alias) + " in the manifest"},
                                                {"code", "unknown_library"}});
      const auto* lib = libraries::find(import->library, import->major);
      if (!lib) return R"({"error":"the host no longer has this library","code":"unknown_library"})";
      if (++library_calls > 10000) return R"({"error":"over 10000 library calls in one request","code":"limit"})";
      libraries::Context ctx{now};
      std::string out = libraries::call(*lib, fn, arg, ctx);
      used_tzdata |= ctx.used_tzdata;
      return out;
    };
  }
  runtime::wasm::CallLimits limits{cfg->timeout_ms.resolve(options.timeout_ms ? options.timeout_ms : m.behavior.timeout_ms),
                                   cfg->fuel.resolve(m.behavior.fuel),
                                   cfg->memory_max_bytes.resolve(m.behavior.memory_max_bytes)};
  net.deadline_us = base::monotonic_us() + limits.timeout_ms * 1000;
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
  if (library_calls) extra["libraries"] = library_calls;
  if (used_tzdata) extra["tzdata"] = calendar::tzdata_version();  // for a replay on newer rules
  // A typed response must match its type: a box can't leak a field the schema doesn't name (037).
  if (std::string type = types::json_type_name(route.response);
      !type.empty() && result.outcome == runtime::wasm::CallResult::Outcome::kOk && result.status >= 200 &&
      result.status < 300 && result.content_type.starts_with("application/json") && ref.box->types) {
    std::string err;
    auto parsed = json::parse(result.body, &err);
    auto checked = parsed ? ref.box->types->check(type, *parsed, &err) : std::nullopt;
    if (!checked) {
      base::append_line(log_file, json::dump(bj::object{{"at", base::format_utc_ms(input.now_ms)},
                                                       {"request_id", request_id},
                                                       {"version", ref.box->version},
                                                       {"level", "host"},
                                                       {"text", "response isn't a valid " + type + ": " + err}}));
      // Treated as a host error: the write set is rolled back below, and the caller gets a 500.
      result.outcome = runtime::wasm::CallResult::Outcome::kHostError;
      result.error = "response isn't a valid " + type;
      extra["outcome"] = "bad_response";
    } else {
      result.body = json::dump(*checked);
    }
  }
  bool ok = (result.outcome == runtime::wasm::CallResult::Outcome::kOk && result.status < 500) ||
            (options.no_response_ok && result.outcome == runtime::wasm::CallResult::Outcome::kNoResponse);
  if (ok && !net.pending.empty() && session) {
    std::string err;
    if (!enqueue_outbox(m, tenant, net.pending, &*session, &err)) {
      ok = false;
      result.outcome = runtime::wasm::CallResult::Outcome::kHostError;
      result.error = "outbox: " + err;
      extra["outcome"] = "outbox_failed";
    }
    extra["outbox"] = int64_t(net.pending.size());
  }
  if (ok && session && options.before_commit) {
    std::string err;
    if (!options.before_commit(*session, &err)) {
      ok = false;
      result.outcome = runtime::wasm::CallResult::Outcome::kHostError;
      result.error = err;
      extra["outcome"] = "host_write_failed";
    }
  }
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
  if (ok && !net.pending.empty() && !session) {
    // A box without data has no transaction to join; its elements go in once it succeeded.
    std::string err;
    if (!enqueue_outbox(m, tenant, net.pending, nullptr, &err)) {
      std::cerr << "goober-host: outbox for " << m.box << ": " << err << "\n";
    }
    extra["outbox"] = int64_t(net.pending.size());
  }
  return BoxCall{std::move(result), std::move(extra)};
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
  // Agent tokens reach only routes that opted in (050.AgentsMCP.md § Who is calling).
  if (who && who->agent && !ref.route->agents) {
    return finish(error_page(tenant, 403, request_id), R"({"why":"agent tokens can't call this route"})");
  }
  // A browser's state-changing request must come from this site (070.UI.md § Keeping browsers safe).
  // A bearer token isn't sent by a browser on its own, so only cookie callers are checked.
  bool bearer_call = base::to_lower(req.header("authorization")).starts_with("bearer ");
  if (req.method != "get" && req.method != "head" && !bearer_call && !same_origin(req, origin_of(req))) {
    return finish(error_page(tenant, 403, request_id), R"({"why":"cross-origin request"})");
  }
  // Anonymous writes (a landing page's form, say) get their own, tighter budget per address,
  // so a public route that stores or mails what it's given can't be turned into a flood.
  std::string client_key = rate_key(client_address(req));
  if (!who && req.method != "get" && req.method != "head") {
    if (int64_t wait = limiter_.take("anonymous-write", client_key, RateRule{20, 10}, base::now_unix_ms())) {
      auto res = error_page(tenant, 429, request_id);
      res.headers.emplace_back("retry-after", std::to_string(wait));
      return finish(std::move(res), R"({"why":"rate limited","bucket":"anonymous-write"})");
    }
  }

  // What the request carries, checked against what the route declares (037): nothing
  // invalid reaches the box.
  const auto& route = *ref.route;
  auto refuse = [&](int status, const std::string& why) {
    Response r;
    r.status = status;
    r.headers.emplace_back("content-type", "application/json");
    r.headers.emplace_back("cache-control", "no-store");
    r.body = json::dump(bj::object{{"error", why}, {"status", status}, {"request_id", request_id}});
    finish(std::move(r), json::dump(bj::object{{"box", m.box}, {"route", route.name}, {"refused", why}}));
  };
  if (route.max_body_bytes && int64_t(req.body.size()) > *route.max_body_bytes) {
    return refuse(413, "the body is larger than " + std::to_string(*route.max_body_bytes) + " bytes");
  }
  std::string body = req.body;
  if (std::string type = types::json_type_name(route.body); !type.empty()) {
    if (!base::to_lower(req.header("content-type")).starts_with("application/json")) {
      return refuse(415, "the body must be application/json");
    }
    std::string err;
    auto parsed = json::parse(req.body, &err);
    if (!parsed) return refuse(400, "the body isn't JSON: " + err);
    auto checked = ref.box->types ? ref.box->types->check(type, *parsed, &err) : std::nullopt;
    if (!checked) return refuse(400, type + " " + err);
    body = json::dump(*checked);
  }
  bj::object headers;
  for (const auto& [k, v] : req.headers) {
    if (hidden_from_box(k)) continue;
    if (route.headers_declared &&
        std::find(route.request_headers.begin(), route.request_headers.end(), k) == route.request_headers.end()) {
      continue;
    }
    headers[k] = v;
  }
  bj::object query;
  auto raw_query = router::parse_query(req.query);
  if (route.query_declared) {
    // Only declared parameters, typed, with defaults filled in.
    for (const auto& [name, schema] : route.query) {
      auto it = raw_query.find(name);
      if (it == raw_query.end()) {
        if (schema->default_value) query[name] = *schema->default_value;
        continue;
      }
      std::string err;
      auto v = types::check_text(*schema, it->second, &err);
      if (!v) return refuse(400, "query parameter " + name + " " + err);
      query[name] = std::move(*v);
    }
  } else {
    for (const auto& [k, v] : raw_query) query[base::valid_utf8(k)] = base::valid_utf8(v);
  }
  bj::object params_json;
  for (const auto& [k, v] : params) params_json[k] = v;
  bj::object doc{{"request_id", request_id}, {"method", req.method},       {"path", req.path},
                 {"params", params_json},   {"query", query},             {"headers", headers},
                 {"body", body},            {"domain", domain},           {"tenant", tenant},
                 {"user", user_json(who)}};
  // A plain HTML form's fields, decoded, so a box needn't parse urlencoded text itself.
  if (types::json_type_name(route.body).empty() &&
      base::to_lower(req.header("content-type")).starts_with("application/x-www-form-urlencoded")) {
    bj::object form;
    for (const auto& [k, v] : router::parse_query(req.body)) form[base::valid_utf8(k)] = base::valid_utf8(v);
    doc["form"] = std::move(form);
  }
  // Who is asking, without saying who: the same address on the same site gives the same key,
  // so a box can spot repeats without ever holding an IP address.
  doc["client_key"] = base::sha256_hex("goober-client\n" + tenant + "\n" + client_key).substr(0, 16);

  auto call = call_box(ref, doc, tenant, request_id, who);
  auto& result = call.result;
  auto& extra = call.extra;
  auto log_file = cfg->storage / "logs" / "boxes" / (m.box + ".log");
  int64_t now_ms = extra["now_ms"].as_int64();
  std::string extra_text = json::dump(extra);

  // The status vocabulary (037): 5xx belongs to the host, a redirect may only go to a path
  // on this site, and an "empty" response is a 204.
  if (result.outcome == runtime::wasm::CallResult::Outcome::kOk) {
    std::string why;
    if (result.status >= 500) {
      why = "the box answered " + std::to_string(result.status) + "; 5xx belongs to the host";
    } else if (result.status >= 300 && result.status < 400) {
      std::string location;
      for (const auto& [k, v] : result.headers) {
        if (k == "location") location = v;
      }
      bool code_ok = result.status == 301 || result.status == 302 || result.status == 303 || result.status == 307 ||
                     result.status == 308;
      if (!code_ok || location.empty() || safe_return(location) != location) {
        why = "the box redirected to \"" + location.substr(0, 200) + "\"; a box may only redirect to a path on this site";
      }
    }
    if (!why.empty()) {
      base::append_line(log_file, json::dump(bj::object{{"at", base::format_utc_ms(now_ms)},
                                                       {"request_id", request_id},
                                                       {"version", ref.box->version},
                                                       {"level", "host"},
                                                       {"text", why}}));
      result.outcome = runtime::wasm::CallResult::Outcome::kHostError;
    } else if (ref.route->response == "empty" && result.status >= 200 && result.status < 300) {
      result.status = 204;
      result.body.clear();
    }
  }
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
    bool templated = false;
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
        // The page carries the person's data and runs in a browser: the templated CSP, and
        // never a shared cache when someone is signed in, whatever the box said (007 §8).
        std::erase_if(res.headers, [&](const auto& h) {
          return h.first == "content-security-policy" || (who && h.first == "cache-control");
        });
        res.headers.emplace_back("content-security-policy", site::templated_csp());
        res.headers.emplace_back("referrer-policy", "same-origin");
        res.headers.emplace_back("vary", "accept, cookie");
        if (who) res.headers.emplace_back("cache-control", "private, no-cache");
        res.body = std::move(*html);
        templated = true;
      } else {
        base::append_line(log_file, json::dump(bj::object{{"at", base::format_utc_ms(now_ms)},
                                                         {"request_id", request_id},
                                                         {"version", ref.box->version},
                                                         {"level", "host"},
                                                         {"text", "template " + ref.route->template_name + ": " + err}}));
        res = error_page(tenant, 500, request_id);
      }
    }
    // Shared partials (design/machine/027.Partials.md): always in what the host rendered, and in
    // the box's own HTML when the route opts in.
    bool html_out = false;
    for (const auto& [k, v] : res.headers) {
      if (k == "content-type" && v.starts_with("text/html")) html_out = true;
    }
    if (html_out && res.status < 300 && (templated || ref.route->includes)) {
      auto e = site::expand_includes(*content_, tenant, res.body, req.path, who.has_value());
      res.body = std::move(e.html);
      if (e.by_auth) {
        std::erase_if(res.headers, [](const auto& h) { return h.first == "cache-control" || h.first == "vary"; });
        res.headers.emplace_back("cache-control", "private, no-cache");
        res.headers.emplace_back("vary", "accept, cookie");
      }
      if (!e.missing.empty()) {
        std::string names;
        for (const auto& n : e.missing) names += (names.empty() ? "" : ", ") + n;
        res.headers.emplace_back("x-goober-partial-missing", names);
        base::append_line(log_file, json::dump(bj::object{{"at", base::format_utc_ms(now_ms)},
                                                         {"request_id", request_id},
                                                         {"version", ref.box->version},
                                                         {"level", "host"},
                                                         {"text", "no partial named " + names + " (_partials/)"}}));
      }
    }
  } else {
    int status = result.outcome == runtime::wasm::CallResult::Outcome::kTimeout ? 504 : 500;
    res = error_page(tenant, status, request_id);
  }
  // Conditional GETs (037): an ETag from the answer's bytes, and a 304 when the caller has
  // them already. The box still runs; the bytes don't travel.
  if (res.status == 200 && (req.method == "get" || req.method == "head") && !res.host_error) {
    bool has_etag = std::any_of(res.headers.begin(), res.headers.end(), [](const auto& h) { return h.first == "etag"; });
    if (!has_etag) res.headers.emplace_back("etag", "\"" + base::sha256_hex(res.body).substr(0, 20) + "\"");
    std::string etag;
    for (const auto& [k, v] : res.headers) {
      if (k == "etag") etag = v;
    }
    if (!etag.empty() && req.header("if-none-match") == etag) {
      res.status = 304;
      res.body.clear();
    }
  }
  // CORS (037): the route's listed origins may read the answer from a browser. Credentials
  // never cross: no cookie rides a cross-origin request, so only bearer callers get in.
  std::string origin = req.header("origin");
  if (!origin.empty() && !ref.route->cors.empty()) {
    bool any = std::find(ref.route->cors.begin(), ref.route->cors.end(), "*") != ref.route->cors.end();
    if (any || std::find(ref.route->cors.begin(), ref.route->cors.end(), base::to_lower(origin)) != ref.route->cors.end()) {
      res.headers.emplace_back("access-control-allow-origin", any ? "*" : origin);
      res.headers.emplace_back("vary", "origin");
    }
  }
  finish(std::move(res), std::move(extra_text));
}

}  // namespace goober::host
