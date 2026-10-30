// Speaking HTTP (design/machine/034.SpeakingHttp.md) against a stand-in API and OAuth provider:
// pinned specs and operations, auth kinds, sealed per-person secrets from paste and OAuth 2
// connections, refresh with rotation, bodies, chunked uploads, polling, and limits.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <fstream>
#include <regex>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/server_http/server.h"
#include "../src/keys.h"
#include "../src/network.h"
#include "goober/signing/oauth1.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

struct Seen {
  std::string method, path, query, body, auth, content_type;
  std::map<std::string, std::string> headers;
};

// The stand-in: an API and an OAuth provider, answering by handler, recording every request.
class Api {
 public:
  using Handler = std::function<server::http::Response(const server::http::Request&)>;
  Api() {
    server::http::ServerOptions o;
    o.port = 0;
    o.threads = 1;
    server_ = std::make_unique<server::http::Server>(o, [this](server::http::Request req, server::http::Responder respond) {
      Seen s{req.method, req.path, req.query, req.body, req.header("authorization"), req.header("content-type"), {}};
      for (const char* h : {"api-key", "x-version", "idempotency-key", "x-box-header"}) s.headers[h] = req.header(h);
      Handler h;
      {
        std::lock_guard lock(mu_);
        seen_.push_back(s);
        for (const auto& [prefix, fn] : handlers_) {
          if (req.path.starts_with(prefix)) h = fn;
        }
      }
      server::http::Response res;
      if (h) {
        res = h(req);
      } else {
        res.status = 200;
        res.body = R"({"ok":true})";
      }
      if (res.headers.empty()) res.headers.emplace_back("content-type", "application/json");
      respond(std::move(res));
    });
    server_->start();
  }
  uint16_t port() const { return server_->port(); }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port()); }
  void on(const std::string& prefix, Handler h) {
    std::lock_guard lock(mu_);
    handlers_[prefix] = std::move(h);
  }
  std::vector<Seen> seen() {
    std::lock_guard lock(mu_);
    return seen_;
  }
  std::vector<Seen> seen(const std::string& prefix) {
    std::vector<Seen> out;
    for (const auto& s : seen()) {
      if (s.path.starts_with(prefix)) out.push_back(s);
    }
    return out;
  }

 private:
  std::mutex mu_;
  std::vector<Seen> seen_;
  std::map<std::string, Handler> handlers_;
  std::unique_ptr<server::http::Server> server_;
};

server::http::Response json_reply(int status, const std::string& body, std::vector<std::pair<std::string, std::string>> headers = {}) {
  server::http::Response r;
  r.status = status;
  r.body = body;
  r.headers.emplace_back("content-type", "application/json");
  for (auto& h : headers) r.headers.push_back(std::move(h));
  return r;
}

struct Reply {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};

Reply fetch(uint16_t port, bhttp::verb verb, const std::string& target, std::vector<std::pair<std::string, std::string>> headers = {},
            const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "kc.example.com");
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

bj::object parse(const std::string& s) {
  std::string err;
  auto v = json::parse(s, &err);
  return v && v->is_object() ? v->as_object() : bj::object{{"unparsed", s}};
}

// A result route that writes what it's given to its log, so the test can read the sealed value.
const char* kLoggingBox = R"((module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_log" (func $log (param i32 i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "text/plain")
  (data (i32.const 16) "ok")
  (func $keep (local $n i32)
    (local.set $n (call $size))
    (drop (call $read (i32.const 1024) (local.get $n)))
    (drop (call $log (i32.const 1) (i32.const 1024) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 10) (i32.const 16) (i32.const 2))))
  (func (export "goober_route_connected") (result i32) (call $keep) (i32.const 0))
  (func (export "goober_route_done") (result i32) (call $keep) (i32.const 0))
))";

}  // namespace

class HttpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "http_out";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"kc.example.com": {"tenant": "kc"}}, "threads": {"network": 1, "boxes": 2},
        "operations": {"token": "op-token-for-http-tests-0123456789"}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }

  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer op-token-for-http-tests-0123456789"}, {"content-type", "application/json"}}, json::dump(req));
    auto doc = parse(r.body);
    const auto& result = doc.at("result").as_object();
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::object{{"error", text}};
    std::string err;
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  }
  std::string pin(const std::string& name, const std::string& spec) {
    auto r = tool("spec_put", {{"name", name}, {"text", spec}});
    EXPECT_TRUE(r.as_object().contains("pin")) << json::dump(r);
    return r.as_object().contains("pin") ? std::string(r.at("pin").as_string()) : "";
  }
  void upload(const std::string& manifest) {
    std::string err;
    ASSERT_TRUE(host->boxes().upload(manifest, kLoggingBox, &err)) << err;
  }
  bj::object call(const std::string& service, const std::string& method, const std::string& request, const std::string& user = "") {
    return parse(host->call_as_box("app", "kc", service, method, request, user));
  }
  std::string sign_in(const std::string& email) {
    fetch(port, bhttp::verb::post, "/identity/email/start",
          {{"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://kc.example.com"}}, "email=" + email);
    host->drain_mail();
    std::string code;
    for (const auto& f : base::list_files(root / "storage/mail/outbox")) {
      std::smatch m;
      auto text = *base::read_file(root / "storage/mail/outbox" / f);
      if (text.find("To: " + email) != std::string::npos && std::regex_search(text, m, std::regex("is ([0-9]{6})"))) code = m[1];
    }
    auto v = fetch(port, bhttp::verb::post, "/identity/email/verify",
                   {{"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://kc.example.com"}},
                   "email=" + email + "&code=" + code);
    return v.headers["set-cookie"].substr(0, v.headers["set-cookie"].find(';'));
  }
  // The newest sealed value a result route was handed, from the box's log.
  std::string last_credential() {
    std::string found;
    std::ifstream in(root / "storage/logs/boxes/app.log");
    std::string line;
    std::smatch m;
    while (std::getline(in, line)) {
      if (std::regex_search(line, m, std::regex(R"((gsv1\.[0-9]+\.[A-Za-z0-9_-]+))"))) found = m[1];
    }
    return found;
  }
  std::string last_result_body() {
    std::string found;
    std::ifstream in(root / "storage/logs/boxes/app.log");
    std::string line;
    while (std::getline(in, line)) found = line;
    return found;
  }

  Api api;
  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

