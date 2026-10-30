// Approvals per connected agent (design/machine/035.Approvals.md): off by default; a person
// turns them on for one agent in the panel, and that agent's changing tool calls wait as
// requests until a person approves (the call then runs as the agent) or denies them.

#include <algorithm>
#include <iostream>
#include <set>

#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"
#include "goober/json/json.h"
#include "goober/sqlite/sqlite.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr int64_t kPendingTtlMs = 24 * 3600 * 1000;
constexpr size_t kMaxArgsBytes = 1 << 20;
constexpr size_t kMaxResultBytes = 64 << 10;

bool has_role(const SignedIn& who, const char* role) {
  return std::find(who.roles.begin(), who.roles.end(), role) != who.roles.end();
}

// The person an agent acts as: "oauth:ann@x:client" and "panel:ann@x" are ann@x; tokens and the
// config's token are no one's.
std::string person_of(const std::string& agent) {
  if (agent.starts_with("oauth:")) {
    std::string rest = agent.substr(6);
    return rest.substr(0, rest.rfind(':'));
  }
  return "";
}

std::string text_arg(const bj::object& args, const char* k) {
  auto* v = args.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

}  // namespace

sqlite::Db& Host::approvals_db() {
  // Callers hold approvals_mu_.
  if (!approvals_db_) {
    auto dir = config_.get()->storage / "operations";
    fs::create_directories(dir);
    approvals_db_ = std::make_unique<sqlite::Db>(dir / "approvals.db");
    approvals_db_->exec(
        "CREATE TABLE IF NOT EXISTS switches (agent TEXT PRIMARY KEY, set_by TEXT NOT NULL, at_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS agents (agent TEXT PRIMARY KEY, first_ms INTEGER NOT NULL, last_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS requests (id INTEGER PRIMARY KEY AUTOINCREMENT, at_ms INTEGER NOT NULL, agent TEXT NOT "
        "NULL, tool TEXT NOT NULL, args TEXT NOT NULL, request_id TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'pending', "
        "decided_by TEXT, decided_ms INTEGER, result TEXT, is_error INTEGER);"
        "CREATE INDEX IF NOT EXISTS requests_state ON requests (state, id);");
  }
  // Waiting past a day is a no: the agent asks again if it still wants it.
  sqlite::Stmt e(*approvals_db_, "UPDATE requests SET state = 'expired', decided_ms = ? WHERE state = 'pending' AND at_ms < ?");
  int64_t now = base::now_unix_ms();
  e.bind(1, now).bind(2, now - kPendingTtlMs).run();
  return *approvals_db_;
}

bool Host::approvals_on(const std::string& agent) {
  std::lock_guard lock(approvals_mu_);
  sqlite::Stmt s(approvals_db(), "SELECT 1 FROM switches WHERE agent = ?");
  s.bind(1, agent);
  return s.step();
}

void Host::saw_agent(const std::string& agent) {
  int64_t now = base::now_unix_ms();
  std::lock_guard lock(approvals_mu_);
  // At most a write a minute an agent.
  auto& last = agents_seen_[agent];
  if (now - last < 60000) return;
  last = now;
  sqlite::Stmt s(approvals_db(), "INSERT INTO agents (agent, first_ms, last_ms) VALUES (?, ?, ?) "
                                 "ON CONFLICT (agent) DO UPDATE SET last_ms = excluded.last_ms");
  s.bind(1, agent).bind(2, now).bind(3, now).run();
}

std::optional<mcp::ToolResult> Host::hold_for_approval(const std::string& agent, const std::string& tool,
                                                       const bj::object& args) {
  // Looking never waits; nor does asking about one's own requests.
  if (is_read_tool(tool) || tool == "approval_status") return std::nullopt;
  if (!approvals_on(agent)) return std::nullopt;
  std::string text = json::dump(args);
  if (text.size() > kMaxArgsBytes) return mcp::ToolResult::fail("the call is too large to hold for approval (1 MiB)");
  int64_t id = 0;
  {
    std::lock_guard lock(approvals_mu_);
    sqlite::Db& db = approvals_db();
    sqlite::Stmt s(db, "INSERT INTO requests (at_ms, agent, tool, args, request_id) VALUES (?, ?, ?, ?, ?)");
    s.bind(1, base::now_unix_ms()).bind(2, agent).bind(3, tool).bind(4, text).bind(5, current_request_id()).run();
    id = db.last_insert_rowid();
  }
  return mcp::ToolResult::ok(bj::object{
      {"pending_approval", id},
      {"tool", tool},
      {"note", "Approvals are on for you: a person approves or denies this call in the panel (Operators). It hasn't run. "
               "Ask approval_status {id} for the outcome; a request not decided in a day expires."}});
}

