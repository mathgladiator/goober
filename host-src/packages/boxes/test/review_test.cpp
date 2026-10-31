// Reviewing a change (review.h): each change is found and classified, and what widens reads as
// plain sentences.

#include <gtest/gtest.h>

#include "goober/boxes/review.h"

using namespace goober;
namespace bj = boost::json;

namespace {

boxes::VersionFiles version(const std::string& id, const std::string& manifest, const std::string& wasm = "\0asm") {
  return boxes::VersionFiles{id, manifest, wasm, ""};
}

const char* kBefore = R"({"box":"members","domains":["example.com"],
  "data":{"sqlite3":{"s":{"schema":"members1","database":"main","pattern":"sql",
    "tables":{"members":{"read":["id","name"],"insert":true,"update":["name"]}}}}},
  "external":{"mail":{"mail":{"from-name":"Example Co"},"methods":{"send":{"budget":{"calls":1}}}}},
  "flags":{"frozen":{"default":"false","values":["true","false"]}},
  "routes":{"http":{
    "/members":{"get":{"name":"list","requirements":[["admin"]]}},
    "/members/{id : string}":{"get":{"name":"one","requirements":[["admin"]]}},
    "/old":{"get":{"name":"old","public":true}}},
    "timer":{"t":{"name":"tidy","every-ms":60000}}}})";

const char* kAfter = R"({"box":"members","domains":["example.com","example.org"],
  "data":{"sqlite3":{"s":{"schema":"members1","database":"main","pattern":"sql",
    "tables":{"members":{"read":true,"insert":true,"delete":true}}}}},
  "external":{"mail":{"mail":{"from-name":"Example Co"},"methods":{"send":{"budget":{"calls":3}}}},
              "agent_mail":{"mail":{"from-name":"Agent","from":"agent@example.com"},"methods":{"send":{"budget":{"calls":1}}}}},
  "flags":{"frozen":{"default":"false","values":["true","false","soft"]}},
  "routes":{"http":{
    "/members":{"get":{"name":"list","public":true}},
    "/members/{id : string}":{"get":{"name":"one","requirements":[["admin"],["helper"]],
      "cache":{"browser-ms":60000}}}},
    "timer":{"t":{"name":"tidy","every-ms":120000}},
    "mail":{"agent@":{"receive":{"name":"agent_mail_in","public":true}}}}})";

bool has(const bj::array& a, const std::string& text) {
  for (const auto& v : a) {
    if (v.is_string() && v.as_string() == text) return true;
  }
  return false;
}

}  // namespace

TEST(Review, FindsAndClassifiesEveryKindOfChange) {
  std::string err;
  auto a = version("v1", kBefore), b = version("v2", kAfter, std::string("\0asm\1", 5));
  auto r = boxes::review(&a, b, &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(r->at("from"), "v1");
  EXPECT_EQ(r->at("new_box"), false);
  const auto& widens = r->at("widens").as_array();
  std::string all = json::dump(widens);
  // Who may call it.
  EXPECT_TRUE(has(widens, "route GET /members is now public")) << all;
  EXPECT_TRUE(has(widens, "route GET /members/{id : string} now admits admin or helper (was admin)")) << all;
  // Data.
  EXPECT_TRUE(has(widens, "strategy s may now read every column of members")) << all;
  EXPECT_TRUE(has(widens, "strategy s may now delete from members")) << all;
  // Mail, services, budgets.
  EXPECT_TRUE(has(widens, "may now reach mail from agent@example.com (service agent_mail)")) << all;
  EXPECT_TRUE(has(widens, "mail.send may now be called 3 times a request (was 1)")) << all;
  EXPECT_TRUE(has(widens, "new mail route agent@ (public): may now receive mail at agent@")) << all;
  // Browsers, domains, flags.
  EXPECT_TRUE(has(widens, "browsers and proxies may now keep route GET /members/{id : string}'s answers for 60 s")) << all;
  EXPECT_TRUE(has(widens, "now serves example.org")) << all;
  EXPECT_TRUE(has(widens, "flag frozen may now be false, soft, true")) << all;
  // What narrows or is neutral isn't in widens, but is in the changes.
  std::string changes = json::dump(r->at("changes"));
  EXPECT_NE(changes.find("strategy s may no longer update members.name"), std::string::npos) << changes;
  EXPECT_NE(changes.find("route GET /old is gone"), std::string::npos) << changes;
  EXPECT_NE(changes.find("timer tidy now runs every 120000 ms"), std::string::npos) << changes;
  EXPECT_NE(changes.find("the module changed"), std::string::npos) << changes;
  EXPECT_FALSE(has(widens, "route GET /old is gone"));
  EXPECT_GE(r->at("counts").at("narrows").as_int64(), 2);
  for (const auto& c : r->at("changes").as_array()) {
    const auto& cls = c.at("class").as_string();
    EXPECT_TRUE(cls == "widens" || cls == "narrows" || cls == "neutral") << json::dump(c);
  }
}

TEST(Review, ANewBoxWidensByAllItReaches) {
  std::string err;
  auto b = version("", kBefore);
  auto r = boxes::review(nullptr, b, &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(r->at("new_box"), true);
  EXPECT_EQ(r->at("from"), nullptr);
  EXPECT_EQ(r->at("to"), "(not stored)");
  const auto& widens = r->at("widens").as_array();
  std::string all = json::dump(widens);
  EXPECT_TRUE(has(widens, "strategy s may now insert into members")) << all;
  EXPECT_TRUE(has(widens, "strategy s may now read members.id, members.name")) << all;
  EXPECT_TRUE(has(widens, "new route GET /old (public)")) << all;
  // A timer is the host's own call: added, but no reach.
  EXPECT_FALSE(has(widens, "new timer tidy")) << all;
}

TEST(Review, TheSameVersionChangesNothing) {
  std::string err;
  auto a = version("v1", kBefore);
  auto r = boxes::review(&a, a, &err);
  ASSERT_TRUE(r) << err;
  EXPECT_TRUE(r->at("changes").as_array().empty()) << json::dump(r->at("changes"));
  EXPECT_FALSE(boxes::review(&a, version("x", "{not json"), &err));
}
