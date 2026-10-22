#include <gtest/gtest.h>

#include "goober/manifest/manifest.h"

using namespace goober::manifest;

TEST(Manifest, ParsesTheV1Subset) {
  std::string err;
  auto m = parse_manifest(R"({
    "box": "hello",
    "mission": "Say hello.",
    "domains": ["*"],
    "behavior": {"timeout-ms": 500, "lifecycle": "solo"},
    "data": {},
    "routes": {
      "http": {
        "/hello/{name : string}": {"get": {"name": "say_hello", "response": "json"}},
        "/hello/new": {"get": {"name": "new_hello"}, "post": {"name": "make_hello"}}
      },
      "timer": {}
    }
  })", &err);
  ASSERT_TRUE(m) << err;
  EXPECT_EQ(m->box, "hello");
  EXPECT_EQ(m->routes.size(), 3u);
  EXPECT_EQ(*m->behavior.timeout_ms, 500);
  EXPECT_EQ(m->warnings.size(), 4u);
  EXPECT_TRUE(m->applies_to("anything.com"));
  EXPECT_EQ(m->routes[0].export_name().substr(0, 13), "goober_route_");
}

TEST(Manifest, Refusals) {
  std::string err;
  auto refuse = [&](const char* text, const char* needle) {
    err.clear();
    EXPECT_FALSE(parse_manifest(text, &err)) << text;
    EXPECT_NE(err.find(needle), std::string::npos) << err;
  };
  refuse(R"({"domains":["*"]})", "box");
  refuse(R"({"box":"Hello","domains":["*"]})", "box name");
  refuse(R"({"box":"h"})", "domains");
  refuse(R"({"box":"h","domains":["*"],"nope":1})", "nope");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"BadName"}}}}})", "route name");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x"}},"/b":{"get":{"name":"x"}}}}})",
         "twice");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a/{x}":{"get":{"name":"x"}},"/a/{y : int64}":{"get":{"name":"y"}}}}})",
         "conflicts");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/goober-operations/x":{"get":{"name":"x"}}}}})",
         "reserved");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"fetch":{"name":"x"}}}}})", "verb");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","wat":1}}}}})", "wat");
  refuse(R"({"config":"c","domains":["*"]})", "config");
}

TEST(Manifest, SpecificDomains) {
  std::string err;
  auto m = parse_manifest(R"({"box":"h","domains":["Example.com"]})", &err);
  ASSERT_TRUE(m) << err;
  EXPECT_TRUE(m->applies_to("example.com"));
  EXPECT_FALSE(m->applies_to("other.com"));
}
