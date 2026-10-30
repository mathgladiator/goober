#pragma once

// External calls and the outbox (design/machine/015.Network.md), private to the host package.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "goober/json/json.h"
#include "goober/host/host.h"
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
  std::string result_route = {};  // 035: the box's routes.result route for the outcome, or ""
  int64_t timeout_ms = 10000;     // one delivery's time
};

// What one box call needs to make external calls.
struct CallContext {
  const manifest::Manifest* manifest = nullptr;
  std::string tenant, domain, request_id;
  std::string route;        // the calling route's name, for !caller_route
  boost::json::value user;  // the request's user, or null
  std::optional<struct SignedIn> who;
  int64_t deadline_us = 0;  // monotonic: no internal call outlives the request (039)
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
  std::vector<std::pair<std::string, std::string>> headers;  // the reply's, names lowercased
  // For the outbox (034 §§ 8, 9): wait until this before the next attempt (Retry-After, a rate
  // limit's reset), and whether the element is polling rather than failing.
  int64_t retry_at_ms = 0;
  bool polling = false;
  std::string header(std::string_view name) const {
    for (const auto& [k, v] : headers) {
      if (k == name) return v;
    }
    return "";
  }
};

// Sends one request with curl; credentials and headers travel in a private config file.
HttpOut send_http(const std::string& verb, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, const std::string& content_type, bool allow_loopback_http,
                  const std::filesystem::path& scratch, int64_t timeout_ms);

}  // namespace goober::host
