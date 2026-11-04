// Cleanup (design/machine/051.Cleanup.md): an agent's box_drop and db_drop always wait for a
// person, only a host:owner approves one (typing the name), the checks run again when approved,
// a dry run only looks, a person drops in the panel directly, and what each drop leaves.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <regex>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

struct Reply {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};

const std::string kOps = "ops.example.com";

Reply fetch(uint16_t port, const std::string& host, bhttp::verb verb, const std::string& target,
            std::vector<std::pair<std::string, std::string>> headers = {}, const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, host);
  for (const auto& [k, v] : headers) req.set(k, v);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  Reply r{int(res.result_int()), res.body(), {}};
  for (const auto& f : res) r.headers[base::to_lower(std::string(f.name_string()))] = std::string(f.value());
  return r;
}

bj::value parse(const std::string& s) {
  std::string err;
  auto v = json::parse(s, &err);
  return v ? *v : bj::value(nullptr);
}

const std::vector<std::pair<std::string, std::string>> kForm = {{"content-type", "application/x-www-form-urlencoded"}};

std::string module_of(std::vector<std::string> routes) {
  std::string wat = "(module (memory (export \"memory\") 1)";
  for (const auto& r : routes) wat += " (func (export \"goober_route_" + r + "\") (result i32) (i32.const 0))";
  return wat + ")";
}

const char* kSchema = R"({"schema":"items1","tables":{"items":{"columns":{"name":{"type":"string","max-length":40}}}}})";
const char* kShop = R"({"box":"shop","domains":["acme.example.com"],
  "data":{"sqlite3":{"w":{"schema":"items1","database":"items","pattern":"sql","tables":{"items":{"read":true,"insert":true}}}}},
  "routes":{"http":{"/shop":{"get":{"name":"shop","public":true}}}}})";
const char* kBilling = R"({"box":"billing","domains":["*"],
  "routes":{"internal":{"/ping":{"get":{"name":"ping","callers":["jobs"]}}}}})";
const char* kJobs = R"({"box":"jobs","domains":["*"],
  "external":{"billing":{"box":"billing","methods":{"ping":{}}}},
  "routes":{"http":{"/run":{"post":{"name":"run","public":true}}}}})";

}  // namespace

class CleanupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "cleanup";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"acme.example.com": {"tenant": "acme"}, "localhost": {"tenant": "acme"}},
        "operations": {"domain": "ops.example.com"},
        "identity": {"operators": {"boss@example.com": ["host:owner"], "helper@example.com": ["host:operator"],
                                   "watcher@example.com": ["host:observer"]}},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    token = *host::create_operator_token(root / "storage", "grok", 0, &err);
    ASSERT_TRUE(host->schemas().put(kSchema, &err)) << err;
    auto schema = host->schemas().get("items1");
    ASSERT_TRUE(host->databases().open("acme", "items", &*schema, &err)) << err;
    ASSERT_TRUE(host->boxes().upload(kShop, module_of({"shop"}), &err)) << err;
    ASSERT_TRUE(host->boxes().upload(kBilling, module_of({"ping"}), &err)) << err;
    ASSERT_TRUE(host->boxes().upload(kJobs, module_of({"run"}), &err)) << err;
  }
  void TearDown() override { host.reset(); }

  std::string sign_in(const std::string& email) {
    fetch(port, kOps, bhttp::verb::post, "/identity/email/start", kForm, "email=" + email);
    host->drain_mail();
    auto files = base::list_files(root / "storage/mail/outbox");
    std::sort(files.begin(), files.end());
    std::string code;
    for (auto it = files.rbegin(); it != files.rend() && code.empty(); ++it) {
      auto text = *base::read_file(root / "storage/mail/outbox" / *it);
      std::smatch m;
      if (text.find("To: " + email) != std::string::npos && std::regex_search(text, m, std::regex("is ([0-9]{6})"))) code = m[1];
    }
    auto v = fetch(port, kOps, bhttp::verb::post, "/identity/email/verify", kForm, "email=" + email + "&code=" + code);
    return v.headers["set-cookie"].substr(0, v.headers["set-cookie"].find(';'));
  }
  Reply api(const std::string& cookie, const std::string& tool, const bj::object& args = {}) {
    return fetch(port, kOps, bhttp::verb::post, "/api/" + tool,
                 {{"cookie", cookie}, {"content-type", "application/json"}, {"origin", "http://" + kOps}}, json::dump(args));
  }
  bj::object call(const std::string& tool, const bj::object& args = {}) {
    auto r = fetch(port, "localhost", bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer " + token}, {"content-type", "application/json"}},
                   json::dump(bj::object{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                         {"params", bj::object{{"name", tool}, {"arguments", args}}}}));
    auto v = parse(r.body);
    if (!v.is_object() || !v.as_object().contains("result")) return bj::object{{"isError", true}, {"text", r.body}};
    auto res = v.as_object().at("result").as_object();
    res["text"] = res.at("content").as_array().at(0).at("text");
    res["value"] = parse(std::string(res.at("text").as_string()));
    return res;
  }
  int64_t pending(const bj::object& r) {
    const auto& v = r.at("value");
    return v.is_object() && v.as_object().contains("pending_approval") ? v.at("pending_approval").as_int64() : 0;
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  std::string token;
};