bj::object Host::approval_json(sqlite::Stmt& s, bool with_args) {
  bj::object o{{"id", s.int64(0)}, {"at_ms", s.int64(1)}, {"agent", s.text(2)}, {"tool", s.text(3)}, {"state", s.text(6)}};
  if (with_args) {
    std::string err;
    auto a = json::parse(s.text(4), &err);
    o["args"] = a ? *a : bj::value(s.text(4));
  }
  if (!s.is_null(7)) o["decided_by"] = s.text(7);
  if (!s.is_null(8)) o["decided_ms"] = s.int64(8);
  if (!s.is_null(9)) {
    std::string err;
    auto r = json::parse(s.text(9), &err);
    o["result"] = r ? *r : bj::value(s.text(9));
    o["is_error"] = s.int64(10) != 0;
  }
  return o;
}

bj::object Host::panel_approvals(const SignedIn& who) {
  bool owner = has_role(who, "host:owner");
  auto cfg = config_.get();
  std::map<std::string, std::string> client_names;
  for (const auto& c : identity_->clients(identity::kHostRealm)) client_names[c.client_id] = c.name;
  std::lock_guard lock(approvals_mu_);
  sqlite::Db& db = approvals_db();
  std::set<std::string> on;
  {
    sqlite::Stmt s(db, "SELECT agent FROM switches");
    while (s.step()) on.insert(s.text(0));
  }
  // Every connected agent: those that have called, every operator token, and the config's.
  std::map<std::string, bj::object> agents;
  auto add = [&](const std::string& agent, const std::string& kind, const std::string& name, int64_t last_ms) {
    std::string person = person_of(agent);
    bool mine = !person.empty() && person == who.user.email;
    agents[agent] = bj::object{{"agent", agent}, {"kind", kind}, {"name", name}, {"person", person}, {"last_ms", last_ms},
                               {"approvals", on.count(agent) > 0}, {"may_set", owner || mine}};
  };
  {
    sqlite::Stmt s(db, "SELECT agent, last_ms FROM agents ORDER BY last_ms DESC LIMIT 200");
    while (s.step()) {
      std::string a = s.text(0);
      if (a.starts_with("oauth:")) {
        std::string client = a.substr(a.rfind(':') + 1);
        auto n = client_names.find(client);
        add(a, "agent", (n == client_names.end() || n->second.empty() ? client : n->second) + " as " + person_of(a), s.int64(1));
      } else if (a.starts_with("token:") || a == "config") {
        add(a, a == "config" ? "config" : "token", a == "config" ? "the config's operations-token" : a.substr(6), s.int64(1));
      }
    }
  }
  for (const auto& t : list_operator_tokens(cfg->storage)) {
    if (!agents.count("token:" + t.name)) add("token:" + t.name, "token", t.name, 0);
  }
  if (!cfg->operations_token.empty() && !agents.count("config")) add("config", "config", "the config's operations-token", 0);
  for (const auto& a : on) {
    if (!agents.count(a)) add(a, "agent", a, 0);
  }
  bj::array list;
  for (auto& [k, v] : agents) list.push_back(std::move(v));
  bj::array pending, recent;
  {
    sqlite::Stmt s(db, "SELECT id, at_ms, agent, tool, args, request_id, state, decided_by, decided_ms, result, is_error "
                       "FROM requests WHERE state = 'pending' ORDER BY id LIMIT 200");
    while (s.step()) {
      std::string person = person_of(s.text(2));
      if (!owner && person != who.user.email) continue;
      pending.push_back(approval_json(s, true));
    }
  }
  {
    sqlite::Stmt s(db, "SELECT id, at_ms, agent, tool, args, request_id, state, decided_by, decided_ms, result, is_error "
                       "FROM requests WHERE state != 'pending' ORDER BY id DESC LIMIT 30");
    while (s.step()) {
      if (!owner && person_of(s.text(2)) != who.user.email) continue;
      recent.push_back(approval_json(s, false));
    }
  }
  return bj::object{{"agents", std::move(list)}, {"pending", std::move(pending)}, {"recent", std::move(recent)}};
}

