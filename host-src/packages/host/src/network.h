#pragma once

// External calls and the outbox (design/machine/015.Network.md), private to the host package.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "goober/json/json.h"
#include "goober/manifest/manifest.h"

namespace goober::host {

// One effectful call, captured with the request and enqueued only if it commits (035).
struct OutboxElement {
  std::string service, method, verb, url, body, content_type;
  std::string credentials;  // a key name with the tenant filled in, resolved at delivery
  std::string auth_header, auth_prefix;
  std::vector<std::pair<std::string, std::string>> headers;  // pinned headers, no credentials
  std::string idempotency_header, idempotency_key;
  int64_t attempts = 8, backoff_ms = 1000, within_ms = 600000;
};

// What one box call needs to make external calls.
struct CallContext {
  const manifest::Manifest* manifest = nullptr;
  std::string tenant, domain, request_id;
  boost::json::value user;  // the request's user, or null
  std::map<std::string, int64_t> calls;  // per service.method, against its budget
  std::vector<OutboxElement> pending;
};

// A request ready to send: the URL, headers, and body after require is applied.
struct BuiltCall {
  std::string url, body, content_type;
  std::vector<std::pair<std::string, std::string>> headers;
};

// Builds a call from the box's request JSON {"path", "query", "body"}, applying the
// method's require block from the context. nullopt with `error` when it can't.
std::optional<BuiltCall> build_call(const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                                    const boost::json::value& request, const CallContext& ctx, std::string* error);

struct HttpOut {
  int status = 0;  // 0 when nothing came back
  std::string body, content_type, error;
};

// Sends one request with curl; credentials and headers travel in a private config file.
HttpOut send_http(const std::string& verb, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, const std::string& content_type, bool allow_loopback_http,
                  const std::filesystem::path& scratch, int64_t timeout_ms);

}  // namespace goober::host
