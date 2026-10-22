#include "goober/host/host.h"

#include <algorithm>
#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/json/json.h"
#include "goober/router/path.h"

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
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return status >= 500 ? "Server Error" : "Client Error";
  }
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
  engine_ = std::make_unique<runtime::wasm::Engine>();
  boxes_ = std::make_unique<boxes::BoxRegistry>(*wal_, *engine_);
  for (auto& w : boxes_->load_all()) startup_notes_.push_back(std::move(w));
  mcp_ = std::make_unique<mcp::McpServer>(
      "goober-operations", std::string(kVersion),
      "Operate a goober host: write static content per tenant (content_*), deploy WASM boxes "
      "(version_upload with a manifest and WAT or base64 WASM), and read logs.");
  register_tools();
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
  std::string notes = config_.replace(next);
  return "config reloaded. " + notes;
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
  std::string domain = host_header.substr(0, host_header.rfind(':') == std::string::npos ? host_header.size()
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
  auto tenant = cfg->tenant_for_host(host_header);
  if (!tenant) return finish(error_page("", 404, request_id), R"({"why":"unknown domain"})");
  if (!path) return finish(error_page(*tenant, 400, request_id), R"({"why":"bad path"})");

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
    serve_static(req, finish, tenant, p);
  });
}

void Host::serve_static(const Request& req, const Finish& finish, const std::string& tenant,
                        const std::string& path) {
  auto file = content_->resolve(tenant, path);
  if (!file) return finish(error_page(tenant, 404, req.header("goober-request-id")), R"({"why":"no content"})");
  Response res;
  res.headers.emplace_back("etag", file->etag);
  res.headers.emplace_back("cache-control", "no-cache");
  res.headers.emplace_back("x-content-type-options", "nosniff");
  if (req.header("if-none-match") == file->etag) {
    res.status = 304;
    return finish(std::move(res), "");
  }
  res.headers.emplace_back("content-type", file->content_type);
  res.body = std::move(file->data);
  res.head_only = req.method == "head";
  finish(std::move(res), "");
}

void Host::serve_box(Request req, Finish finish, boxes::RouteRef ref, std::map<std::string, std::string> params,
                     std::string domain, std::string tenant, std::shared_ptr<const boxes::ActiveSet>) {
  auto cfg = config_.get();
  std::string request_id = req.header("goober-request-id");
  const auto& m = ref.box->manifest;

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
                 {"body", req.body},        {"domain", domain},           {"tenant", tenant}};

  runtime::wasm::CallInput input{json::dump(doc), base::now_unix_ms(), base::os_random_u64()};
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
  } else {
    int status = result.outcome == runtime::wasm::CallResult::Outcome::kTimeout ? 504 : 500;
    res = error_page(tenant, status, request_id);
  }
  finish(std::move(res), std::move(extra_text));
}

}  // namespace goober::host