// The Hevy shape: a spec with no operationIds, the key in an api-key header, each person's key
// pasted into a host form, checked, sealed, and handed to the box.
TEST_F(HttpTest, APastedKeyIsCheckedSealedAndAppliedFromTheBoxsCall) {
  std::string spec = R"({"openapi":"3.0.0","info":{"title":"Hevy","version":"0.0.1"},
    "paths":{"/v1/workouts":{"get":{"parameters":[{"name":"page","in":"query"},{"name":"x-box-header","in":"header"}]},
                              "post":{"requestBody":{"content":{"application/json":{}}}}},
             "/v1/user":{"get":{}}}})";
  std::string p = pin("hevy", spec);
  api.on("/v1/user", [](const server::http::Request& r) {
    return r.header("api-key") == "good-key" ? json_reply(200, R"({"name":"Ann"})") : json_reply(401, R"({"error":"no"})");
  });
  upload(R"({"box":"app","domains":["kc.example.com"],"routes":{"result":{"connected":{},"done":{}}},
    "external":{"hevy":{"base":")" + api.base() + R"(","spec":")" + p + R"(","credentials":"sealed",
      "auth":{"kind":"header","name":"api-key"},"headers":{"x-version":"2"},
      "connect":{"paste":{"label":"Hevy API key","check":"GET /v1/user","result-route":"connected"}},
      "methods":{"workouts":{"op":"GET /v1/workouts"},"log":{"op":"POST /v1/workouts","effect":"none"}}}}})");
  // An op the spec doesn't have is refused at upload.
  std::string err;
  EXPECT_FALSE(host->boxes().upload(R"({"box":"bad","domains":["kc.example.com"],"external":{"hevy":{"base":")" + api.base() +
                                    R"(","spec":")" + p + R"(","methods":{"x":{"op":"DELETE /v1/nothing"}}}}})", kLoggingBox, &err));
  EXPECT_NE(err.find("has no operation DELETE /v1/nothing"), std::string::npos) << err;

  // Signed out: to sign-in first. Signed in: the form; a bad key is refused; a good one is sealed.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/goober/connect/app/hevy").status, 303);
  std::string cookie = sign_in("ann@example.com");
  auto form = fetch(port, bhttp::verb::get, "/goober/connect/app/hevy?return=/settings", {{"cookie", cookie}});
  EXPECT_NE(form.body.find("Hevy API key"), std::string::npos) << form.body;
  auto post = [&](const std::string& key) {
    return fetch(port, bhttp::verb::post, "/goober/connect/app/hevy",
                 {{"cookie", cookie}, {"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://kc.example.com"}},
                 "key=" + key + "&return=%2Fsettings&ref=row7");
  };
  auto refused = post("bad-key");
  EXPECT_EQ(refused.status, 400);
  EXPECT_NE(refused.body.find("didn&#39;t work"), std::string::npos) << refused.body;
  auto done = post("good-key");
  EXPECT_EQ(done.status, 303) << done.body;
  EXPECT_EQ(done.headers["location"], "/settings");
  std::string sealed = last_credential();
  ASSERT_TRUE(sealed.starts_with("gsv1.")) << last_result_body();
  EXPECT_EQ(last_result_body().find("good-key"), std::string::npos);  // the box never sees it
  EXPECT_NE(last_result_body().find("row7"), std::string::npos);

  // The box's call: the spec's verb and path, the query, a declared header, the constant header, the key.
  auto r = call("hevy", "workouts", R"({"query":{"page":"2"},"headers":{"x-box-header":"hi"},"credential":")" + sealed + R"("})");
  EXPECT_EQ(r.at("status").as_int64(), 200) << json::dump(r);
  auto seen = api.seen("/v1/workouts").back();
  EXPECT_EQ(seen.method, "get");
  EXPECT_EQ(seen.query, "page=2");
  EXPECT_EQ(seen.headers["api-key"], "good-key");
  EXPECT_EQ(seen.headers["x-version"], "2");
  EXPECT_EQ(seen.headers["x-box-header"], "hi");
  // Undeclared headers, no credential, and another service's credential are refused.
  EXPECT_NE(json::dump(call("hevy", "workouts", R"({"headers":{"x-evil":"1"},"credential":")" + sealed + R"("})")).find("isn't a header parameter"),
            std::string::npos);
  EXPECT_NE(json::dump(call("hevy", "workouts", R"({})")).find("carry a credential"), std::string::npos);
  std::string moved = sealed;
  moved[moved.size() - 3] = moved[moved.size() - 3] == 'A' ? 'B' : 'A';
  EXPECT_NE(json::dump(call("hevy", "workouts", R"({"credential":")" + moved + R"("})")).find("doesn't open"), std::string::npos);
  // A body as JSON, from the spec's request body.
  auto w = call("hevy", "log", R"({"body":{"title":"Leg day"},"credential":")" + sealed + R"("})");
  EXPECT_EQ(w.at("status").as_int64(), 200) << json::dump(w);
  EXPECT_EQ(api.seen("/v1/workouts").back().body, R"({"title":"Leg day"})");
  // The owner's own requests may use it; another signed-in person's may not.
  std::smatch owner_m;
  ASSERT_TRUE(std::regex_search(sealed, owner_m, std::regex(R"(^gsv1\.([0-9]+)\.)")));
  EXPECT_EQ(call("hevy", "workouts", R"({"credential":")" + sealed + R"("})", owner_m[1]).at("status").as_int64(), 200);
  EXPECT_NE(json::dump(call("hevy", "workouts", R"({"credential":")" + sealed + R"("})", "999999")).find("another person"),
            std::string::npos);
  // When the check can't say yes (the service is down), nothing is sealed.
  api.on("/v1/user", [](const server::http::Request&) { return json_reply(503, "{}"); });
  auto down = post("good-key");
  EXPECT_EQ(down.status, 400);
  EXPECT_NE(down.body.find("couldn&#39;t be reached"), std::string::npos) << down.body;
  // A later version moving the service to another origin can't take the sealed key with it.
  std::string moved_base = "http://localhost:" + std::to_string(api.port());
  upload(R"({"box":"app","domains":["kc.example.com"],"routes":{"result":{"connected":{},"done":{}}},
    "external":{"hevy":{"base":")" + moved_base + R"(","spec":")" + p + R"(","credentials":"sealed",
      "auth":{"kind":"header","name":"api-key"},"methods":{"workouts":{"op":"GET /v1/workouts"}}}}})");
  EXPECT_NE(json::dump(call("hevy", "workouts", R"({"credential":")" + sealed + R"("})")).find("connect hevy again"),
            std::string::npos);
  // The egress log never holds the key.
  auto log = base::read_file(root / "storage/logs/egress.log");
  ASSERT_TRUE(log);
  EXPECT_EQ(log->find("good-key"), std::string::npos);
}

