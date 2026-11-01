// Roles a box grants and revokes (013.Identity.md § Granting and revoking roles; decided by the
// human on 2026-10-31): a route's "grantable" and "revocable" options, goober_role_grant and
// goober_role_revoke, applied when the call commits, audited, and gating pages.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <regex>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/manifest/manifest.h"

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

Reply fetch(uint16_t port, bhttp::verb verb, const std::string& target,
            std::vector<std::pair<std::string, std::string>> headers = {}, const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "shop.example.com");
  for (const auto& [k, v] : headers) req.set(k, v);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response_parser<bhttp::string_body> parser;
  bhttp::read(stream, buf, parser);
  auto res = parser.release();
  Reply r{int(res.result_int()), res.body(), {}};
  for (const auto& f : res) r.headers[base::to_lower(std::string(f.name_string()))] = std::string(f.value());
  return r;
}

// Each route asks for one role change and answers 204 when the host took it, 409 when it refused.
const char* kWat = R"wat((module
  (import "goober" "goober_role_grant" (func $grant (param i32 i32) (result i32)))
  (import "goober" "goober_role_revoke" (func $revoke (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "text/plain")
  (data (i32.const 32) "consented")
  (data (i32.const 64) "admin")
  (data (i32.const 96) "member")
  (func $answer (param $rc i32) (result i32)
    (drop (call $respond (select (i32.const 204) (i32.const 409) (i32.eqz (local.get $rc)))
                         (i32.const 0) (i32.const 10) (i32.const 0) (i32.const 0)))
    (i32.const 0))
  (func (export "goober_route_consent") (result i32) (call $answer (call $grant (i32.const 32) (i32.const 9))))
  (func (export "goober_route_withdraw") (result i32) (call $answer (call $revoke (i32.const 32) (i32.const 9))))
  (func (export "goober_route_sneak") (result i32) (call $answer (call $grant (i32.const 64) (i32.const 5))))
  (func (export "goober_route_promote") (result i32) (call $answer (call $grant (i32.const 96) (i32.const 6))))
  (func (export "goober_route_flood") (result i32)
    (local $i i32) (local $rc i32)
    (loop $again
      (local.set $rc (call $grant (i32.const 32) (i32.const 9)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (i32.const 17))))
    (call $answer (local.get $rc)))
  (func (export "goober_route_failing") (result i32)
    (drop (call $grant (i32.const 32) (i32.const 9)))
    (drop (call $respond (i32.const 500) (i32.const 0) (i32.const 10) (i32.const 0) (i32.const 0)))
    (i32.const 0))
))wat";

const char* kManifest = R"({"box":"gate","domains":["*"],"routes":{"http":{
  "/consent":{
    "post":{"name":"consent","requirements":[],"grantable":{"options":["consented"],"to":"{!user_id}"}},
    "delete":{"name":"withdraw","requirements":[],
              "revocable":{"options":["consented"],"to":"{!user_id}","allow-self-revoke":true}}},
  "/sneak":{"post":{"name":"sneak","requirements":[],"grantable":{"options":["consented"],"to":"{!user_id}"}}},
  "/flood":{"post":{"name":"flood","requirements":[],"grantable":{"options":["consented"],"to":"{!user_id}"}}},
  "/failing":{"post":{"name":"failing","requirements":[],"grantable":{"options":["consented"],"to":"{!user_id}"}}},
  "/members/{to : int64}/promote":{"post":{"name":"promote","requirements":[["owner"]],
                                           "grantable":{"options":["member"],"to":"{to}"}}}}}})";

}  // namespace

class RolesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "roles";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"shop.example.com": {"tenant": "shop"}}, "threads": {"network": 1, "boxes": 2},
        "operations": {"token": "op-token-for-roles-tests-0123456789"}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }

  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer op-token-for-roles-tests-0123456789"}, {"content-type", "application/json"}},
                   json::dump(req));
    std::string err;
    auto doc = json::parse(r.body, &err);
    if (!doc || !doc->as_object().contains("result")) {
      ADD_FAILURE() << name << ": " << r.status << " " << r.body;
      return bj::object{{"error", r.body}};
    }
    const auto& result = doc->at("result").as_object();
    if (result.at("isError").as_bool()) return bj::object{{"error", result.at("content").at(0).at("text")}};
    return result.at("structuredContent");
  }
  std::string sign_in(const std::string& email) {
    fetch(port, bhttp::verb::post, "/identity/email/start",
          {{"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://shop.example.com"}}, "email=" + email);
    host->drain_mail();
    std::string code;
    for (const auto& f : base::list_files(root / "storage/mail/outbox")) {
      std::smatch m;
      auto text = *base::read_file(root / "storage/mail/outbox" / f);
      if (text.find("To: " + email) != std::string::npos && std::regex_search(text, m, std::regex("is ([0-9]{6})"))) code = m[1];
    }
    auto v = fetch(port, bhttp::verb::post, "/identity/email/verify",
                   {{"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://shop.example.com"}},
                   "email=" + email + "&code=" + code);
    return v.headers["set-cookie"].substr(0, v.headers["set-cookie"].find(';'));
  }
  int post(const std::string& path, const std::string& cookie, bhttp::verb verb = bhttp::verb::post) {
    return fetch(port, verb, path, {{"cookie", cookie}, {"origin", "http://shop.example.com"}}).status;
  }
  std::vector<std::string> roles_of(const std::string& email) {
    auto realm = tool("realm_get", {{"tenant", "shop"}});
    std::vector<std::string> out;
    for (const auto& u : realm.at("users").as_array()) {
      if (u.at("email").as_string() != email) continue;
      for (const auto& r : u.at("roles").as_array()) out.emplace_back(r.as_string());
    }
    return out;
  }
  std::string id_of(const std::string& email) {
    auto realm = tool("realm_get", {{"tenant", "shop"}});
    for (const auto& u : realm.at("users").as_array()) {
      if (u.at("email").as_string() == email) return std::string(u.at("user_id").as_string());
    }
    return "";
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(RolesTest, ABoxGrantsWhatItsRouteOffersAndPagesGateOnIt) {
  auto up = tool("version_upload", {{"manifest", kManifest}, {"wat", kWat}});
  ASSERT_FALSE(up.as_object().contains("error")) << json::dump(up);
  std::string widens = json::dump(up.at("widens"));
  EXPECT_NE(widens.find("may now grant consented (the caller's own)"), std::string::npos) << widens;
  EXPECT_NE(widens.find("may now grant member to the user {to} names"), std::string::npos) << widens;
  EXPECT_NE(json::dump(up.at("warnings")).find("allow-self-revoke"), std::string::npos) << json::dump(up);

  ASSERT_FALSE(tool("page_put", {{"tenant", "shop"}, {"path", "/chef"}, {"format", "markdown"}, {"body", "For cooks."},
                                 {"fields", bj::object{{"requirements", bj::array{bj::array{"consented"}}}}}})
                   .as_object()
                   .contains("error"));
  std::string ann = sign_in("ann@example.com");
  ASSERT_FALSE(ann.empty());
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/chef", {{"cookie", ann}}).status, 403);

  // An option the route doesn't offer is refused, and nothing changes.
  EXPECT_EQ(post("/sneak", ann), 409);
  // A grant in a call that fails isn't applied.
  EXPECT_EQ(post("/failing", ann), 500);
  EXPECT_TRUE(roles_of("ann@example.com").empty());

  // The person's own consent: the role lands with the commit, and the page opens.
  EXPECT_EQ(post("/consent", ann), 204);
  EXPECT_EQ(roles_of("ann@example.com"), std::vector<std::string>{"consented"});
  auto page = fetch(port, bhttp::verb::get, "/chef", {{"cookie", ann}});
  EXPECT_EQ(page.status, 200) << page.body;
  EXPECT_NE(page.body.find("For cooks."), std::string::npos);

  // Audited: the host's audit log and the realm's own, each naming the box.
  auto audit = json::dump(tool("log_search", {{"log", "audit"}, {"pattern", "role.grant"}}));
  EXPECT_NE(audit.find("box:gate"), std::string::npos) << audit;
  auto realm = json::dump(tool("realm_get", {{"tenant", "shop"}}));
  EXPECT_NE(realm.find("box:gate/consent"), std::string::npos) << realm;

  // Withdrawing is the person's own revoke, which the route opted into.
  EXPECT_EQ(post("/consent", ann, bhttp::verb::delete_), 204);
  EXPECT_TRUE(roles_of("ann@example.com").empty());
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/chef", {{"cookie", ann}}).status, 403);

  // A route that grants to a user it names: only an owner may call it, and only that role.
  std::string bob = sign_in("bob@example.com");
  std::string ann_id = id_of("ann@example.com");
  ASSERT_FALSE(ann_id.empty());
  EXPECT_EQ(post("/members/" + ann_id + "/promote", bob), 403);
  ASSERT_FALSE(tool("role_grant", {{"tenant", "shop"}, {"email", "bob@example.com"}, {"role", "owner"}})
                   .as_object()
                   .contains("error"));
  EXPECT_EQ(post("/members/" + ann_id + "/promote", bob), 204);
  EXPECT_EQ(roles_of("ann@example.com"), std::vector<std::string>{"member"});
  EXPECT_EQ(post("/members/99999999999/promote", bob), 409);  // no such user in the realm
}

TEST(RoleChanges, TheManifestSaysWhoMayGrantWhat) {
  auto refuse = [](const std::string& route, const char* why) {
    std::string err;
    auto m = manifest::parse_manifest(R"({"box":"x","domains":["*"],"routes":{"http":)" + route + "}}", &err);
    EXPECT_FALSE(m) << route;
    EXPECT_NE(err.find(why), std::string::npos) << err;
  };
  refuse(R"({"/a":{"post":{"name":"a","public":true,"grantable":{"options":["x"],"to":"{!user_id}"}}}})", "public route");
  refuse(R"({"/a":{"get":{"name":"a","requirements":[],"grantable":{"options":["x"],"to":"{!user_id}"}}}})", "a GET changes nothing");
  refuse(R"({"/a":{"post":{"name":"a","requirements":[],"grantable":{"options":["host:owner"],"to":"{!user_id}"}}}})", "host role");
  refuse(R"({"/a":{"post":{"name":"a","requirements":[],"grantable":{"options":["x"],"to":"{who}"}}}})", "not a parameter");
  refuse(R"({"/a":{"post":{"name":"a","requirements":[],"grantable":{"options":["x"],"to":"{!tenant}"}}}})", "only {!user_id}");
  refuse(R"({"/a":{"post":{"name":"a","requirements":[],"grantable":{"options":["x"],"to":"{!user_id}","allow-self-revoke":true}}}})",
         "unknown field");
  // A scope the caller picks must be one the requirements check.
  refuse(R"({"/p/{p}/{to : int64}":{"post":{"name":"a","requirements":[[]],"grantable":{"options":["admin/{p}"],"to":"{to}"}}}})",
         "no requirements clause checks it");
  std::string err;
  auto bound = manifest::parse_manifest(R"({"box":"x","domains":["*"],"routes":{"http":{
      "/p/{p}/{to : int64}":{"post":{"name":"a","requirements":[["admin/{p}"],["owner"]],
                                      "grantable":{"options":["admin/{p}","member/{p}"],"to":"{to}"}}}}}})",
                                        &err);
  ASSERT_TRUE(bound) << err;
  EXPECT_EQ(bound->routes.at(0).grantable.options.size(), 2u);
  EXPECT_TRUE(bound->warnings.empty());
  // Anyone signed in granting to a user they name is allowed, and said.
  auto anyone = manifest::parse_manifest(R"({"box":"x","domains":["*"],"routes":{"http":{
      "/u/{to : int64}":{"post":{"name":"a","requirements":[],"grantable":{"options":["fan"],"to":"{to}"}}}}}})",
                                         &err);
  ASSERT_TRUE(anyone) << err;
  ASSERT_EQ(anyone->warnings.size(), 1u);
  EXPECT_NE(anyone->warnings.at(0).find("any signed-in user may grant"), std::string::npos);
}

// A call asks for at most 16 changes: the 17th is refused, so a loop in a box can't turn one
// request into a flood of realm writes and audit lines. A try asks too, but changes nothing.
TEST_F(RolesTest, ACallChangesAFewRolesAndATryNone) {
  auto up = tool("version_upload", {{"manifest", kManifest}, {"wat", kWat}});
  ASSERT_FALSE(up.as_object().contains("error")) << json::dump(up);
  std::string ann = sign_in("ann@example.com");
  ASSERT_FALSE(ann.empty());
  EXPECT_EQ(post("/flood", ann), 409);

  std::string bob = sign_in("bob@example.com");
  ASSERT_FALSE(bob.empty());
  auto tried = tool("route_try", {{"domain", "shop.example.com"}, {"box", "gate"}, {"path", "/consent"}, {"method", "post"},
                                  {"as", bj::object{{"email", "bob@example.com"}}}});
  ASSERT_FALSE(tried.as_object().contains("error")) << json::dump(tried);
  EXPECT_EQ(tried.at("status"), 204) << json::dump(tried);
  EXPECT_TRUE(roles_of("bob@example.com").empty());
}
