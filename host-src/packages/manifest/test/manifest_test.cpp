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
        "/hello/{name : string}": {"get": {"name": "say_hello", "public": true, "response": "json"}},
        "/hello/new": {"get": {"name": "new_hello", "public": true}, "post": {"name": "make_hello", "public": true}}
      },
      "timer": {}
    }
  })", &err);
  ASSERT_TRUE(m) << err;
  EXPECT_EQ(m->box, "hello");
  EXPECT_EQ(m->routes.size(), 3u);
  EXPECT_EQ(*m->behavior.timeout_ms, 500);
  EXPECT_EQ(m->warnings.size(), 1u);  // data, response, and timers are read now; lifecycle warns
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
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"BadName","public":true}}}}})", "route name");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","public":true}},"/b":{"get":{"name":"x","public":true}}}}})",
         "twice");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a/{x}":{"get":{"name":"x","public":true}},"/a/{y : int64}":{"get":{"name":"y","public":true}}}}})",
         "conflicts");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/goober-operations/x":{"get":{"name":"x","public":true}}}}})",
         "reserved");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"fetch":{"name":"x","public":true}}}}})", "verb");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","public":true,"wat":1}}}}})", "wat");
  refuse(R"({"config":"c","domains":["*"]})", "config");
}

TEST(Manifest, SpecificDomains) {
  std::string err;
  auto m = parse_manifest(R"({"box":"h","domains":["Example.com"]})", &err);
  ASSERT_TRUE(m) << err;
  EXPECT_TRUE(m->applies_to("example.com"));
  EXPECT_FALSE(m->applies_to("other.com"));
}

TEST(Manifest, RequirementsAndPublic) {
  std::string err;
  auto m = parse_manifest(R"({"box":"h","domains":["*"],"routes":{"http":{
    "/open": {"get": {"name": "open", "public": true}},
    "/any": {"get": {"name": "any", "requirements": []}},
    "/flat": {"get": {"name": "flat", "requirements": ["admin", "member"]}},
    "/projects/{project_id : int64}": {"get": {"name": "proj",
        "requirements": [["owner"], ["admin/project_{project_id}"], ["host:owner"]]}},
    "/mine": {"get": {"name": "mine", "requirements": [["self/user_{!user_id}"], ["owner/{!tenant}"]]}}
  }}})", &err);
  ASSERT_TRUE(m) << err;
  auto route = [&](const char* name) -> const HttpRoute& {
    for (const auto& r : m->routes) if (r.name == name) return r;
    throw std::runtime_error(name);
  };
  EXPECT_TRUE(route("open").is_public);
  EXPECT_EQ(route("any").requirements.size(), 1u);
  EXPECT_TRUE(route("any").requirements[0].empty());
  EXPECT_EQ(route("flat").requirements.size(), 1u);
  EXPECT_EQ(route("flat").requirements[0].size(), 2u);
  EXPECT_EQ(route("proj").requirements.size(), 3u);

  Caller alice{"7", false, {"admin/project_42", "member"}};
  Caller owner{"8", false, {"owner"}};
  Caller op{"1", true, {"host:owner"}};
  Caller nobody{"9", false, {}};
  std::map<std::string, std::string> p42{{"project_id", "42"}}, p43{{"project_id", "43"}};
  EXPECT_EQ(check_access(route("open"), nullptr, {}, "t"), Access::kAllowed);
  EXPECT_EQ(check_access(route("any"), nullptr, {}, "t"), Access::kSignIn);
  EXPECT_EQ(check_access(route("any"), &nobody, {}, "t"), Access::kAllowed);
  EXPECT_EQ(check_access(route("any"), &op, {}, "t"), Access::kForbidden);
  EXPECT_EQ(check_access(route("flat"), &alice, {}, "t"), Access::kForbidden);
  EXPECT_EQ(check_access(route("proj"), &alice, p42, "t"), Access::kAllowed);
  EXPECT_EQ(check_access(route("proj"), &alice, p43, "t"), Access::kForbidden);
  EXPECT_EQ(check_access(route("proj"), &owner, p43, "t"), Access::kAllowed);
  EXPECT_EQ(check_access(route("proj"), &op, p43, "t"), Access::kAllowed);
  // A host identity holding a tenant-looking role still can't use a tenant clause.
  Caller sneaky{"1", true, {"owner"}};
  EXPECT_EQ(check_access(route("proj"), &sneaky, p43, "t"), Access::kForbidden);
  Caller self{"9", false, {"self/user_9", "owner/acme"}};
  EXPECT_EQ(check_access(route("mine"), &self, {}, "other"), Access::kAllowed);
  EXPECT_EQ(check_access(route("mine"), &alice, {}, "acme"), Access::kForbidden);
  Caller tenant_owner{"5", false, {"owner/acme"}};
  EXPECT_EQ(check_access(route("mine"), &tenant_owner, {}, "acme"), Access::kAllowed);

  auto refuse = [&](const char* text, const char* needle) {
    err.clear();
    EXPECT_FALSE(parse_manifest(text, &err)) << text;
    EXPECT_NE(err.find(needle), std::string::npos) << err;
  };
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x"}}}}})", "denied to everyone");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","public":false}}}}})", "denied");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","public":true,"requirements":[]}}}}})",
         "both");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","requirements":["a/{nope}"]}}}}})",
         "not a parameter");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","requirements":["{!email}"]}}}}})",
         "reserved");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","requirements":["a/b/c"]}}}}})",
         "role or role/scope");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","requirements":["host:a/b"]}}}}})",
         "role or role/scope");
  refuse(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"x","requirements":["a-b"]}}}}})",
         "character");
}

