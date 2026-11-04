// MCP: a strict rate and a fixed queue (design/machine/047.Controls.md § 13). The human, on
// 2026-11-03: "we should assume agents may behave badly. Beyond a strict rate limit, MCP should
// also have a queue for operations to front-load. The queue should be fixed such that we aim to
// maximize reliability." Every call to a domain's /mcp and to the operator MCP comes through
// mcp_enter on the network thread: it is counted against its agent's strict rate, then runs on
// the pool, waits its turn as a parked continuation (no worker held), or is refused with "rate
// limit reached". The rate, the queue, and the counting are the controls package's (queue.h);
// this file names agents, posts to the pool, answers refusals, and reports.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"
#include "goober/controls/report.h"
#include "goober/host/host.h"
#include "http_util.h"

namespace goober::host {
namespace bj = boost::json;
using server::http::Request;
using server::http::Response;

namespace {

constexpr int64_t kAgentKeptMs = 10 * 60 * 1000;
constexpr size_t kAgentsMax = 10000;
constexpr int64_t kSweepMs = 250;
constexpr int64_t kRowsEveryMs = 5000;

std::string bearer_of(const Request& req) {
  std::string auth = req.header("authorization");
  if (auth.size() > 7 && base::to_lower(auth.substr(0, 7)) == "bearer ") return auth.substr(7);
  return "";
}

// The refusal, the same for every reason: the status, Retry-After, and "rate limit reached" as
// a JSON-RPC error answering the request's id when it had one.
Response refusal(int status, int64_t retry_after_s, const std::string& raw_id) {
  std::string id = "null";
  if (!raw_id.empty()) {
    std::string err;
    if (json::parse(raw_id, &err)) id = raw_id;
  }
  Response res;
  res.status = status;
  res.headers.emplace_back("content-type", "application/json");
  res.headers.emplace_back("cache-control", "no-store");
  res.headers.emplace_back("retry-after", std::to_string(std::max<int64_t>(1, retry_after_s)));
  res.body = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":-32000,\"message\":\"" +
             std::string(controls::kRefusalText) + "\"}}";
  return res;
}

const char* refusal_name(controls::Refusal r) {
  switch (r) {
    case controls::Refusal::kFull: return "full";
    case controls::Refusal::kWaited: return "waited";
    case controls::Refusal::kGone: return "gone";
    case controls::Refusal::kStopping: return "stopping";
  }
  return "full";
}

// Adds "mcp_queue": note to an access line's extra (a JSON object's text, or "").
std::string with_queue(const std::string& extra, const std::string& note) {
  if (extra.size() < 2 || extra.front() != '{' || extra.back() != '}' || extra == "{}") return "{\"mcp_queue\":" + note + "}";
  return extra.substr(0, extra.size() - 1) + ",\"mcp_queue\":" + note + "}";
}

}  // namespace

std::pair<std::string, std::string> Host::mcp_agent(const Request& req, const std::string& realm) {
  std::string token = bearer_of(req);
  if (!token.empty()) {
    std::string key = base::sha256_hex(realm + "\n" + token);
    std::lock_guard lock(mcp_agents_mu_);
    auto it = mcp_agents_.find(key);
    if (it != mcp_agents_.end() && base::now_unix_ms() - it->second.first < kAgentKeptMs) return it->second.second;
  }
  // Nobody proved yet (no token, or one not yet seen good): the client's address, as the
  // host-wide limit keys it. A made-up token never opens a fresh bucket.
  return {"addr:" + rate_key(client_address(req)), "unknown"};
}

void Host::mcp_remember(const Request& req, const std::string& realm, const std::string& agent, const std::string& cls) {
  std::string token = bearer_of(req);
  if (token.empty()) return;
  std::string key = base::sha256_hex(realm + "\n" + token);
  int64_t now = base::now_unix_ms();
  std::lock_guard lock(mcp_agents_mu_);
  if (mcp_agents_.size() >= kAgentsMax && !mcp_agents_.count(key)) {
    std::erase_if(mcp_agents_, [&](const auto& kv) { return now - kv.second.first >= kAgentKeptMs; });
    if (mcp_agents_.size() >= kAgentsMax) mcp_agents_.clear();
  }
  mcp_agents_[key] = {now, {"id:" + agent, cls}};
}

void Host::mcp_enter(controls::QueueKind kind, const Request& req, const Finish& finish, const std::string& domain,
                     const std::string& tenant, std::function<void(const Finish&)> handle) {
  const auto& limits = controls::limits_of(kind);
  auto [agent, cls] = mcp_agent(req, kind == controls::QueueKind::kOperator ? std::string("operator") : tenant);
  auto calls = controls::count_rpc_calls(req.body);
  // A batch bigger than the burst could never pass the rate: say so, rather than refuse it forever.
  if (calls.batch && calls.calls() > int(limits.burst)) {
    Response res;
    res.status = 400;
    res.headers.emplace_back("content-type", "application/json");
    res.headers.emplace_back("cache-control", "no-store");
    res.body = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"a batch holds at most " +
               std::to_string(int(limits.burst)) + " calls\"}}";
    return finish(std::move(res), R"({"why":"batch too large","mcp_queue":"batch"})");
  }
  int64_t entered = base::now_unix_ms();
  controls::Call call;
  call.kind = kind;
  call.scope = kind == controls::QueueKind::kMcp ? domain : "";
  call.tenant = tenant;
  call.agent = agent;
  call.cls = cls;
  call.calls = calls;
  call.bytes = int64_t(req.body.size());
  call.gone = [gone = req.peer_gone] { return gone && gone->load(); };
  call.run = [this, finish, handle = std::move(handle), entered, id = calls.id](std::shared_ptr<controls::Slot> slot) {
    int64_t waited = base::now_unix_ms() - entered;
    // The slot goes with the finish: given back when it is called, or when the last copy of it
    // is dropped (a handler that threw), so every path out of a call frees its slot.
    Finish wrapped = [finish, slot, waited](Response res, std::string extra) {
      slot->release();
      finish(std::move(res), waited > 0 ? with_queue(extra, "{\"waited_ms\":" + std::to_string(waited) + "}") : extra);
    };
    if (!pool_->post([handle, wrapped] { handle(wrapped); }, max_queued_)) {
      slot->release();
      finish(refusal(503, 1, id), R"({"why":"rate limit reached","mcp_queue":"pool"})");
    }
  };
  call.refuse = [finish, id = calls.id](controls::Refusal r) {
    finish(refusal(503, 1, id), "{\"why\":\"rate limit reached\",\"mcp_queue\":\"" + std::string(refusal_name(r)) + "\"}");
  };
  auto admitted = mcp_queue_->admit(std::move(call));
  if (admitted.outcome == controls::Outcome::kRate) {
    return finish(refusal(429, admitted.retry_after_s, calls.id), R"({"why":"rate limit reached","mcp_queue":"rate"})");
  }
  if (admitted.outcome == controls::Outcome::kFull) {
    return finish(refusal(503, admitted.retry_after_s, calls.id), R"({"why":"rate limit reached","mcp_queue":"full"})");
  }
}

void Host::mcp_queue_start() {
  {
    std::lock_guard lock(mcp_sweep_mu_);
    mcp_sweep_stopping_ = false;
  }
  mcp_sweep_thread_ = std::thread([this] {
    int64_t rows_at = base::now_unix_ms();
    std::unique_lock lock(mcp_sweep_mu_);
    while (!mcp_sweep_stopping_) {
      lock.unlock();
      try {
        mcp_queue_->sweep();
        if (int64_t now = base::now_unix_ms(); now - rows_at >= kRowsEveryMs) {
          rows_at = now;
          if (traffic_) traffic_->add_mcp_queue_rows(mcp_queue_->take_closed());
        }
      } catch (const std::exception& e) {
        std::cerr << "goober-host: mcp queue: " << e.what() << "\n";
      }
      lock.lock();
      mcp_sweep_cv_.wait_for(lock, std::chrono::milliseconds(kSweepMs), [this] { return mcp_sweep_stopping_; });
    }
  });
}

void Host::mcp_queue_stop() {
  {
    std::lock_guard lock(mcp_sweep_mu_);
    mcp_sweep_stopping_ = true;
  }
  mcp_sweep_cv_.notify_all();
  if (mcp_sweep_thread_.joinable()) mcp_sweep_thread_.join();
  if (mcp_queue_) mcp_queue_->stop();
}

void Host::mcp_queue_flush() {
  try {
    if (mcp_queue_ && traffic_) traffic_->add_mcp_queue_rows(mcp_queue_->take_all());
  } catch (const std::exception& e) {
    std::cerr << "goober-host: mcp queue rollups: " << e.what() << "\n";
  }
}

bj::object Host::mcp_queue_report(const std::string& domain, const std::string& window) {
  auto kind = domain.empty() ? controls::QueueKind::kOperator : controls::QueueKind::kMcp;
  int64_t now = base::now_unix_ms();
  std::vector<operations::McpQueueRow> rows;
  if (traffic_) {
    traffic_->add_mcp_queue_rows(mcp_queue_->take_closed());
    traffic_->catch_up(now);
    rows = traffic_->mcp_queue_rows(std::string(controls::queue_kind_name(kind)), domain, now - 8 * 24 * 3600 * 1000LL,
                                    now + controls::kBucketMs);
  }
  for (auto& r : mcp_queue_->pending_rows(kind, domain)) rows.push_back(std::move(r));
  auto out = controls::queue_report(kind, domain, std::move(rows), window, now, config_.get()->box_threads);
  auto s = mcp_queue_->stats(kind, domain);
  out["now"] = bj::object{{"running", s.running}, {"waiting", s.waiting}};
  return out;
}

void Host::register_mcp_queue_tools() {
  mcp_->add_tool(
      {"mcp_queue_report",
       "Read tool. What MCP's strict rate and fixed queue did (every agent gets a strict rate and a fixed queue: calls "
       "past an agent's running limit wait their turn instead of being refused): per five-minute point over window (1h, "
       "12h, 7d), calls offered, queued (waited for a slot), ran, refused (rate, full, waited too long), gone (the client "
       "left while waiting), peak waiting and running, and wait p50/p99; totals and by class of agent (agent, person, "
       "unknown, operator); the fixed limits; and verdicts in plain words. With domain, that domain's /mcp; without, the "
       "operator MCP. Agents are never named.",
       mcp::object_schema(
           {{"domain", mcp::string_property("A domain, as domain_list names it, for its /mcp. Omit for the operator MCP.")},
            {"window", mcp::enum_property("How far back: 1h, 12h (the default), or 7d.", {"1h", "12h", "7d"})}},
           {}),
       [this](const bj::object& args) {
         std::string domain;
         if (const auto* d = args.if_contains("domain"); d && d->is_string()) domain = base::to_lower(std::string(d->as_string()));
         std::string window = "12h";
         if (const auto* w = args.if_contains("window"); w && w->is_string()) window = std::string(w->as_string());
         if (!controls::valid_report_window(window)) return mcp::ToolResult::fail("window is 1h, 12h, or 7d");
         if (!domain.empty() && !config_.get()->domains.count(domain)) {
           return mcp::ToolResult::fail("no domain " + domain + " on this host (domain_list names them)");
         }
         return mcp::ToolResult::ok(mcp_queue_report(domain, window));
       }});
}

}  // namespace goober::host
