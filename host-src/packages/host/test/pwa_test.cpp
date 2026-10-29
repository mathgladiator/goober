// Progressive web apps (design/machine/782.PWA.md): an app over part of a site, its manifest,
// worker, and registration, push subscriptions, and a box's push delivered through the outbox
// to a stand-in push service, which the test decrypts as a browser would.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <mutex>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/server_http/server.h"
#include "goober/webpush/webpush.h"

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
  req.set(bhttp::field::host, "localhost:8080");
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
  return v ? *v : bj::value();
}

// A push service: keeps what arrives, answers with the next status queued (201 by default).
class PushService {
 public:
  struct Got {
    std::string target, body, authorization, encoding, ttl, urgency, topic;
  };
  PushService() {
    server::http::ServerOptions o;
    o.port = 0;
    o.threads = 1;
    server_ = std::make_unique<server::http::Server>(o, [this](server::http::Request req, server::http::Responder respond) {
      server::http::Response res;
      std::lock_guard lock(mu_);
      got_.push_back({req.target, req.body, req.header("authorization"), req.header("content-encoding"), req.header("ttl"),
                      req.header("urgency"), req.header("topic")});
      res.status = statuses_.empty() ? 201 : statuses_.front();
      if (!statuses_.empty()) statuses_.erase(statuses_.begin());
      respond(std::move(res));
    });
    server_->start();
  }
  uint16_t port() const { return server_->port(); }
  std::vector<Got> got() {
    std::lock_guard lock(mu_);
    return got_;
  }
  void answer(std::vector<int> s) {
    std::lock_guard lock(mu_);
    statuses_ = std::move(s);
  }

 private:
  std::mutex mu_;
  std::vector<Got> got_;
  std::vector<int> statuses_;
  std::unique_ptr<server::http::Server> server_;
};

// A WAT data segment's text: JSON with its quotes escaped.
std::string wat(const std::string& s) {
  std::string out;
  for (char c : s) out += c == '"' ? std::string("\\22") : std::string(1, c);
  return out;
}

const char* kShop = R"({"scope": "/shop/", "manifest": {"name": "KC Meat Shop", "short_name": "Meat", "theme_color": "#8b1e1e",
  "icons": [{"src": "/icons/192.png", "sizes": "192x192", "type": "image/png"},
            {"src": "/icons/512.png", "sizes": "512x512", "type": "image/png"}]},
  "offline": {"fallback": "/shop/offline"}, "push": {"anonymous": true}})";

}  // namespace

class PwaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "pwa";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "operations": {"token": "test-token-0123456789"},
        "pwa": {"push-hosts": ["127.0.0.1"], "push-contact": "mailto:ops@example.com"},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }
  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp", {{"authorization", "Bearer test-token-0123456789"}},
                   json::dump(req));
    auto doc = parse(r.body);
    const auto& result = doc.at("result");
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::object{{"error", text}};
    auto v = parse(text);
    return v.is_null() ? bj::value(text) : v;
  }
  // Subscribes as a browser would, with a fresh key; returns the browser's key and auth secret.
  std::pair<webpush::Key, std::string> subscribe(const std::string& path, std::vector<std::string> topics = {}) {
    std::string err;
    auto key = *webpush::generate_key(&err);
    std::string auth(16, '\x05');
    bj::array t;
    for (auto& x : topics) t.push_back(bj::value(x));
    bj::object sub{{"endpoint", "http://127.0.0.1:" + std::to_string(push.port()) + path},
                   {"keys", bj::object{{"p256dh", webpush::b64url(key.public_raw)}, {"auth", webpush::b64url(auth)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober/pwa/shop/push",
                   {{"content-type", "application/json"}, {"origin", "http://localhost:8080"}},
                   json::dump(bj::object{{"subscription", sub}, {"topics", t}}));
    EXPECT_EQ(r.status, 201) << r.body;
    return {key, auth};
  }

  PushService push;
  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(PwaTest, PartOfASiteBecomesAnInstallableApp) {
  auto put = tool("pwa_put", {{"tenant", "playground"}, {"name", "shop"}, {"app", parse(kShop)}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  EXPECT_EQ(put.at("warnings").as_array().size(), 0u);
  tool("page_put", {{"tenant", "playground"}, {"path", "/shop/"}, {"format", "markdown"}, {"body", "# Shop"}});
  tool("page_put", {{"tenant", "playground"}, {"path", "/about"}, {"format", "markdown"}, {"body", "# About"}});

  // Pages in scope name the app; pages outside it don't.
  auto shop = fetch(port, bhttp::verb::get, "/shop/");
  ASSERT_EQ(shop.status, 200);
  EXPECT_NE(shop.body.find(R"(<link rel="manifest" href="/goober/pwa/shop/manifest.webmanifest">)"), std::string::npos);
  EXPECT_NE(shop.body.find(R"(<script src="/goober/pwa/shop/register.js" defer></script>)"), std::string::npos);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/about").body.find("manifest"), std::string::npos);

  auto manifest = fetch(port, bhttp::verb::get, "/goober/pwa/shop/manifest.webmanifest");
  ASSERT_EQ(manifest.status, 200);
  EXPECT_EQ(manifest.headers["content-type"], "application/manifest+json");
  auto m = parse(manifest.body);
  EXPECT_EQ(m.at("scope"), "/shop/");
  EXPECT_EQ(m.at("start_url"), "/shop/");
  EXPECT_EQ(m.at("display"), "standalone");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/goober/pwa/shop/manifest.webmanifest", {{"if-none-match", manifest.headers["etag"]}})
                .status,
            304);

  // The worker lives under /goober/pwa/ and controls the app's scope.
  auto sw = fetch(port, bhttp::verb::get, "/goober/pwa/shop/sw.js");
  ASSERT_EQ(sw.status, 200);
  EXPECT_EQ(sw.headers["service-worker-allowed"], "/shop/");
  EXPECT_EQ(sw.headers["content-type"], "text/javascript; charset=utf-8");
  EXPECT_EQ(sw.headers["cache-control"], "no-cache");
  EXPECT_NE(sw.body.find("\"fallback\":\"/shop/offline\""), std::string::npos);

  // Registration carries the host's VAPID key, the same one GET push gives.
  auto reg = fetch(port, bhttp::verb::get, "/goober/pwa/shop/register.js");
  std::string key(parse(fetch(port, bhttp::verb::get, "/goober/pwa/shop/push").body).at("public_key").as_string());
  EXPECT_EQ(webpush::b64url_decode(key)->size(), 65u);
  EXPECT_NE(reg.body.find("\"push_key\":\"" + key + "\""), std::string::npos);

  EXPECT_EQ(fetch(port, bhttp::verb::get, "/goober/pwa/nope/manifest.webmanifest").status, 404);
  // An app over the whole domain fits /about, and the shop keeps its own pages.
  tool("pwa_put", {{"tenant", "playground"}, {"name", "site"}, {"app", parse(R"({"manifest": {"name": "Playground"}})")}});
  EXPECT_NE(fetch(port, bhttp::verb::get, "/about").body.find("/goober/pwa/site/manifest.webmanifest"), std::string::npos);
  EXPECT_NE(fetch(port, bhttp::verb::get, "/shop/").body.find("/goober/pwa/shop/manifest.webmanifest"), std::string::npos);
  EXPECT_TRUE(tool("pwa_put", {{"tenant", "playground"}, {"name", "x"}, {"app", parse(R"({"manifest": {"name": "Dup"}})")}})
                  .as_object()
                  .contains("error"));  // two apps can't share a scope
  auto list = tool("pwa_list", {{"tenant", "playground"}});
  EXPECT_EQ(list.at("apps").as_array().size(), 2u);
}

TEST_F(PwaTest, ABoxsPushIsEncryptedForEachSubscriberAndGoneOnesAreDropped) {
  tool("pwa_put", {{"tenant", "playground"}, {"name", "shop"}, {"app", parse(kShop)}});
  // Subscriptions come as JSON from this site only.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/goober/pwa/shop/push",
                  {{"content-type", "application/json"}, {"origin", "https://evil.example"}}, "{}")
                .status,
            403);
  auto bad = fetch(port, bhttp::verb::post, "/goober/pwa/shop/push",
                   {{"content-type", "application/json"}, {"origin", "http://localhost:8080"}},
                   R"({"subscription": {"endpoint": "https://attacker.example/x", "keys": {"p256dh": "AA", "auth": "AA"}}})");
  EXPECT_EQ(bad.status, 400);
  EXPECT_NE(bad.body.find("isn't a browser's push service"), std::string::npos) << bad.body;

  auto [ua, auth] = subscribe("/push/one", {"orders"});
  subscribe("/push/two", {"news"});
  auto counts = tool("pwa_subscriptions", {{"tenant", "playground"}});
  EXPECT_EQ(counts.at("subscriptions").as_int64(), 2);
  EXPECT_EQ(counts.at("by_topic").at("orders").as_int64(), 1);

  std::string err;
  ASSERT_TRUE(host->schemas().put(R"({"schema":"shop1","tables":{"orders":{"columns":{"note":{"type":"string"}}}}})", &err)) << err;
  std::string request = R"({"to":{"topic":"orders"},"title":"Pickup ready","body":"Order 7 is ready","url":"/shop/orders/7","tag":"order-7"})";
  std::string module = R"((module
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "alerts")
  (data (i32.const 32) "send")
  (data (i32.const 64) ")" + wat(request) + R"(")
  (func (export "goober_route_ready") (result i32) (local $n i32)
    (local.set $n (call $call (i32.const 16) (i32.const 6) (i32.const 32) (i32.const 4) (i32.const 64) (i32.const )" +
                       std::to_string(request.size()) + R"()))
    (drop (call $result (i32.const 4096) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 4096) (local.get $n)))
    (i32.const 0))
))";
  ASSERT_TRUE(host->boxes().upload(R"({"box":"shop","domains":["*"],
      "data":{"sqlite3":{"db":{"schema":"shop1","database":"shop","pattern":"sql","tables":{"orders":{"read":true}}}}},
      "external":{"alerts":{"push":{}}},
      "routes":{"http":{"/shop/api/ready":{"post":{"name":"ready","public":true}}}}})",
                                   module, &err))
      << err;
  auto sent = fetch(port, bhttp::verb::post, "/shop/api/ready");
  ASSERT_EQ(sent.status, 200) << sent.body;
  EXPECT_NE(sent.body.find("\"accepted\":true"), std::string::npos) << sent.body;
  EXPECT_TRUE(push.got().empty());  // after commit, through the outbox

  host->outbox_tick(base::now_unix_ms());
  auto got = push.got();
  ASSERT_EQ(got.size(), 1u);  // the "orders" subscriber only
  EXPECT_EQ(got[0].target, "/push/one");
  EXPECT_EQ(got[0].encoding, "aes128gcm");
  EXPECT_EQ(got[0].ttl, "86400");
  ASSERT_TRUE(got[0].authorization.starts_with("vapid t="));
  std::string jwt = got[0].authorization.substr(8, got[0].authorization.find(", k=") - 8);
  std::string vapid = got[0].authorization.substr(got[0].authorization.find(", k=") + 4);
  EXPECT_TRUE(webpush::verify_jwt(jwt, *webpush::b64url_decode(vapid)));
  auto claims = *webpush::b64url_decode(jwt.substr(jwt.find('.') + 1, jwt.rfind('.') - jwt.find('.') - 1));
  EXPECT_NE(claims.find("\"aud\":\"http://127.0.0.1:" + std::to_string(push.port()) + "\""), std::string::npos) << claims;
  EXPECT_NE(claims.find("\"sub\":\"mailto:ops@example.com\""), std::string::npos);
  auto plain = webpush::decrypt(got[0].body, ua, auth, &err);
  ASSERT_TRUE(plain) << err;
  auto note = parse(*plain);
  EXPECT_EQ(note.at("title"), "Pickup ready");
  EXPECT_EQ(note.at("url"), "/shop/orders/7");
  // Delivered once: another tick sends nothing more.
  host->outbox_tick(base::now_unix_ms() + 3600 * 1000);
  EXPECT_EQ(push.got().size(), 1u);

  // An operator's try, to everyone: the second browser is gone (410), so it's dropped.
  push.answer({201, 410});
  auto r = tool("push_send", {{"tenant", "playground"}, {"to", bj::object{{"everyone", true}}}, {"title", "Hello"}});
  EXPECT_EQ(r.at("sent").as_int64(), 1) << json::dump(r);
  EXPECT_EQ(r.at("gone").as_int64(), 1);
  EXPECT_EQ(tool("pwa_subscriptions", {{"tenant", "playground"}}).at("subscriptions").as_int64(), 1);
  // A user id as a number, which no subscriber here has, reaches nobody (never everyone).
  auto none = tool("push_send", {{"tenant", "playground"}, {"to", bj::object{{"user_id", 42}}}, {"title", "x"}});
  EXPECT_EQ(none.at("subscriptions").as_int64(), 0) << json::dump(none);
  EXPECT_TRUE(tool("push_send", {{"tenant", "playground"}, {"to", bj::object{{"who", "all"}}}, {"title", "x"}})
                  .as_object()
                  .contains("error"));
}