TEST(Manifest, SubstitutedValuesCantSmuggleScopes) {
  std::string err;
  auto m = parse_manifest(R"({"box":"h","domains":["*"],"routes":{"http":{
    "/p/{project_id : string}": {"get": {"name": "p", "requirements": ["admin/project_{project_id}"]}}}}})", &err);
  ASSERT_TRUE(m) << err;
  Caller c{"1", false, {"admin/project_x"}};
  EXPECT_EQ(check_access(m->routes[0], &c, {{"project_id", "x"}}, "t"), Access::kAllowed);
  EXPECT_EQ(check_access(m->routes[0], &c, {{"project_id", "x:y"}}, "t"), Access::kBadValue);
}

TEST(Manifest, TemplatesAreCheckedAndRoutesNameThem) {
  std::string err;
  auto m = parse_manifest(R"({"box":"h","domains":["*"],
    "templates": {"job": "<h1>Job {{id}}</h1>"},
    "routes":{"http":{"/jobs/{id : int64}":{"get":{"name":"job","template":"job","public":true}}}}})", &err);
  ASSERT_TRUE(m) << err;
  EXPECT_EQ(m->templates.at("job"), "<h1>Job {{id}}</h1>");
  EXPECT_EQ(m->routes[0].template_name, "job");
  EXPECT_FALSE(parse_manifest(R"({"box":"h","domains":["*"],"templates":{"bad":"{{#open}}"}})", &err));
  EXPECT_NE(err.find("templates.bad"), std::string::npos);
  EXPECT_FALSE(parse_manifest(R"({"box":"h","domains":["*"],"routes":{"http":{"/attachment/x":{"get":{"name":"x","public":true}}}}})", &err));
  EXPECT_NE(err.find("reserved"), std::string::npos);
}

TEST(Manifest, AgentsOptIn) {
  std::string err;
  auto m = parse_manifest(R"({"box":"h","domains":["*"],"routes":{"http":{
    "/a":{"get":{"name":"a","requirements":[],"agents":true}},"/b":{"get":{"name":"b","requirements":[]}}}}})", &err);
  ASSERT_TRUE(m) << err;
  EXPECT_TRUE(m->routes[0].agents);
  EXPECT_FALSE(m->routes[1].agents);
  EXPECT_FALSE(parse_manifest(R"({"box":"h","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"a","requirements":[],"agents":"yes"}}}}})", &err));
}