TEST_F(CleanupTest, AnAgentsBoxDropWaitsForAnOwnerWhoTypesItsName) {
  // Approvals are off for grok, and the drop waits anyway.
  auto asked = call("box_drop", {{"box", "shop"}, {"why", "migrated to the new shop"}});
  ASSERT_FALSE(asked.at("isError").as_bool()) << asked.at("text");
  int64_t id = pending(asked);
  ASSERT_GT(id, 0) << asked.at("text");
  EXPECT_NE(std::string(asked.at("text").as_string()).find("Cleanup view"), std::string::npos);
  EXPECT_TRUE(host->boxes().active()->boxes.count("shop"));

  auto boss = sign_in("boss@example.com"), helper = sign_in("helper@example.com"), watcher = sign_in("watcher@example.com");
  // The panel shows it with what it would take, checked now.
  auto list = parse(api(boss, "approvals").body);
  const auto& held = list.at("result").at("pending").at(0);
  EXPECT_EQ(held.at("tool"), "box_drop");
  EXPECT_EQ(held.at("drop").at("state"), "live");
  EXPECT_EQ(held.at("drop").at("databases").at(0).at("database"), "items");
  EXPECT_TRUE(held.at("drop").at("blocked").as_array().empty());

  bj::object approve{{"id", id}, {"decision", "approve"}, {"confirm", true}};
  EXPECT_EQ(api(helper, "write/decide", approve).status, 403);   // an operator can't approve a drop
  EXPECT_EQ(api(watcher, "write/decide", approve).status, 403);  // nor an observer
  EXPECT_EQ(api(boss, "write/decide", approve).status, 400);     // the owner must type the name
  approve["confirm_name"] = "shop";
  auto done = api(boss, "write/decide", approve);
  ASSERT_EQ(done.status, 200) << done.body;
  auto res = parse(done.body).at("result");
  EXPECT_FALSE(res.at("is_error").as_bool()) << done.body;
  EXPECT_EQ(res.at("result").at("dropped"), "shop");
  EXPECT_EQ(res.at("result").at("databases_unbound").at(0).at("database"), "items");

  EXPECT_FALSE(host->boxes().active()->boxes.count("shop"));
  EXPECT_TRUE(host->boxes().dropped("shop"));
  EXPECT_EQ(fetch(port, "acme.example.com", bhttp::verb::get, "/shop").status, 404);
  auto audit = *base::read_file(root / "storage/logs/audit.log");
  EXPECT_NE(audit.find("drop.box"), std::string::npos);
  EXPECT_NE(audit.find("\"approved_by\":\"panel:boss@example.com\""), std::string::npos);
  auto report = call("cleanup_report");
  EXPECT_EQ(report.at("value").at("dropped").at(0).at("box"), "shop");
  EXPECT_EQ(report.at("value").at("dropped").at(0).at("approved_by"), "panel:boss@example.com");
  // The database is now unbound: the next drop can go.
  auto check = call("db_drop", {{"tenant", "acme"}, {"database", "items"}, {"dry_run", true}});
  EXPECT_TRUE(check.at("value").at("blocked").as_array().empty()) << check.at("text");
}

