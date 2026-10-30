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

TEST(Manifest, ExternalServices) {
  std::string err;
  auto m = parse_manifest(R"({"box":"n","domains":["*"],"external":{"mailer":{
      "base":"https://mail.example.com/v3/","credentials":"keys/tenant/{!tenant}/ses",
      "auth":{"header":"X-Api-Key","prefix":""},
      "methods":{"send":{"verb":"POST","path":"/messages","idempotency":"header/Idempotency-Key",
                         "require":{"body.to":"{!email}"},"budget":{"calls":3}},
                 "status":{"verb":"get","path":"/messages/{message_id}"}}}}})", &err);
  ASSERT_TRUE(m) << err;
  const auto& svc = m->external.at(0);
  EXPECT_EQ(svc.base, "https://mail.example.com/v3");
  EXPECT_EQ(svc.auth_header, "x-api-key");
  EXPECT_EQ(svc.methods.at("send").effect, "outbox");  // effectful verbs wait for the commit
  EXPECT_EQ(svc.methods.at("status").effect, "none");  // GET is safe
  EXPECT_EQ(svc.methods.at("send").idempotency_header, "idempotency-key");
  EXPECT_TRUE(svc.has_outbox());

  auto refuse = [&](const std::string& ext, const char* why) {
    std::string e;
    std::string text = R"({"box":"n","domains":["*"],"external":)" + ext + "}";
    EXPECT_FALSE(parse_manifest(text, &e)) << text;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refuse(R"({"s":{"base":"http://example.com","methods":{"m":{"verb":"get"}}}})", "https");
  refuse(R"({"s":{"base":"https://example.com?x=1","methods":{"m":{"verb":"get"}}}})", "https");
  refuse(R"({"s":{"base":"https://example.com"}})", "needs methods");
  refuse(R"({"s":{"base":"https://example.com","credentials":"sk_live_123","methods":{"m":{"verb":"get"}}}})", "names a key");
  refuse(R"({"s":{"base":"https://example.com","methods":{"m":{"verb":"fetch"}}}})", "verb");
  refuse(R"({"s":{"base":"https://example.com","methods":{"m":{"verb":"get","path":"/a/../b"}}}})", "path");
  refuse(R"({"s":{"base":"https://example.com","methods":{"m":{"verb":"get","require":{"to":"x"}}}}})", "body.to");
  refuse(R"({"s":{"base":"https://example.com","methods":{"m":{"verb":"get","require":{"body.to":"{!password}"}}}}})", "{!email}");
  refuse(R"({"s":{"base":"https://example.com","methods":{"m":{"verb":"post","effect":"later"}}}})", "effect");
  EXPECT_TRUE(parse_manifest(R"({"box":"n","domains":["*"],"external":{"dev":{"base":"http://127.0.0.1:9000","methods":{"m":{"verb":"get"}}}}})", &err)) << err;
  // The host's own keys are never a box's credentials.
  refuse(R"({"s":{"base":"https://evil.example","credentials":"keys/system/aws_secret_access_key","methods":{"m":{"verb":"get"}}}})",
         "host's own key");
  refuse(R"({"s":{"base":"https://evil.example","credentials":"keys/system/backup_key","methods":{"m":{"verb":"get"}}}})",
         "host's own key");
}

TEST(Manifest, MailService) {
  std::string err;
  auto m = parse_manifest(R"({"box":"n","domains":["*"],"external":{"mail":{"mail":{"from-name":"Beef Share"},
      "methods":{"send":{"budget":{"calls":2}}}}}})", &err);
  ASSERT_TRUE(m) << err;
  const auto& svc = m->external.at(0);
  EXPECT_TRUE(svc.mail);
  EXPECT_EQ(svc.mail_from_name, "Beef Share");
  EXPECT_EQ(svc.methods.at("send").effect, "outbox");  // mail leaves only after the request commits
  EXPECT_EQ(svc.methods.at("send").budget_calls, 2);
  EXPECT_TRUE(svc.has_outbox());
  ASSERT_TRUE(parse_manifest(R"({"box":"n","domains":["*"],"external":{"m":{"mail":{}}}})", &err)) << err;

  auto refuse = [&](const std::string& ext, const char* why) {
    std::string e;
    std::string text = R"({"box":"n","domains":["*"],"external":)" + ext + "}";
    EXPECT_FALSE(parse_manifest(text, &e)) << text;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refuse(R"({"m":{"mail":{},"base":"https://x.example"}})", "no base");
  refuse(R"({"m":{"mail":{"reply":"x@y.z"}}})", "unknown field");
  refuse(R"({"m":{"mail":{"from":"Evil <x@y.z>"}}})", "mail.from is one address");
  refuse(R"({"m":{"mail":{"from":"nobody"}}})", "mail.from is one address");
  std::string ok_err;
  auto with_from = parse_manifest(R"({"box":"n","domains":["*"],"external":{"m":{"mail":{"from":"Hello@KCmeatup.com"}}}})", &ok_err);
  ASSERT_TRUE(with_from) << ok_err;
  EXPECT_EQ(with_from->external.at(0).mail_from, "hello@kcmeatup.com");
  refuse(R"({"m":{"mail":{"from-name":"Evil\" <x@y.z>"}}})", "from-name");
  refuse(R"({"m":{"mail":{},"methods":{"blast":{}}}})", "methods are send, and attachment");
  refuse(R"({"m":{"mail":{},"methods":{"send":{"budget":{"calls":50}}}}})", "1 to 10");
}

TEST(Manifest, IdentityRoutes) {
  std::string err;
  auto m = parse_manifest(R"({"box":"t","domains":["*"],
    "routes":{"identity":{
      "first-verified":{"name":"welcome","timeout-ms":2000},
      "signed-in":{"name":"on_sign_in"}}}})", &err);
  ASSERT_TRUE(m) << err;
  ASSERT_EQ(m->identity.size(), 2u);
  EXPECT_EQ(m->identity[0].verb, "identity");
  EXPECT_EQ(m->identity[0].identity_event, "first-verified");
  EXPECT_EQ(m->identity[0].name, "welcome");
  EXPECT_EQ(*m->identity[0].timeout_ms, 2000);
  EXPECT_EQ(m->identity[1].identity_event, "signed-in");
  EXPECT_TRUE(m->routes.empty());

  auto refuse = [&](const char* text, const char* why) {
    std::string e;
    EXPECT_FALSE(parse_manifest(text, &e)) << text;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refuse(R"({"box":"t","domains":["*"],"routes":{"identity":{"signed-out":{"name":"x"}}}})", "first-verified, signed-in");
  refuse(R"({"box":"t","domains":["*"],"routes":{"identity":{"signed-in":{"name":"x","public":true}}}})", "only by the host");
  refuse(R"({"box":"t","domains":["*"],"routes":{"identity":{"signed-in":{}}}})", "name is required");
  refuse(R"({"box":"t","domains":["*"],"routes":{"identity":{"signed-in":{"name":"x","every-ms":5}}}})", "unknown field");
  refuse(R"({"box":"t","domains":["*"],"routes":{"identity":{"signed-in":{"name":"x","timeout-ms":0}}}})", "1 to 10000");
}

TEST(Manifest, MailRoutes) {
  std::string err;
  auto m = parse_manifest(R"({"box":"desk","domains":["kcmeatup.com","beef.example"],
    "routes":{"mail":{
      "support@kcmeatup.com":{"receive":{"name":"support_inbox","public":true,"max-bytes":10485760,"attachments":5}},
      "reply+{ticket : token}@":{"receive":{"name":"ticket_reply","public":true,"attachments":0}},
      "*@":{"receive":{"name":"catch_all","public":true,"automatic":true,"timeout-ms":3000}}}}})", &err);
  ASSERT_TRUE(m) << err;
  ASSERT_EQ(m->mail.size(), 3u);
  const auto* support = &m->mail[0];
  for (const auto& r : m->mail) {
    if (r.name == "support_inbox") support = &r;
  }
  EXPECT_EQ(support->verb, "receive");
  EXPECT_EQ(support->mail_address, "support@kcmeatup.com");
  EXPECT_EQ(*support->max_body_bytes, 10485760);
  EXPECT_EQ(support->mail_attachments, 5);
  EXPECT_FALSE(support->mail_automatic);
  EXPECT_TRUE(support->is_public);
  for (const auto& r : m->mail) {
    if (r.name == "catch_all") EXPECT_TRUE(r.mail_automatic);
    if (r.name == "ticket_reply") EXPECT_EQ(*r.max_body_bytes, 10LL << 20);  // the default
  }

  auto refuse = [&](const std::string& routes, const char* why) {
    std::string e;
    std::string text = R"({"box":"t","domains":["kcmeatup.com"],"routes":{"mail":)" + routes + "}}";
    EXPECT_FALSE(parse_manifest(text, &e)) << routes;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refuse(R"({"a@":{"receive":{"name":"a"}}})", "public");
  refuse(R"({"a@":{"receive":{"name":"a","public":true,"requirements":[]}}})", "public");
  refuse(R"({"a@":{"send":{"name":"a","public":true}}})", "one verb is receive");
  refuse(R"({"a@other.com":{"receive":{"name":"a","public":true}}})", "isn't one of the box's domains");
  refuse(R"({"A@":{"receive":{"name":"a","public":true}}})", "can't be in a local part");
  refuse(R"({"a+{x}@":{"receive":{"name":"a","public":true}},"a+{y}@":{"receive":{"name":"b","public":true}}})", "same addresses");
  refuse(R"({"a@":{"receive":{"name":"a","public":true,"max-bytes":99999999}}})", "max-bytes");
  refuse(R"({"a@":{"receive":{"name":"a","public":true,"attachments":101}}})", "attachments");
  refuse(R"({"a@":{"receive":{"name":"a","public":true,"color":"red"}}})", "unknown field");
}

TEST(Manifest, PushServices) {
  std::string err;
  auto m = parse_manifest(R"({"box":"shop","domains":["*"],"external":{"alerts":{"push":{},
      "methods":{"send":{"budget":{"calls":20}}}}}})", &err);
  ASSERT_TRUE(m) << err;
  ASSERT_EQ(m->external.size(), 1u);
  EXPECT_TRUE(m->external[0].push);
  EXPECT_EQ(m->external[0].methods.at("send").effect, "outbox");
  EXPECT_EQ(m->external[0].methods.at("send").budget_calls, 20);
  for (const char* bad : {R"({"box":"b","domains":["*"],"external":{"a":{"push":{},"base":"https://x.example"}}})",
                          R"({"box":"b","domains":["*"],"external":{"a":{"push":{"vapid":"x"}}}})",
                          R"({"box":"b","domains":["*"],"external":{"a":{"push":{},"methods":{"blast":{}}}}})"}) {
    EXPECT_FALSE(parse_manifest(bad, &err)) << bad;
  }
}

TEST(Manifest, Libraries) {
  std::string err;
  auto m = parse_manifest(R"({"box":"cal","domains":["*"],"libraries":{"time":"time@1","cal":"ics@1"}})", &err);
  ASSERT_TRUE(m) << err;
  ASSERT_EQ(m->libraries.size(), 2u);
  EXPECT_EQ(m->libraries[1].alias, "cal");  // in the manifest's order
  EXPECT_EQ(m->libraries[1].library, "ics");
  EXPECT_EQ(m->libraries[1].major, 1);
  for (const char* bad : {R"({"box":"b","domains":["*"],"libraries":{"t":"time"}})",
                          R"({"box":"b","domains":["*"],"libraries":{"t":"time@x"}})",
                          R"({"box":"b","domains":["*"],"libraries":{"t-x":"time@1"}})",
                          R"({"box":"b","domains":["*"],"libraries":{"t":"Time@1"}})",
                          R"({"box":"b","domains":["*"],"libraries":["time@1"]})"}) {
    EXPECT_FALSE(parse_manifest(bad, &err)) << bad;
  }
}

TEST(Manifest, MailListsAndInvites) {
  std::string err;
  auto m = parse_manifest(R"({"box":"thesis","domains":["*"],"external":{
      "mail":{"mail":{"from-name":"KC Meat Up","lists":{"digest":{"title":"The daily digest","about":"One email a day."}}}},
      "invites":{"invites":{"roles":["member","editor/blog"],"lists":["digest"]}}}})", &err);
  ASSERT_TRUE(m) << err;
  ASSERT_EQ(m->external.size(), 2u);
  const auto& mail = m->external[0].mail ? m->external[0] : m->external[1];
  const auto& inv = m->external[0].invites ? m->external[0] : m->external[1];
  EXPECT_EQ(mail.mail_lists.at("digest").title, "The daily digest");
  EXPECT_TRUE(mail.methods.count("subscribe"));
  EXPECT_EQ(mail.methods.at("preferences").effect, "none");
  EXPECT_TRUE(inv.invites);
  EXPECT_EQ(inv.invite_roles.size(), 2u);
  EXPECT_EQ(inv.methods.at("send").effect, "outbox");
  EXPECT_EQ(inv.methods.at("status").effect, "none");
  for (const char* bad : {
           R"({"box":"b","domains":["*"],"external":{"i":{"invites":{"roles":["host:owner"]}}}})",
           R"({"box":"b","domains":["*"],"external":{"i":{"invites":{"lists":["digest"]}}}})",  // no such list
           R"({"box":"b","domains":["*"],"external":{"i":{"invites":{},"base":"https://x.example"}}})",
           R"({"box":"b","domains":["*"],"external":{"i":{"invites":{"grant":["x"]}}}})",
           R"({"box":"b","domains":["*"],"external":{"m":{"mail":{"lists":{"Digest":{"title":"x"}}}}}})",
           R"({"box":"b","domains":["*"],"external":{"m":{"mail":{"lists":{"digest":{"title":""}}}}}})",
           R"({"box":"b","domains":["*"],"external":{"m":{"mail":{"lists":{"digest":{"title":"x","color":"red"}}}}}})"}) {
    EXPECT_FALSE(parse_manifest(bad, &err)) << bad;
  }
}

TEST(Manifest, ServicesSpokenFromSpecs) {
  std::string err;
  auto m = parse_manifest(R"({"box":"app","domains":["*"],"routes":{"result":{"connected":{},"done":{}}},"external":{
    "hevy":{"base":"https://api.hevyapp.com","spec":"hevy@0123456789ab","credentials":"sealed",
      "auth":{"kind":"header","name":"api-key"},"headers":{"x-version":"2"},
      "connect":{"paste":{"label":"Hevy API key","check":"GET /v1/user","result-route":"connected"}},
      "methods":{"workouts":{"op":"GET /v1/workouts"},"save":{"op":"POST /v1/workouts","body":"json"}}},
    "x":{"base":"https://api.x.com","spec":"x@0123456789ab","credentials":"sealed","auth":{"kind":"bearer"},
      "connect":{"oauth2":{"authorize":"https://x.com/i/oauth2/authorize","token":"https://api.x.com/2/oauth2/token",
        "client-id":"keys/tenant/{!tenant}/x_id","pkce":true,"scopes":["tweet.write"],"result-route":"connected"}},
      "methods":{"media":{"op":"init","result-route":"done","upload":{"attachment":"body.media",
          "init":{"op":"init","body":{"total_bytes":"{bytes}"},"id":"body.data.id"},
          "append":{"op":"append","path":{"id":"{id}"},"parts":{"media":"{chunk}"}},"finalize":{"op":"fin","path":{"id":"{id}"}}},
        "poll":{"op":"status","query":{"media_id":"{body.data.id}"},"done-when":{"at":"body.data.state","equals":"succeeded"}}}}},
    "anywhere":{"base":"*","methods":{"mention":{"verb":"post","path":"/","body":"form","any-public":true,"effect":"none"}}}}})", &err);
  ASSERT_TRUE(m) << err;
  const goober::manifest::ExternalService* hevy = nullptr;
  for (const auto& s : m->external) {
    if (s.name == "hevy") hevy = &s;
  }
  ASSERT_TRUE(hevy);
  EXPECT_TRUE(hevy->sealed);
  EXPECT_EQ(hevy->auth_kind, "header");
  EXPECT_EQ(hevy->auth_header, "api-key");
  EXPECT_EQ(hevy->connect.kind, "paste");
  EXPECT_EQ(hevy->methods.at("workouts").verb, "get");
  EXPECT_EQ(hevy->methods.at("workouts").effect, "none");
  EXPECT_EQ(hevy->methods.at("save").effect, "outbox");
  for (const char* bad : {
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","methods":{"m":{"op":"getX"}}}}})",  // no spec
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","spec":"s@zz","methods":{"m":{"op":"x"}}}}})",
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","spec":"s@0123456789ab",
              "methods":{"m":{"op":"x","verb":"get","path":"/"}}}}})",  // op and verb both
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","credentials":"keys/system/vault_key",
              "methods":{"m":{"verb":"get","path":"/"}}}}})",
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","connect":{"paste":{"result-route":"r"}},
              "methods":{"m":{"verb":"get","path":"/"}}}}})",  // connect without sealed
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","auth":{"kind":"magic"},
              "methods":{"m":{"verb":"get","path":"/"}}}}})",
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","headers":{"authorization":"x"},
              "methods":{"m":{"verb":"get","path":"/"}}}}})",
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","methods":{"m":{"verb":"get","path":"/","any-public":true}}}}})",
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","spec":"s@0123456789ab",
              "methods":{"m":{"op":"x","poll":{"op":"y","done-when":{"at":"body.x","present":true}}}}}}})"}) {  // poll without result
    EXPECT_FALSE(parse_manifest(bad, &err)) << bad;
  }
}

TEST(Manifest, SealedSecretsAndHostKeysStayWhereTheyBelong) {
  std::string err;
  for (const char* bad : {
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"*","credentials":"sealed","methods":{"m":{"verb":"post","path":"/","any-public":true}}}}})",
           R"({"box":"b","domains":["*"],"routes":{"result":{"r":{}}},"external":{"s":{"base":"https://a.example","credentials":"sealed",
              "connect":{"oauth2":{"authorize":"https://a.example/a","token":"https://a.example/t","client-id":"keys/system/vault_key","result-route":"r"}},
              "methods":{"m":{"verb":"get","path":"/"}}}}})",
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","auth":{"kind":"oauth1","consumer-key":"keys/system/aws_access_key_id",
              "consumer-secret":"keys/system/x"},"methods":{"m":{"verb":"get","path":"/"}}}}})",
           R"({"box":"b","domains":["*"],"external":{"s":{"base":"https://a.example","auth":{"kind":"oauth1","consumer-key":"keys/system/k",
              "consumer-secret":"keys/system/x","token":"keys/system/t"},"methods":{"m":{"verb":"get","path":"/"}}}}})"}) {
    EXPECT_FALSE(parse_manifest(bad, &err)) << bad;
  }
}