TEST(Manifest, DataStrategies) {
  std::string err;
  auto m = parse_manifest(R"({"box":"g","domains":["*"],"data":{"sqlite3":{
      "book": {"schema": "guestbook1", "database": "guest_{!tenant}", "pattern": "sql",
               "tables": {"entries": {"read": true, "insert": true, "update": ["approved"], "delete": false}},
               "row-limit": 50},
      "later": {"schema": "s1", "database": "d", "pattern": "by_id", "table": "users", "id-is": "{!user_id}"}}}})", &err);
  ASSERT_TRUE(m) << err;
  ASSERT_EQ(m->data.size(), 2u);
  const auto& book = m->data[0];
  EXPECT_TRUE(book.runnable());
  EXPECT_EQ(book.database_for("playground"), "guest_playground");
  EXPECT_TRUE(book.tables.at("entries").read_all);
  EXPECT_EQ(book.tables.at("entries").update.count("approved"), 1u);
  EXPECT_EQ(book.row_limit, 50);
  EXPECT_FALSE(m->data[1].runnable());
  EXPECT_FALSE(m->warnings.empty());

  auto refuse = [&](const char* text, const char* why) {
    std::string e;
    EXPECT_FALSE(parse_manifest(text, &e)) << text;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refuse(R"({"box":"g","domains":["*"],"data":{"sqlite3":{"b":{"schema":"s","database":"d","pattern":"sql"}}}})",
         "needs tables");
  refuse(R"({"box":"g","domains":["*"],"data":{"sqlite3":{"b":{"schema":"s","database":"../x","pattern":"sql","tables":{"t":{}}}}}})",
         "database must be");
  refuse(R"({"box":"g","domains":["*"],"data":{"sqlite3":{"b":{"schema":"s","database":"d","pattern":"sql","tables":{"t":{"drop":true}}}}}})",
         "unknown operation");
  refuse(R"({"box":"g","domains":["*"],"data":{"sqlite3":{"b":{"schema":"s","database":"d","pattern":"sql","sql":"x","tables":{"t":{}}}}}})",
         "doesn't apply");
}

TEST(Manifest, RequestPartsTypesAndTools) {
  std::string err;
  auto m = parse_manifest(R"({"box":"o","domains":["*"],"types":"orders.openapi.json","routes":{
    "http":{"/orders":{"post":{"name":"add","requirements":[],"body":"json:Order","max-body-bytes":4096,
                               "response":"json:Order","request-headers":["Idempotency-Key"]},
                       "get":{"name":"list","public":true,"query":{"limit":{"type":"integer","format":"int32","default":10}}}}},
    "mcp":{"add_order":{"tool":{"description":"Adds an order.","input":"json:Order","requirements":[["owner"]]}}}}})",
                          &err);
  ASSERT_TRUE(m) << err;
  EXPECT_EQ(m->types, "orders.openapi.json");
  EXPECT_EQ(m->routes[0].body, "json:Order");
  EXPECT_EQ(*m->routes[0].max_body_bytes, 4096);
  EXPECT_EQ(m->routes[0].request_headers.at(0), "idempotency-key");
  EXPECT_TRUE(m->routes[1].query_declared);
  ASSERT_EQ(m->tools.size(), 1u);
  EXPECT_EQ(m->tools[0].verb, "tool");
  EXPECT_EQ(m->tools[0].export_name(), "goober_route_add_order");

  auto types = goober::types::Types::parse(R"({"openapi":"3.1.0","components":{"schemas":{
    "Order":{"type":"object","additionalProperties":false,"properties":{"n":{"type":"integer","format":"int32"}}}}}})", &err);
  ASSERT_TRUE(types) << err;
  EXPECT_TRUE(check_types(*m, &*types, &err)) << err;
  EXPECT_FALSE(check_types(*m, nullptr, &err));
  EXPECT_NE(err.find("wasn't supplied"), std::string::npos) << err;

  auto refuse = [&](const char* text, const char* why) {
    std::string e;
    EXPECT_FALSE(parse_manifest(text, &e)) << text;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refuse(R"({"box":"o","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"a","public":true,"body":"json:X"}}}}})", "carries no body");
  refuse(R"({"box":"o","domains":["*"],"routes":{"http":{"/a":{"post":{"name":"a","public":true,"body":"xml"}}}}})", "body must be");
  refuse(R"({"box":"o","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"a","public":true,"request-headers":["Cookie"]}}}}})", "host keeps it");
  refuse(R"({"box":"o","domains":["*"],"routes":{"http":{"/a":{"get":{"name":"a","public":true,"query":{"q":{"type":"object","additionalProperties":false}}}}}}})", "scalars");
  refuse(R"({"box":"o","domains":["*"],"routes":{"mcp":{"t":{"tool":{"description":"d","public":true}}}}})", "can't be public");
  refuse(R"({"box":"o","domains":["*"],"routes":{"mcp":{"t":{"tool":{"requirements":[]}}}}})", "description is required");
  refuse(R"({"box":"o","domains":["*"],"routes":{"mcp":{"t":{"tool":{"description":"d"}}}}})", "needs requirements");
  refuse(R"({"box":"o","domains":["*"],"routes":{"mcp":{"t":{"tool":{"name":"u","description":"d","requirements":[]}}}}})", "tool's key");
  refuse(R"({"box":"o","domains":["*"],"types":"../x.json"})", "types document");
}