// The Strava and X shape: OAuth 2 run by the host (state, PKCE), the tokens sealed, refreshed
// once at a time when due, the rotated refresh token kept even when the box keeps an old copy.
TEST_F(HttpTest, OAuthConnectsRefreshesOnceAndRemembersRotation) {
  std::string spec = R"({"swagger":"2.0","info":{"title":"Strava","version":"3"},"basePath":"/api/v3",
    "paths":{"/athlete":{"get":{"operationId":"getLoggedInAthlete"}}}})";
  std::string p = pin("strava", spec);
  std::string err;
  ASSERT_TRUE(host->keys().set("tenant", "kc", "strava_client_id", "cid-1", 1, &err, {"127.0.0.1"})) << err;
  ASSERT_TRUE(host->keys().set("tenant", "kc", "strava_client_secret", "csecret", 1, &err, {"127.0.0.1"})) << err;
  int refreshes = 0;
  api.on("/oauth/token", [&](const server::http::Request& r) {
    if (r.body.find("grant_type=authorization_code") != std::string::npos) {
      return json_reply(200, R"({"access_token":"at-1","refresh_token":"rt-1","expires_in":21600,"scope":"activity:write"})");
    }
    ++refreshes;
    if (r.body.find("refresh_token=rt-1") != std::string::npos) {
      return json_reply(200, R"({"access_token":"at-2","refresh_token":"rt-2","expires_in":21600})");
    }
    return json_reply(400, R"({"error":"invalid_grant"})");
  });
  upload(R"({"box":"app","domains":["kc.example.com"],"routes":{"result":{"connected":{},"done":{}}},
    "external":{"strava":{"base":")" + api.base() + R"(/api/v3","spec":")" + p + R"(","credentials":"sealed","auth":{"kind":"bearer"},
      "connect":{"oauth2":{"authorize":")" + api.base() + R"(/oauth/authorize","token":")" + api.base() + R"(/oauth/token",
        "client-id":"keys/tenant/{!tenant}/strava_client_id","client-secret":"keys/tenant/{!tenant}/strava_client_secret",
        "scopes":["activity:write","read"],"scope-separator":",","pkce":true,"result-route":"connected","early-ms":21700000}},
      "methods":{"athlete":{"op":"getLoggedInAthlete"}}}}})");
  std::string cookie = sign_in("bo@example.com");
  auto go = fetch(port, bhttp::verb::get, "/goober/connect/app/strava?return=/me", {{"cookie", cookie}});
  ASSERT_EQ(go.status, 303) << go.body;
  std::string to = go.headers["location"];
  EXPECT_TRUE(to.starts_with(api.base() + "/oauth/authorize?")) << to;
  EXPECT_NE(to.find("client_id=cid-1"), std::string::npos);
  EXPECT_NE(to.find("scope=activity%3Awrite%2Cread"), std::string::npos) << to;
  EXPECT_NE(to.find("code_challenge_method=S256"), std::string::npos);
  std::smatch m;
  ASSERT_TRUE(std::regex_search(to, m, std::regex("state=([0-9a-f]+)")));
  std::string state = m[1];
  // A made-up state goes nowhere; the real one, once.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/goober/connect/callback?state=nope&code=x", {{"cookie", cookie}}).status, 400);
  auto back = fetch(port, bhttp::verb::get, "/goober/connect/callback?state=" + state + "&code=the-code", {{"cookie", cookie}});
  ASSERT_EQ(back.status, 303) << back.body;
  EXPECT_EQ(back.headers["location"], "/me");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/goober/connect/callback?state=" + state + "&code=the-code", {{"cookie", cookie}}).status, 400);
  auto exchange = api.seen("/oauth/token").at(0);
  EXPECT_NE(exchange.body.find("code_verifier="), std::string::npos);
  EXPECT_NE(exchange.body.find("client_secret=csecret"), std::string::npos);
  std::string sealed = last_credential();
  ASSERT_FALSE(sealed.empty());
  EXPECT_EQ(last_result_body().find("at-1"), std::string::npos);

  // early-ms is past the token's life, so every call is due: the first refreshes (rt-1 to rt-2).
  auto first = call("strava", "athlete", R"({"credential":")" + sealed + R"("})");
  EXPECT_EQ(first.at("status").as_int64(), 200) << json::dump(first);
  EXPECT_EQ(api.seen("/api/v3/athlete").back().auth, "Bearer at-2");
  ASSERT_TRUE(first.contains("credential"));  // the new seal, for the box to keep
  EXPECT_EQ(refreshes, 1);
  // The box kept its old copy: the host's newer generation is used, rt-1 isn't spent twice. The
  // newer one is due again, so it refreshes with rt-2, which the stand-in refuses: expired.
  auto again = call("strava", "athlete", R"({"credential":")" + sealed + R"("})");
  EXPECT_EQ(refreshes, 2);
  auto seen_bodies = api.seen("/oauth/token");
  EXPECT_NE(seen_bodies.back().body.find("refresh_token=rt-2"), std::string::npos) << seen_bodies.back().body;
  EXPECT_NE(json::dump(again).find("connection_expired"), std::string::npos) << json::dump(again);
  EXPECT_NE(json::dump(again).find("/goober/connect/app/strava"), std::string::npos);
}