TEST_F(PwaTest, SignedOutVisitorsSubscribeOnlyWhenTheAppSaysSo) {
  tool("pwa_put", {{"tenant", "playground"}, {"name", "shop"},
                   {"app", parse(R"({"scope": "/shop/", "manifest": {"name": "Shop"}})")}});
  std::string err;
  auto key = *webpush::generate_key(&err);
  bj::object sub{{"endpoint", "http://127.0.0.1:" + std::to_string(push.port()) + "/p"},
                 {"keys", bj::object{{"p256dh", webpush::b64url(key.public_raw)}, {"auth", webpush::b64url(std::string(16, 'a'))}}}};
  auto r = fetch(port, bhttp::verb::post, "/goober/pwa/shop/push",
                 {{"content-type", "application/json"}, {"origin", "http://localhost:8080"}},
                 json::dump(bj::object{{"subscription", sub}}));
  EXPECT_EQ(r.status, 401) << r.body;
  tool("pwa_put", {{"tenant", "playground"}, {"name", "quiet"},
                   {"app", parse(R"({"scope": "/quiet/", "manifest": {"name": "Quiet"}, "push": false})")}});
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/goober/pwa/quiet/push",
                  {{"content-type", "application/json"}, {"origin", "http://localhost:8080"}}, json::dump(bj::object{{"subscription", sub}}))
                .status,
            403);
}