TEST(Manifest, TimersAndTimerSources) {
  std::string err;
  auto m = parse_manifest(R"({"box":"t","domains":["*"],
    "data":{"sqlite3":{"people":{"schema":"s1","database":"d","pattern":"by_timer","table":"devices",
                                 "timezone-field":"timezone","active-field":"on"}}},
    "routes":{"timer":{
      "sweep":{"name":"sweep","every-ms":60000},
      "nightly":{"name":"nightly","cron":"0 3 * * *"},
      "morning":{"name":"morning","source":"people","cron":"0 6 * * *"},
      "remind":{"name":"remind","source":"people","at-field":"remind_at","timeout-ms":500}}}})", &err);
  ASSERT_TRUE(m) << err;
  ASSERT_EQ(m->timers.size(), 4u);
  EXPECT_EQ(m->timers[0].verb, "timer");
  EXPECT_EQ(m->timers[0].missed, "skip");  // periodic timers skip what they missed
  EXPECT_EQ(m->timers[1].missed, "once");  // cron timers catch up once
  EXPECT_EQ(m->data[0].table, "devices");
  EXPECT_EQ(m->data[0].timezone_field, "timezone");
  EXPECT_FALSE(m->data[0].runnable());  // the box can't run it as code
  EXPECT_TRUE(m->data[0].applies_schema());

  auto refuse = [&](const char* text, const char* why) {
    std::string e;
    EXPECT_FALSE(parse_manifest(text, &e)) << text;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refuse(R"({"box":"t","domains":["*"],"routes":{"timer":{"x":{"name":"x"}}}})", "exactly one");
  refuse(R"({"box":"t","domains":["*"],"routes":{"timer":{"x":{"name":"x","every-ms":5,"cron":"* * * * *"}}}})", "exactly one");
  refuse(R"({"box":"t","domains":["*"],"routes":{"timer":{"x":{"name":"x","every-ms":10}}}})", "every-ms");
  refuse(R"({"box":"t","domains":["*"],"routes":{"timer":{"x":{"name":"x","cron":"0 25 * * *"}}}})", "cron");
  refuse(R"({"box":"t","domains":["*"],"routes":{"timer":{"x":{"name":"x","cron":"0 1 * * *","public":true}}}})", "only by the host");
  refuse(R"({"box":"t","domains":["*"],"routes":{"timer":{"x":{"name":"x","at-field":"when"}}}})", "needs a source");
  refuse(R"({"box":"t","domains":["*"],"routes":{"timer":{"x":{"name":"x","source":"nope","cron":"0 1 * * *"}}}})", "by_timer");
  refuse(R"({"box":"t","domains":["*"],"data":{"sqlite3":{"p":{"schema":"s","database":"d","pattern":"by_timer","table":"t"}}},
            "routes":{"timer":{"x":{"name":"x","source":"p","at-field":"when"}}}})", "active-field");
}