// X's media shape: a chunked upload (init, appends, finalize) of a stored attachment, then
// polling until done, all one outbox element, reported to the result route.
TEST_F(HttpTest, AChunkedUploadThenPollingReportsToTheResultRoute) {
  std::string spec = R"({"openapi":"3.1.0","info":{"title":"X","version":"2"},"paths":{
    "/2/media/upload/initialize":{"post":{"operationId":"init","requestBody":{"content":{"application/json":{}}}}},
    "/2/media/upload/{id}/append":{"post":{"operationId":"append","parameters":[{"name":"id","in":"path","required":true}]}},
    "/2/media/upload/{id}/finalize":{"post":{"operationId":"finalize","parameters":[{"name":"id","in":"path","required":true}]}},
    "/2/media/upload":{"get":{"operationId":"status","parameters":[{"name":"media_id","in":"query"}]}}}})";
  std::string p = pin("x", spec);
  std::string err;
  ASSERT_TRUE(host->keys().set("tenant", "kc", "x_token", "xt", 1, &err, {"127.0.0.1"}));
  api.on("/2/media/upload/initialize", [](const server::http::Request&) { return json_reply(200, R"({"data":{"id":"m77"}})"); });
  api.on("/2/media/upload/m77/append", [](const server::http::Request&) { return json_reply(204, ""); });
  api.on("/2/media/upload/m77/finalize", [](const server::http::Request&) {
    return json_reply(200, R"({"data":{"id":"m77","processing_info":{"state":"pending","check_after_secs":1}}})");
  });
  int polls = 0;
  api.on("/2/media/upload", [&](const server::http::Request& r) {
    if (r.path != "/2/media/upload") return json_reply(404, "{}");
    return ++polls < 2 ? json_reply(200, R"({"data":{"processing_info":{"state":"in_progress"}}})")
                       : json_reply(200, R"({"data":{"id":"m77","processing_info":{"state":"succeeded"}}})");
  });
  upload(R"({"box":"app","domains":["kc.example.com"],"routes":{"result":{"connected":{},"done":{}}},
    "external":{"x":{"base":")" + api.base() + R"(","spec":")" + p + R"(","credentials":"keys/tenant/{!tenant}/x_token",
      "auth":{"kind":"bearer"},
      "methods":{"ping":{"op":"status","effect":"outbox"},"media":{"op":"init","result-route":"done",
        "upload":{"attachment":"body.media",
          "init":{"op":"init","body":{"media_type":"{content_type}","total_bytes":"{bytes}","media_category":"tweet_image"},"id":"body.data.id"},
          "append":{"op":"append","path":{"id":"{id}"},"parts":{"media":"{chunk}","segment_index":"{index}"},"chunk-bytes":65536},
          "finalize":{"op":"finalize","path":{"id":"{id}"}}},
        "poll":{"op":"status","query":{"media_id":"{body.data.id}"},
          "done-when":{"at":"body.data.processing_info.state","equals":"succeeded"},
          "failed-when":{"at":"body.data.processing_info.state","equals":"failed"},"every-ms":1000}}}}}})");
  // 150 KiB: three appends of 64 KiB.
  std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(150 * 1024, 'p') + std::string("\0\0\0\0IEND\xAE\x42\x60\x82", 12);
  auto a = host->attachments().put("kc", png, "cat.png", "a cat", "public", 1, &err);
  ASSERT_TRUE(a) << err;
  auto queued = call("x", "media", R"({"body":{"media":")" + std::to_string(a->id) + R"("}})");
  ASSERT_TRUE(queued.contains("accepted")) << json::dump(queued);
  int64_t now = base::now_unix_ms();
  host->outbox_tick(now);
  auto init = api.seen("/2/media/upload/initialize");
  ASSERT_EQ(init.size(), 1u);
  EXPECT_NE(init[0].body.find(R"("total_bytes":)" + std::to_string(png.size())), std::string::npos) << init[0].body;
  EXPECT_NE(init[0].body.find(R"("media_type":"image/png")"), std::string::npos);
  auto appends = api.seen("/2/media/upload/m77/append");
  ASSERT_EQ(appends.size(), 3u);
  EXPECT_TRUE(appends[0].content_type.starts_with("multipart/form-data; boundary=")) << appends[0].content_type;
  EXPECT_NE(appends[2].body.find("name=\"segment_index\"\r\n\r\n2\r\n"), std::string::npos);
  EXPECT_EQ(api.seen("/2/media/upload/m77/finalize").size(), 1u);
  EXPECT_EQ(polls, 0);  // waiting every-ms before the first look
  // A polling job is out of the ordered lane: a later call to the same service goes now.
  ASSERT_TRUE(call("x", "ping", R"({"query":{"media_id":"other"}})").contains("accepted"));
  host->outbox_tick(base::now_unix_ms() + 10);  // the ping is due; the poll, every-ms away, isn't
  EXPECT_EQ(api.seen("/2/media/upload").back().query, "media_id=other");
  EXPECT_EQ(polls, 1);  // the ping answered through the same stand-in
  polls = 0;
  host->outbox_tick(now + 1500);
  EXPECT_EQ(polls, 1);
  EXPECT_EQ(api.seen("/2/media/upload").back().query, "media_id=m77");
  host->outbox_tick(now + 3000);
  EXPECT_EQ(polls, 2);
  std::string result = last_result_body();
  EXPECT_NE(result.find("succeeded"), std::string::npos) << result;
  EXPECT_TRUE(std::regex_search(result, std::regex(R"(outcome[\\"]+:[\\"]+done)"))) << result;
  EXPECT_EQ(api.seen("/2/media/upload/m77/append").at(0).auth, "Bearer xt");
}