TEST_F(CleanupTest, ADatabaseDropWaitsChecksAgainAndSnapshotsFirst) {
  // While shop binds it, a dry run says so, and asking still waits; approving it is refused.
  auto check = call("db_drop", {{"tenant", "acme"}, {"database", "items"}, {"dry_run", true}});
  ASSERT_FALSE(check.at("isError").as_bool()) << check.at("text");
  EXPECT_FALSE(check.at("value").as_object().contains("pending_approval"));
  ASSERT_EQ(check.at("value").at("blocked").as_array().size(), 1u) << check.at("text");
  int64_t id = pending(call("db_drop", {{"tenant", "acme"}, {"database", "items"}, {"why", "old"}}));
  ASSERT_GT(id, 0);
  auto boss = sign_in("boss@example.com");
  bj::object approve{{"id", id}, {"decision", "approve"}, {"confirm", true}, {"confirm_name", "items"}};
  auto refused = api(boss, "write/decide", approve);
  ASSERT_EQ(refused.status, 200) << refused.body;
  EXPECT_TRUE(parse(refused.body).at("result").at("is_error").as_bool()) << refused.body;
  EXPECT_TRUE(host->databases().exists("acme", "items"));

  // The person drops the box in the panel, then approves a new request.
  EXPECT_EQ(api(boss, "write/drop", {{"kind", "box"}, {"box", "shop"}, {"confirm", "shop"}, {"why", "tidy"}}).status, 200);
  id = pending(call("db_drop", {{"tenant", "acme"}, {"database", "items"}, {"why", "old"}}));
  approve["id"] = id;
  auto done = api(boss, "write/decide", approve);
  ASSERT_EQ(done.status, 200) << done.body;
  auto res = parse(done.body).at("result");
  EXPECT_FALSE(res.at("is_error").as_bool()) << done.body;
  EXPECT_TRUE(res.at("result").as_object().contains("snapshot"));
  EXPECT_FALSE(host->databases().exists("acme", "items"));
}

TEST_F(CleanupTest, ABoxAnotherBoxCallsIsntDropped) {
  auto check = call("box_drop", {{"box", "billing"}, {"dry_run", true}});
  ASSERT_FALSE(check.at("isError").as_bool()) << check.at("text");
  EXPECT_EQ(check.at("value").at("callers").at(0), "jobs");
  EXPECT_FALSE(check.at("value").at("blocked").as_array().empty());
  auto boss = sign_in("boss@example.com");
  auto r = api(boss, "write/drop", {{"kind", "box"}, {"box", "billing"}, {"confirm", "billing"}});
  EXPECT_EQ(r.status, 409) << r.body;
  EXPECT_NE(r.body.find("jobs calls it"), std::string::npos) << r.body;
  // Dropping the caller first frees it.
  EXPECT_EQ(api(boss, "write/drop", {{"kind", "box"}, {"box", "jobs"}, {"confirm", "jobs"}}).status, 200);
  EXPECT_EQ(api(boss, "write/drop", {{"kind", "box"}, {"box", "billing"}, {"confirm", "billing"}}).status, 200);
  // Only an owner drops in the panel.
  auto helper = sign_in("helper@example.com");
  EXPECT_EQ(api(helper, "write/drop", {{"kind", "box"}, {"box", "shop"}, {"confirm", "shop"}}).status, 403);
}

TEST_F(CleanupTest, ADropNeverRunsWithoutAPersonAndIsScoped) {
  EXPECT_TRUE(host::Host::always_held("box_drop", {{"box", "x"}}));
  EXPECT_FALSE(host::Host::always_held("box_drop", {{"box", "x"}, {"dry_run", true}}));
  EXPECT_TRUE(host::Host::is_read_call("db_drop", {{"dry_run", true}}));
  EXPECT_FALSE(host::Host::is_read_call("db_drop", {}));
  EXPECT_TRUE(host::Host::is_read_tool("cleanup_report"));
  for (const char* t : {"box_drop", "db_drop", "cleanup_report"}) EXPECT_TRUE(host::Host::scope_rule(t).has_value()) << t;
  // Denied, nothing happens.
  int64_t id = pending(call("box_drop", {{"box", "shop"}, {"why", "x"}}));
  auto boss = sign_in("boss@example.com");
  EXPECT_EQ(api(boss, "write/decide", {{"id", id}, {"decision", "deny"}, {"confirm", true}}).status, 200);
  EXPECT_TRUE(host->boxes().active()->boxes.count("shop"));
}
