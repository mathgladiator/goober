// Logs in the host (design/machine/032.Logs.md): the audit trail, addresses as hashes, the log
// dive within an operator's scope, the chain, live statistics, and the agent audit.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <fstream>
#include <regex>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"
#include "goober/json/json.h"
#include "goober/logs/logs.h"

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

}  // namespace

class LogsHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "logs_host";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"kc.example.com": {"tenant": "kc"}, "other.example.com": {"tenant": "other"}},
        "operations": {"token": "op-token-for-log-tests-0123456789"},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    token = *host::create_operator_token(root / "storage", "grok", 0, &err);
  }
  void TearDown() override { host.reset(); }

  bj::object call(const std::string& bearer, const std::string& tool, const std::string& args = "{}") {
    auto r = fetch(port, "kc.example.com", bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer " + bearer}, {"content-type", "application/json"}},
                   R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}");
    std::string err;
    auto v = json::parse(r.body, &err);
    if (!v || !v->as_object().contains("result")) return bj::object{{"isError", true}, {"text", r.body}};
    auto res = v->as_object().at("result").as_object();
    bj::object out{{"isError", res.at("isError")}, {"text", res.at("content").at(0).at("text")}};
    if (res.contains("structuredContent")) {
      out["value"] = res.at("structuredContent");
    } else if (auto parsed = json::parse(std::string(res.at("content").at(0).at("text").as_string()), &err)) {
      out["value"] = *parsed;  // an array answer
    }
    return out;
  }
  bj::object op(const std::string& tool, const std::string& args = "{}") { return call("op-token-for-log-tests-0123456789", tool, args); }
  std::vector<std::string> lines(const std::string& log) {
    std::vector<std::string> out;
    for (const auto& f : logs::files_of(root / "storage/logs", log)) {
      std::ifstream in(f);
      std::string l;
      while (std::getline(in, l)) out.push_back(l);
    }
    return out;
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  std::string token;
};

TEST_F(LogsHostTest, ToolCallsAuthFailuresAndSignInsAreAudited) {
  op("domain_list");
  call("not-a-token", "domain_list");
  // A sign-in: the address is in the audit log only as a hash.
  auto start = fetch(port, "kc.example.com", bhttp::verb::post, "/identity/email/start",
                     {{"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://kc.example.com"}},
                     "email=ann@example.com");
  host->drain_mail();
  std::string code;
  for (const auto& f : base::list_files(root / "storage/mail/outbox")) {
    std::smatch m;
    auto text = *base::read_file(root / "storage/mail/outbox" / f);
    if (std::regex_search(text, m, std::regex("is ([0-9]{6})"))) code = m[1];
  }
  fetch(port, "kc.example.com", bhttp::verb::post, "/identity/email/verify",
        {{"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://kc.example.com"}},
        "email=ann@example.com&code=" + code);
  auto audit = lines("audit");
  std::string all;
  for (const auto& l : audit) all += l + "\n";
  EXPECT_NE(all.find(R"("action":"host.start")"), std::string::npos);
  EXPECT_NE(all.find(R"("action":"operator.tool","actor":"config","target":"domain_list","outcome":"ok")"), std::string::npos) << all;
  EXPECT_NE(all.find(R"("action":"operator.auth_failed")"), std::string::npos);
  EXPECT_NE(all.find(R"("action":"identity.sign_in")"), std::string::npos);
  EXPECT_EQ(all.find("ann@example.com"), std::string::npos) << all;  // never the address
  EXPECT_NE(all.find(R"("email_domain":"example.com")"), std::string::npos);
  // The log dive finds it by the address, and the chain holds.
  auto found = op("log_search", R"({"log":"audit","email":"Ann@Example.com"})");
  ASSERT_FALSE(found.at("isError").as_bool()) << found.at("text");
  EXPECT_EQ(found.at("value").at("count").as_int64(), 1);
  auto verified = op("log_verify", R"({})");
  EXPECT_TRUE(verified.at("value").at("ok").as_bool()) << verified.at("text");
  // Every line carries the envelope.
  for (const auto& l : lines("access")) {
    EXPECT_TRUE(l.starts_with(R"({"at":")")) << l;
    EXPECT_NE(l.find(R"("log":"access","seq":)"), std::string::npos) << l;
  }
}

TEST_F(LogsHostTest, TheDiveIsGrepWithinAWindowAndAScope) {
  std::string err;
  ASSERT_TRUE(host->content().put("kc", "index.md", "# KC", &err));
  ASSERT_TRUE(host->content().put("other", "index.md", "# Other", &err));
  fetch(port, "kc.example.com", bhttp::verb::get, "/");
  fetch(port, "other.example.com", bhttp::verb::get, "/");
  fetch(port, "kc.example.com", bhttp::verb::get, "/missing-page");
  auto by_field = op("log_search", R"({"log":"access","fields":{"status":"404"}})");
  ASSERT_EQ(by_field.at("value").at("count").as_int64(), 1) << by_field.at("text");
  auto rx = op("log_search", R"({"log":"access","regex":"\"path\":\"/missing-[a-z]+\""})");
  EXPECT_EQ(rx.at("value").at("count").as_int64(), 1);
  EXPECT_EQ(op("log_search", R"({"log":"access","since":"2020-01-01T00:00:00Z","until":"2020-01-02T00:00:00Z"})")
                .at("value").at("count").as_int64(), 0);
  EXPECT_TRUE(op("log_search", R"({"log":"../etc/passwd"})").at("isError").as_bool());
  // A focused operator sees only their tenant's lines.
  host::Host::OperatorScope kc;
  kc.all = false;
  kc.tenants = {"kc"};
  ASSERT_TRUE(host->set_operator_scope("token:grok", kc, "test", &err)) << err;
  auto mine = call(token, "log_search", R"({"log":"access","pattern":"\"path\":\"/\""})");
  ASSERT_FALSE(mine.at("isError").as_bool()) << mine.at("text");
  for (const auto& l : mine.at("value").at("lines").as_array()) {
    EXPECT_NE(std::string(l.as_string()).find(R"("tenant":"kc")"), std::string::npos) << l;
  }
  EXPECT_GE(mine.at("value").at("count").as_int64(), 1);
  EXPECT_TRUE(call(token, "log_verify").at("isError").as_bool());  // the whole host's: not theirs
}

