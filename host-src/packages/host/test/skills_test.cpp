// Skills and playbooks in the host (design/machine/050.Skills.md): the tools over the operator MCP,
// if_etag, revisions and the change log, the budget, a restart, the operator scope, approvals, the
// panel's reads, and the audit log.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <fstream>

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
  return Reply{int(res.result_int()), res.body()};
}

const char* kOpToken = "op-token-for-skill-tests-0123456789";

bj::object deploy_skill() {
  return bj::object{{"name", "deploy-a-box"},
                    {"title", "Deploy a box"},
                    {"description", "When a box's new version is ready to go live."},
                    {"roles", bj::array{"box-engineer", "operator"}},
                    {"body", "# Steps\n\n1. version_upload with dry_run first.\n2. Watch version_health ten minutes.\n"},
                    {"why", "what the team learned in October"}};
}

}  // namespace

class SkillsHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "skills_host";
    fs::remove_all(root);
    start_host();
    std::string err;
    grok = *host::create_operator_token(root / "storage", "grok", 0, &err);
  }
  void TearDown() override { host.reset(); }
  void start_host(const std::string& limits = "") {
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"acme.example.com": {"tenant": "acme"}},
        "operations": {"token": "op-token-for-skill-tests-0123456789"},)" + limits + R"(
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  bj::object call(const std::string& bearer, const std::string& tool, const bj::object& args = {}) {
    auto r = fetch(port, "acme.example.com", bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer " + bearer}, {"content-type", "application/json"}},
                   json::dump(bj::object{{"jsonrpc", "2.0"},
                                         {"id", 1},
                                         {"method", "tools/call"},
                                         {"params", bj::object{{"name", tool}, {"arguments", args}}}}));
    std::string err;
    auto v = json::parse(r.body, &err);
    if (!v || !v->as_object().contains("result")) return bj::object{{"isError", true}, {"text", r.body}};
    auto res = v->as_object().at("result").as_object();
    bj::object out{{"isError", res.at("isError")}, {"text", res.at("content").at(0).at("text")}};
    if (res.contains("structuredContent")) {
      out["value"] = res.at("structuredContent");
    } else if (auto parsed = json::parse(std::string(res.at("content").at(0).at("text").as_string()), &err)) {
      out["value"] = *parsed;
    }
    return out;
  }
  bj::object op(const std::string& tool, const bj::object& args = {}) { return call(kOpToken, tool, args); }
  static std::string text(const bj::object& r) { return std::string(r.at("text").as_string()); }
  std::string audit_text() {
    std::string all;
    for (const auto& f : logs::files_of(root / "storage/logs", "audit")) {
      std::ifstream in(f);
      std::string l;
      while (std::getline(in, l)) all += l + "\n";
    }
    return all;
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  std::string grok;
};

TEST_F(SkillsHostTest, PutGetListSearchAndDelete) {
  auto args = deploy_skill();
  args["if_etag"] = "none";
  auto put = op("skill_put", args);
  ASSERT_FALSE(put.at("isError").as_bool()) << text(put);
  EXPECT_EQ(put.at("value").at("revision"), 1);
  EXPECT_TRUE(put.at("value").at("created").as_bool());
  EXPECT_TRUE(put.at("value").as_object().contains("change_id")) << text(put);
  std::string etag(put.at("value").at("file_etag").as_string());

  // The stored file is a SKILL.md.
  std::ifstream in(root / "storage/skills/deploy-a-box.md");
  std::string file((std::istreambuf_iterator<char>(in)), {});
  EXPECT_TRUE(file.starts_with("---\nname: deploy-a-box\ndescription: ")) << file;

  auto got = op("skill_get", {{"name", "deploy-a-box"}});
  ASSERT_FALSE(got.at("isError").as_bool()) << text(got);
  const auto& g = got.at("value");
  EXPECT_EQ(g.at("title"), "Deploy a box");
  EXPECT_EQ(g.at("file_etag").as_string(), etag);
  EXPECT_EQ(g.at("file").as_string(), file);
  EXPECT_EQ(g.at("created_by"), "config");
  ASSERT_EQ(g.at("history").as_array().size(), 1u);
  EXPECT_EQ(g.at("history").at(0).at("action"), "create");

  // A second write over a stale etag is refused; with the current one, revision 2.
  auto stale = op("skill_put", args);
  ASSERT_TRUE(stale.at("isError").as_bool());
  EXPECT_NE(text(stale).find("someone changed it"), std::string::npos) << text(stale);
  args["if_etag"] = etag;
  args["body"] = "# Steps\n\n1. route_try first.\n2. version_upload.\n";
  auto again = op("skill_put", args);
  ASSERT_FALSE(again.at("isError").as_bool()) << text(again);
  EXPECT_EQ(again.at("value").at("revision"), 2);
  auto history = op("skill_get", {{"name", "deploy-a-box"}}).at("value").at("history").as_array();
  ASSERT_EQ(history.size(), 2u);
  auto change = op("change_get", {{"id", history.at(0).at("id")}});
  ASSERT_FALSE(change.at("isError").as_bool()) << text(change);
  EXPECT_NE(json::dump(change.at("value")).find("route_try first"), std::string::npos);
  EXPECT_NE(json::dump(change.at("value")).find("dry_run first"), std::string::npos);

  bj::object playbook{{"name", "site-down"},
                      {"title", "The site is down"},
                      {"kind", "playbook"},
                      {"description", "When /goober/ready fails or visitors get 5xx."},
                      {"roles", "operator"},
                      {"body", "Check host_health, then error_report, then box_failures."}};
  ASSERT_FALSE(op("skill_put", playbook).at("isError").as_bool());

  auto list = op("skill_list", {{"role", "operator"}});
  ASSERT_FALSE(list.at("isError").as_bool()) << text(list);
  EXPECT_EQ(list.at("value").at("skills").as_array().size(), 2u);
  EXPECT_EQ(op("skill_list", {{"kind", "playbook"}}).at("value").at("skills").as_array().size(), 1u);
  EXPECT_EQ(op("skill_list", {{"role", "box-engineer"}}).at("value").at("skills").as_array().size(), 1u);
  EXPECT_EQ(list.at("value").at("roles").as_array().size(), 2u);

  auto found = op("skill_search", {{"query", "version upload"}});
  ASSERT_FALSE(found.at("isError").as_bool()) << text(found);
  ASSERT_EQ(found.at("value").at("skills").as_array().size(), 1u);
  EXPECT_EQ(found.at("value").at("skills").at(0).at("name"), "deploy-a-box");
  EXPECT_EQ(op("skill_search", {{"query", "error_report"}}).at("value").at("skills").at(0).at("name"), "site-down");
  EXPECT_TRUE(op("skill_search", {{"query", " ! "}}).at("isError").as_bool());
  EXPECT_TRUE(op("skill_search", {{"query", "x"}, {"kind", "runbook"}}).at("isError").as_bool());

  auto del = op("skill_delete", {{"name", "site-down"}, {"if_etag", "\"0000000000000000\""}});
  ASSERT_TRUE(del.at("isError").as_bool());
  del = op("skill_delete", {{"name", "site-down"}, {"why", "folded into incident-first-hour"}});
  ASSERT_FALSE(del.at("isError").as_bool()) << text(del);
  EXPECT_FALSE(fs::exists(root / "storage/skills/site-down.md"));
  EXPECT_TRUE(op("skill_get", {{"name", "site-down"}}).at("isError").as_bool());
  EXPECT_EQ(op("skill_list").at("value").at("count"), 1);

  auto audit = audit_text();
  EXPECT_NE(audit.find("skill.create"), std::string::npos);
  EXPECT_NE(audit.find("skill.write"), std::string::npos);
  EXPECT_NE(audit.find("skill.delete"), std::string::npos);
  EXPECT_NE(audit.find("folded into incident-first-hour"), std::string::npos);
}

TEST_F(SkillsHostTest, RefusesWhatItCantKeep) {
  auto bad = [&](bj::object args, const std::string& want) {
    auto r = op("skill_put", args);
    EXPECT_TRUE(r.at("isError").as_bool()) << want;
    EXPECT_NE(text(r).find(want), std::string::npos) << text(r);
  };
  auto a = deploy_skill();
  a["name"] = "../../keys/root";
  bad(a, "name");
  a = deploy_skill();
  a["roles"] = bj::array{};
  bad(a, "roles is required");
  a = deploy_skill();
  a["roles"] = bj::array{1};
  bad(a, "roles is a list");
  a = deploy_skill();
  a["body"] = std::string(64 * 1024 + 1, 'x');
  bad(a, "at most 65536");
  EXPECT_FALSE(fs::exists(root / "storage/skills"));
  EXPECT_TRUE(op("skill_delete", {{"name", "nope"}}).at("isError").as_bool());
}

TEST_F(SkillsHostTest, TheBudgetRefusesGrowth) {
  host.reset();
  start_host(R"("limits": {"skills-bytes": 1048576},)");
  auto a = deploy_skill();
  std::string body(60000, 'x');
  int kept = 0;
  for (int i = 0; i < 20; ++i) {
    a["name"] = "skill-" + std::string(1, char('a' + i));
    a["body"] = body;
    auto r = op("skill_put", a);
    if (r.at("isError").as_bool()) {
      EXPECT_NE(text(r).find("limits.skills-bytes"), std::string::npos) << text(r);
      break;
    }
    ++kept;
  }
  EXPECT_GT(kept, 10);
  EXPECT_LT(kept, 20);
  // Shrinking one is never refused.
  a["name"] = "skill-a";
  a["body"] = "short now";
  EXPECT_FALSE(op("skill_put", a).at("isError").as_bool());
  auto health = op("host_health");
  EXPECT_NE(json::dump(health.at("value")).find("skills-bytes"), std::string::npos);
}

TEST_F(SkillsHostTest, SkillsAreReadBackAfterARestart) {
  ASSERT_FALSE(op("skill_put", deploy_skill()).at("isError").as_bool());
  // A file changed by hand into something a put refuses is skipped, and said.
  {
    std::ofstream out(root / "storage/skills/broken.md");
    out << "---\nname: other\n---\nbody\n";
  }
  host.reset();
  start_host();
  auto got = op("skill_get", {{"name", "deploy-a-box"}});
  ASSERT_FALSE(got.at("isError").as_bool()) << text(got);
  EXPECT_EQ(got.at("value").at("revision"), 1);
  EXPECT_TRUE(op("skill_get", {{"name", "broken"}}).at("isError").as_bool());
  EXPECT_NE(json::dump(op("host_health").at("value")).find("skipped skills/broken.md"), std::string::npos);
}

TEST_F(SkillsHostTest, EveryOperatorReadsAndOnlyTheUnfocusedWrite) {
  ASSERT_FALSE(op("skill_put", deploy_skill()).at("isError").as_bool());
  host::Host::OperatorScope acme;
  acme.all = false;
  acme.tenants = {"acme"};
  std::string why;
  ASSERT_TRUE(host->set_operator_scope("token:grok", acme, "test", &why)) << why;
  EXPECT_FALSE(call(grok, "skill_get", {{"name", "deploy-a-box"}}).at("isError").as_bool());
  EXPECT_FALSE(call(grok, "skill_search", {{"query", "deploy"}}).at("isError").as_bool());
  EXPECT_FALSE(call(grok, "skill_list").at("isError").as_bool());
  auto put = deploy_skill();
  put["name"] = "planted";
  EXPECT_TRUE(call(grok, "skill_put", put).at("isError").as_bool());
  EXPECT_TRUE(call(grok, "skill_delete", {{"name", "deploy-a-box"}}).at("isError").as_bool());
  EXPECT_FALSE(fs::exists(root / "storage/skills/planted.md"));

  // Reads are read tools (observers and the panel); writes are changing tools approvals hold.
  for (const char* t : {"skill_list", "skill_search", "skill_get"}) {
    EXPECT_TRUE(host::Host::is_read_tool(t)) << t;
  }
  for (const char* t : {"skill_put", "skill_delete"}) {
    EXPECT_FALSE(host::Host::is_read_call(t, {})) << t;
    EXPECT_TRUE(host::Host::scope_rule(t).has_value()) << t;
  }
}