std::pair<int, bj::object> Host::panel_set_approvals(const SignedIn& who, const bj::object& args, const std::string& request_id) {
  bool owner = has_role(who, "host:owner");
  if (!owner && !has_role(who, "host:operator")) return {403, bj::object{{"error", "an observer can't change approvals"}}};
  const auto* c = args.if_contains("confirm");
  if (!c || !c->is_bool() || !c->as_bool()) return {400, bj::object{{"error", "confirm the change: {\"confirm\": true}"}}};
  std::string agent = text_arg(args, "agent");
  const auto* onv = args.if_contains("on");
  if (!onv || !onv->is_bool()) return {400, bj::object{{"error", "on is true or false"}}};
  bool known = agent == "config" || agent.starts_with("token:") ||
               (agent.starts_with("oauth:") && agent.find(':', 6) != std::string::npos);
  if (!known || agent.size() > 300) return {400, bj::object{{"error", "agent is oauth:$email:$client, token:$name, or config"}}};
  // An operator sets approvals for their own agents; an owner, for anyone's.
  if (!owner && person_of(agent) != who.user.email) {
    return {403, bj::object{{"error", "only a host:owner sets approvals for another operator's agents or a token"}}};
  }
  {
    std::lock_guard lock(approvals_mu_);
    sqlite::Db& db = approvals_db();
    if (onv->as_bool()) {
      sqlite::Stmt s(db, "INSERT INTO switches (agent, set_by, at_ms) VALUES (?, ?, ?) ON CONFLICT (agent) DO UPDATE SET "
                         "set_by = excluded.set_by, at_ms = excluded.at_ms");
      s.bind(1, agent).bind(2, "panel:" + who.user.email).bind(3, base::now_unix_ms()).run();
    } else {
      sqlite::Stmt s(db, "DELETE FROM switches WHERE agent = ?");
      s.bind(1, agent).run();
    }
  }
  if (changes_) {
    try {
      changes_->record_action(operations::Action{base::now_unix_ms(), "panel:" + who.user.email, "approvals_set", request_id,
                                                 bj::object{{"agent", agent}, {"on", onv->as_bool()}}, false});
    } catch (const std::exception& e) {
      std::cerr << "goober-host: change log: " << e.what() << "\n";
    }
  }
  return {200, bj::object{{"agent", agent}, {"approvals", onv->as_bool()}}};
}