TEST_F(LogsHostTest, LiveStatsTheSiteTreeAndFlow) {
  std::string err;
  ASSERT_TRUE(host->content().put("kc", "index.md", "# KC", &err));
  ASSERT_TRUE(host->content().put("kc", "places/joes.md", "---\ntitle: Joe's\n---\nBBQ", &err));
  ASSERT_TRUE(host->content().put("kc", "css/site.css", "body{}", &err));
  for (int i = 0; i < 5; ++i) fetch(port, "kc.example.com", bhttp::verb::get, "/");
  fetch(port, "kc.example.com", bhttp::verb::get, "/places/joes", {{"referer", "http://kc.example.com/"}});
  auto stats = op("uri_stats", R"({"tenant":"kc","window":"5m","sort":"count"})");
  ASSERT_FALSE(stats.at("isError").as_bool()) << stats.at("text");
  const auto& rows = stats.at("value").as_array();
  ASSERT_GE(rows.size(), 2u);
  EXPECT_EQ(rows[0].at("path"), "/");
  EXPECT_EQ(rows[0].at("n").as_int64(), 5);
  EXPECT_GT(rows[0].at("p99_us").as_int64(), 0);
  auto tree = op("uri_tree", R"({"tenant":"kc"})");
  std::string t(tree.at("text").as_string());
  EXPECT_NE(t.find("\"name\": \"joes\""), std::string::npos) << t;
  EXPECT_NE(t.find("\"kind\": \"file\""), std::string::npos);  // css/site.css, never visited
  auto flow = op("uri_flow", R"({"tenant":"kc","path":"/places/joes"})");
  EXPECT_EQ(flow.at("value").at("in").at(0).at("from"), "/");
}

TEST_F(LogsHostTest, AnAgentGoingHamIsFlagged) {
  for (int i = 0; i < 70; ++i) call(token, "content_put", R"({"tenant":"kc","path":"spam.txt","text":"x"})");
  auto a = op("agent_audit", R"({"window":"1h"})");
  ASSERT_FALSE(a.at("isError").as_bool()) << a.at("text");
  const auto& actors = a.at("value").at("actors").as_array();
  ASSERT_FALSE(actors.empty());
  EXPECT_EQ(actors[0].at("actor"), "token:grok");
  std::string flags = json::dump(actors[0].at("flags"));
  EXPECT_NE(flags.find("burst"), std::string::npos) << flags;
  EXPECT_NE(flags.find("looping"), std::string::npos) << flags;
  EXPECT_EQ(actors[0].at("writes").as_int64(), 70);
}