// Bodies the specs don't make JSON, rate limits, errors on 200, and any-public URLs.
TEST_F(HttpTest, FormsMultipartRawLimitsAndErrorsOn200) {
  std::string err;
  ASSERT_TRUE(host->keys().set("tenant", "kc", "tok", "t0k", 1, &err, {"127.0.0.1"}));
  ASSERT_TRUE(host->keys().set("tenant", "kc", "ck", "xvz1evFS4wEEPTGEFPHBog", 1, &err, {"127.0.0.1"}));
  ASSERT_TRUE(host->keys().set("tenant", "kc", "cs", "kAcSOqF21Fu85e7zjz7ZN2U4ZRhfV3WpwPAoE3Z7kBw", 1, &err, {"127.0.0.1"}));
  ASSERT_TRUE(host->keys().set("tenant", "kc", "tk", "370773112-GmHxMAgYyLbNEtIKZeRNFsMKPR9EyMZeS9weJAEb", 1, &err, {"127.0.0.1"}));
  ASSERT_TRUE(host->keys().set("tenant", "kc", "ts", "LswwdoUaIvS8ltyTt5jkRh4J50vUPVVHtR2YPi5kE", 1, &err, {"127.0.0.1"}));
  api.on("/slack", [](const server::http::Request&) { return json_reply(200, R"({"ok":false,"error":"channel_not_found"})"); });
  api.on("/busy", [](const server::http::Request&) { return json_reply(429, "{}", {{"retry-after", "120"}}); });
  api.on("/huge", [](const server::http::Request&) { return json_reply(429, "{}", {{"retry-after", "9999999999999999999999999"}}); });
  api.on("/forever", [](const server::http::Request&) { return json_reply(503, "{}", {{"retry-after", "999999999"}}); });
  upload(R"({"box":"app","domains":["kc.example.com"],"routes":{"result":{"connected":{},"done":{}}},
    "external":{
      "forms":{"base":")" + api.base() + R"(","credentials":"keys/tenant/{!tenant}/tok","auth":{"kind":"basic","user":"api"},
        "methods":{"status":{"verb":"post","path":"/statuses","body":"form","effect":"none"},
                   "repeat":{"verb":"post","path":"/repeat","body":"form","form-arrays":"repeat","effect":"none"},
                   "file":{"verb":"post","path":"/media","body":"multipart","effect":"none"},
                   "blob":{"verb":"post","path":"/blob","body":"raw","effect":"none"},
                   "slack":{"verb":"post","path":"/slack","effect":"none","error-when":{"at":"body.ok","equals":false}},
                   "busy":{"verb":"post","path":"/busy","result-route":"done"},
                   "huge":{"verb":"post","path":"/huge","effect":"none"},
                   "forever":{"verb":"post","path":"/forever","effect":"none"}}},
      "q":{"base":")" + api.base() + R"(","credentials":"keys/tenant/{!tenant}/tok","auth":{"kind":"query","name":"key"},
        "methods":{"list":{"verb":"get","path":"/list"}}},
      "tw":{"base":")" + api.base() + R"(","auth":{"kind":"oauth1","consumer-key":"keys/tenant/{!tenant}/ck",
        "consumer-secret":"keys/tenant/{!tenant}/cs","token":"keys/tenant/{!tenant}/tk","token-secret":"keys/tenant/{!tenant}/ts"},
        "methods":{"update":{"verb":"post","path":"/1.1/statuses/update.json","body":"form","effect":"none"}}},
      "anywhere":{"base":"*","methods":{"mention":{"verb":"post","path":"/","body":"form","any-public":true,"effect":"none"}}}}})");
  auto f = call("forms", "status", R"({"body":{"status":"Hello Ladies + Gentlemen","media_ids":["1","2"],"meta":{"a":"b"}}})");
  EXPECT_EQ(f.at("status").as_int64(), 200) << json::dump(f);
  auto s = api.seen("/statuses").back();
  EXPECT_EQ(s.content_type, "application/x-www-form-urlencoded");
  EXPECT_EQ(s.body, "status=Hello%20Ladies%20%2B%20Gentlemen&media_ids%5B%5D=1&media_ids%5B%5D=2&meta%5Ba%5D=b");
  EXPECT_EQ(s.auth, "Basic " + base::base64_encode("api:t0k"));
  call("forms", "repeat", R"({"body":{"to":["a","b"]}})");
  EXPECT_EQ(api.seen("/repeat").back().body, "to=a&to=b");
  auto att = host->attachments().put("kc", std::string("\x89PNG\r\n\x1a\nxx", 10) + std::string("\0\0\0\0IEND\xAE\x42\x60\x82", 12), "a\".png",
                                     "", "public", 1, &err);
  ASSERT_TRUE(att) << err;
  std::string id = std::to_string(att->id);
  call("forms", "file", R"({"body":{"file":{"attachment":")" + id + R"("},"payload_json":{"json":{"content":"hi"}},"data_type":"png"}})");
  auto mp = api.seen("/media").back();
  EXPECT_NE(mp.body.find("name=\"file\"; filename=\"a_.png\"\r\nContent-Type: image/png"), std::string::npos) << mp.body;
  EXPECT_NE(mp.body.find("name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n{\"content\":\"hi\"}"), std::string::npos);
  EXPECT_NE(mp.body.find("name=\"data_type\"\r\n\r\npng"), std::string::npos);
  call("forms", "blob", R"({"body":{"attachment":")" + id + R"("}})");
  EXPECT_EQ(api.seen("/blob").back().content_type, "image/png");
  EXPECT_TRUE(api.seen("/blob").back().body.starts_with("\x89PNG"));
  EXPECT_NE(json::dump(call("forms", "blob", R"({"body":{"attachment":"999"}})")).find("isn't one of this tenant's"), std::string::npos);
  // Slack answers 200 with ok: false.
  auto sl = call("forms", "slack", R"({})");
  EXPECT_NE(json::dump(sl).find("error-when"), std::string::npos) << json::dump(sl);
  // A query key.
  call("q", "list", R"({"query":{"page":"1"}})");
  EXPECT_EQ(api.seen("/list").back().query, "page=1&key=t0k");
  // any-public: the whole URL from the call (here the loopback stand-in, local mode); a non-https one refused.
  auto wm = call("anywhere", "mention", R"({"url":")" + api.base() + R"(/webmention","body":{"source":"https://kc.example.com/p","target":"https://x.example/a"}})");
  EXPECT_EQ(wm.at("status").as_int64(), 200) << json::dump(wm);
  EXPECT_NE(json::dump(call("anywhere", "mention", R"({"url":"http://10.0.0.1/x","body":{}})")).find("https"), std::string::npos);
  // OAuth 1.0a: the signature the service gets checks out with the secrets it would hold.
  call("tw", "update", R"({"query":{"include_entities":"true"},"body":{"status":"Hello Ladies + Gentlemen, a signed OAuth request!"}})");
  auto signed_req = api.seen("/1.1/statuses/update.json").back();
  std::map<std::string, std::string> oa;
  std::string h = signed_req.auth;
  ASSERT_TRUE(h.starts_with("OAuth ")) << h;
  std::regex pair_rx(R"re(([a-z_]+)="([^"]*)")re");
  for (std::sregex_iterator it(h.begin(), h.end(), pair_rx), end; it != end; ++it) oa[(*it)[1]] = (*it)[2];
  EXPECT_EQ(oa["oauth_consumer_key"], "xvz1evFS4wEEPTGEFPHBog");
  EXPECT_EQ(oa["oauth_token"], "370773112-GmHxMAgYyLbNEtIKZeRNFsMKPR9EyMZeS9weJAEb");
  signing::OAuth1 o{"xvz1evFS4wEEPTGEFPHBog", "kAcSOqF21Fu85e7zjz7ZN2U4ZRhfV3WpwPAoE3Z7kBw",
                    "370773112-GmHxMAgYyLbNEtIKZeRNFsMKPR9EyMZeS9weJAEb", "LswwdoUaIvS8ltyTt5jkRh4J50vUPVVHtR2YPi5kE",
                    oa["oauth_nonce"], oa["oauth_timestamp"]};
  std::string expect = signing::oauth1_signature(
      signing::oauth1_base_string("POST", api.base() + "/1.1/statuses/update.json?include_entities=true",
                                  {{"status", "Hello Ladies + Gentlemen, a signed OAuth request!"}}, o), o);
  EXPECT_EQ(oa["oauth_signature"], signing::rfc3986(expect));
  // A Retry-After too long to parse is ignored; one of decades is capped at a day.
  auto huge = call("forms", "huge", R"({})");
  EXPECT_EQ(huge.at("status").as_int64(), 429) << json::dump(huge);
  EXPECT_FALSE(huge.contains("retry_at"));
  auto forever = call("forms", "forever", R"({})");
  ASSERT_TRUE(forever.contains("retry_at")) << json::dump(forever);
  EXPECT_LE(std::string(forever.at("retry_at").as_string()), base::format_utc_ms(base::now_unix_ms() + 24 * 3600 * 1000 + 5000));
  // Retry-After: the outbox waits as long as the service said, not its own backoff.
  call("forms", "busy", R"({})");
  int64_t now = base::now_unix_ms();
  host->outbox_tick(now);
  EXPECT_EQ(api.seen("/busy").size(), 1u);
  host->outbox_tick(now + 60000);
  EXPECT_EQ(api.seen("/busy").size(), 1u);  // its backoff alone would have been a second
  host->outbox_tick(now + 121000);
  EXPECT_EQ(api.seen("/busy").size(), 2u);
}

// A sandbox (a rehydrated devbox) sends nothing off the machine, whatever path asks; and a URL
// with user info is never called (it names one host to the checks, another to curl).
TEST(SendHttp, ASandboxReachesNothingOutsideAndUserInfoIsRefused) {
  auto scratch = fs::path(GOOBER_TEST_TMP) / "send_http";
  fs::create_directories(scratch);
  host::set_outbound_sandbox(true);
  auto out = host::send_http("GET", "https://example.com/", {}, "", "", false, scratch, 2000);
  host::set_outbound_sandbox(false);
  EXPECT_NE(out.error.find("sandbox"), std::string::npos) << out.error;
  auto ui = host::send_http("GET", "https://8.8.8.8:x@10.0.0.5/", {}, "", "", false, scratch, 2000);
  EXPECT_NE(ui.error.find("user info"), std::string::npos) << ui.error;
}