std::pair<int, bj::object> Host::panel_decide(const SignedIn& who, const bj::object& args, const std::string& request_id) {
  bool owner = has_role(who, "host:owner");
  if (!owner && !has_role(who, "host:operator")) return {403, bj::object{{"error", "an observer can't decide"}}};
  const auto* c = args.if_contains("confirm");
  if (!c || !c->is_bool() || !c->as_bool()) return {400, bj::object{{"error", "confirm the decision: {\"confirm\": true}"}}};
  std::string decision = text_arg(args, "decision");
  if (decision != "approve" && decision != "deny") return {400, bj::object{{"error", "decision is approve or deny"}}};
  const auto* idv = args.if_contains("id");
  if (!idv || !idv->is_int64()) return {400, bj::object{{"error", "id is the request's number"}}};
  int64_t id = idv->as_int64();
  std::string agent, tool, args_text, by = "panel:" + who.user.email;
  {
    std::lock_guard lock(approvals_mu_);
    sqlite::Db& db = approvals_db();
    sqlite::Stmt s(db, "SELECT agent, tool, args FROM requests WHERE id = ? AND state = 'pending'");
    s.bind(1, id);
    if (!s.step()) return {404, bj::object{{"error", "no pending request " + std::to_string(id) + " (decided, expired, or none)"}}};
    agent = s.text(0), tool = s.text(1), args_text = s.text(2);
    if (!owner && person_of(agent) != who.user.email) {
      return {403, bj::object{{"error", "only a host:owner decides for another operator's agents or a token"}}};
    }
    // Claimed before it runs, so two people clicking at once can't run it twice.
    sqlite::Stmt u(db, "UPDATE requests SET state = ?, decided_by = ?, decided_ms = ? WHERE id = ? AND state = 'pending'");
    u.bind(1, decision == "approve" ? "running" : "denied").bind(2, by).bind(3, base::now_unix_ms()).bind(4, id).run();
    if (db.changes() != 1) return {409, bj::object{{"error", "someone else decided it first"}}};
  }
  audit(decision == "approve" ? "approval.approved" : "approval.denied", by, tool, "ok",
        bj::object{{"request", id}, {"agent", agent}, {"request_id", request_id}});
  if (decision == "deny") return {200, bj::object{{"id", id}, {"state", "denied"}}};
  // Run as the agent, under the agent's scope and roles as they are now, without holding again.
  std::string err;
  auto parsed = json::parse(args_text, &err);
  bj::object call{{"jsonrpc", "2.0"},
                  {"id", 1},
                  {"method", "tools/call"},
                  {"params", bj::object{{"name", tool}, {"arguments", parsed && parsed->is_object() ? *parsed : bj::object{}}}}};
  auto policy = operator_policy(agent, !may_change(operator_roles(agent)), false);
  std::string saved_op = current_operator(), saved_req = current_request_id();
  current_operator() = agent;
  current_request_id() = request_id;
  auto reply = mcp_->handle(json::dump(call), policy);
  current_operator() = saved_op;
  current_request_id() = saved_req;
  bj::value result = bj::value(reply.body);
  bool is_error = true;
  if (auto r = json::parse(reply.body, &err); r && r->is_object()) {
    const auto& o = r->as_object();
    if (const auto* res = o.if_contains("result"); res && res->is_object()) {
      is_error = res->as_object().contains("isError") && res->as_object().at("isError").as_bool();
      const auto* content = res->as_object().if_contains("content");
      if (content && content->is_array() && !content->as_array().empty()) {
        std::string text(content->as_array()[0].as_object().at("text").as_string());
        auto t = json::parse(text, &err);
        result = t ? *t : bj::value(text);
      }
    } else if (const auto* e = o.if_contains("error")) {
      result = *e;
    }
  }
  std::string stored = json::dump(result);
  if (stored.size() > kMaxResultBytes) stored = json::dump(bj::value(stored.substr(0, kMaxResultBytes) + "…"));
  {
    std::lock_guard lock(approvals_mu_);
    sqlite::Stmt u(approvals_db(), "UPDATE requests SET state = 'done', result = ?, is_error = ? WHERE id = ?");
    u.bind(1, stored).bind(2, int64_t(is_error)).bind(3, id).run();
  }
  // The call itself, in the audit log as any operator call is, naming who approved it.
  audit("operator.tool", agent, tool, is_error ? "failed" : "ok",
        bj::object{{"request_id", request_id}, {"via", "approval"}, {"approved_by", by}, {"request", id}});
  if (changes_) {
    try {
      changes_->record_action(operations::Action{base::now_unix_ms(), agent, tool, request_id,
                                                 bj::object{{"approved_by", by}, {"approval", id}}, is_error});
    } catch (const std::exception& e) {
      std::cerr << "goober-host: change log: " << e.what() << "\n";
    }
  }
  return {200, bj::object{{"id", id}, {"state", "done"}, {"is_error", is_error}, {"result", std::move(result)}}};
}

void Host::register_approval_tools() {
  mcp_->add_tool({"approval_status",
                  "Your calls held for approval (approvals are turned on per agent, by a person in the panel): one by id, or "
                  "your latest 20. Each is pending, done (with its result), denied, or expired (not decided in a day). "
                  "Also says whether approvals are on for you.",
                  mcp::object_schema({{"id", mcp::integer_property("A request's number, from pending_approval.")}}, {}),
                  [this](const bj::object& args) {
                    std::string me = current_operator();
                    bool on = approvals_on(me);
                    std::lock_guard lock(approvals_mu_);
                    sqlite::Db& db = approvals_db();
                    const char* cols = "SELECT id, at_ms, agent, tool, args, request_id, state, decided_by, decided_ms, result, is_error "
                                       "FROM requests WHERE agent = ? ";
                    bj::array out;
                    if (const auto* id = args.if_contains("id"); id && id->is_int64()) {
                      sqlite::Stmt s(db, std::string(cols) + "AND id = ?");
                      s.bind(1, me).bind(2, id->as_int64());
                      if (!s.step()) return mcp::ToolResult::fail("no request " + std::to_string(id->as_int64()) + " of yours");
                      return mcp::ToolResult::ok(approval_json(s, true));
                    }
                    sqlite::Stmt s(db, std::string(cols) + "ORDER BY id DESC LIMIT 20");
                    s.bind(1, me);
                    while (s.step()) out.push_back(approval_json(s, false));
                    return mcp::ToolResult::ok(bj::object{{"approvals_on", on}, {"requests", std::move(out)}});
                  }});
}

}  // namespace goober::host
