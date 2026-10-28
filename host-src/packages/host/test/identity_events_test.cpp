// Identity routes (design/machine/021.IdentityEvents.md): boxes hear first verifications and
// sign-ins after they commit, once each, in order, with retries and a cursor that survives restarts.

#include <gtest/gtest.h>

#include <algorithm>
#include <regex>

#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr int64_t kNow = 1'800'000'000'000;

// Each identity route writes one row naming itself; "grumpy" fails until the table says not to.
const char* kWat = R"wat((module
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "w")
  (data (i32.const 16) "INSERT INTO heard (what) VALUES (?)")
  (data (i32.const 64) "[\22welcome\22]")
  (data (i32.const 80) "[\22signin\22]")
  (data (i32.const 96) "[\22grumpy\22]")
  (data (i32.const 128) "SELECT 1 FROM moods WHERE ok = 1")
  (func $note (param $p i32) (param $n i32)
    (drop (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 35) (local.get $p) (local.get $n))))
  (func (export "goober_route_welcome") (result i32) (call $note (i32.const 64) (i32.const 11)) (i32.const 0))
  (func (export "goober_route_on_sign_in") (result i32) (call $note (i32.const 80) (i32.const 10)) (i32.const 0))
  (func (export "goober_route_grumpy") (result i32)
    ;; goober_sql answers its result's length: 58 bytes is {"columns":["1"],"rows":[],...}, no row.
    (if (i32.le_s (call $sql (i32.const 0) (i32.const 1) (i32.const 128) (i32.const 32) (i32.const 0) (i32.const 0))
                  (i32.const 58))
      (then unreachable))
    (call $note (i32.const 96) (i32.const 10))
    (i32.const 0))
))wat";

const char* kSchema = R"({"schema":"heard1","tables":{
  "heard":{"columns":{"what":{"type":"string","max-length":40}}},
  "moods":{"columns":{"ok":{"type":"boolean","default":false}}}}})";

const char* kManifest = R"({"box":"greeter","domains":["*"],
  "data":{"sqlite3":{"w":{"schema":"heard1","database":"heard","pattern":"sql",
                          "tables":{"heard":{"read":true,"insert":true},"moods":{"read":true}}}}},
  "routes":{"identity":{
    "first-verified":{"name":"welcome"},
    "signed-in":{"name":"on_sign_in"}}}})";

const char* kGrumpyManifest = R"({"box":"grumpy","domains":["*"],
  "data":{"sqlite3":{"w":{"schema":"heard1","database":"heard","pattern":"sql",
                          "tables":{"heard":{"read":true,"insert":true},"moods":{"read":true}}}}},
  "routes":{"identity":{"signed-in":{"name":"grumpy"}}}})";

}  // namespace

class IdentityEventsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "identity_events";
    fs::remove_all(root);
    start();
    std::string err;
    ASSERT_TRUE(host->schemas().put(kSchema, &err)) << err;
    auto schema = host->schemas().get("heard1");
    ASSERT_TRUE(host->databases().open("playground", "heard", &*schema, &err)) << err;
  }
  void start() {
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    host->set_timers_inline(true);
  }
  // Signs in by an emailed code, straight through the identity service.
  void sign_in(const std::string& email, int64_t at) {
    host->identity().start_email("playground", email, "localhost", true, at);
    host->drain_mail();
    auto files = base::list_files(root / "storage/mail/outbox");
    std::sort(files.begin(), files.end());
    auto text = *base::read_file(root / "storage/mail/outbox" / files.back());
    std::smatch m;
    ASSERT_TRUE(std::regex_search(text, m, std::regex("\\b([0-9]{6})\\b")));
    std::string why;
    ASSERT_TRUE(host->identity().verify_email("playground", email, m[1].str(), true, at + 1, 86400000, &why)) << why;
  }
  int64_t count(const char* what) {
    std::string err;
    auto r = host->databases().operate("playground", "heard", "SELECT count(*) FROM heard WHERE what = ?",
                                       bj::array{what}, 10, false, &err);
    return r ? r->at("rows").at(0).at(0).as_int64() : -1;
  }
  fs::path root;
  std::unique_ptr<host::Host> host;
};

TEST_F(IdentityEventsTest, BoxesHearFirstVerificationsAndEverySignIn) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kManifest, kWat, &err)) << err;
  host->identity_tick(kNow);  // the routes start hearing from here
  sign_in("ada@example.com", kNow + 1000);
  EXPECT_EQ(count("welcome"), 0);  // sign-in never waits on a box
  host->identity_tick(kNow + 2000);
  EXPECT_EQ(count("welcome"), 1);
  EXPECT_EQ(count("signin"), 1);

  // Coming back is a sign-in, not a first verification.
  sign_in("ada@example.com", kNow + 3000);
  host->identity_tick(kNow + 4000);
  EXPECT_EQ(count("welcome"), 1);
  EXPECT_EQ(count("signin"), 2);
  host->identity_tick(kNow + 5000);
  EXPECT_EQ(count("signin"), 2);  // each event once

  // The cursor survives a restart: nothing is heard twice, and what came meanwhile is heard.
  host.reset();
  start();
  sign_in("bo@example.com", kNow + 6000);
  host->identity_tick(kNow + 7000);
  EXPECT_EQ(count("welcome"), 2);
  EXPECT_EQ(count("signin"), 3);
}

TEST_F(IdentityEventsTest, ANewRouteHearsOnlyWhatComesAfterIt) {
  sign_in("early@example.com", kNow);
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kManifest, kWat, &err)) << err;
  host->identity_tick(kNow + 1000);
  host->identity_tick(kNow + 2000);
  EXPECT_EQ(count("welcome"), 0);
  sign_in("late@example.com", kNow + 3000);
  host->identity_tick(kNow + 4000);
  EXPECT_EQ(count("welcome"), 1);
}

TEST_F(IdentityEventsTest, AFailingRouteRetriesInOrderThenGivesUp) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kGrumpyManifest, kWat, &err)) << err;
  host->identity_tick(kNow);
  sign_in("cy@example.com", kNow + 1000);
  host->identity_tick(kNow + 2000);  // fails: retried after two seconds
  EXPECT_EQ(count("grumpy"), 0);
  host->identity_tick(kNow + 3000);  // still backing off
  // Now it works, and the event is delivered on the retry.
  ASSERT_TRUE(host->databases().operate("playground", "heard", "INSERT INTO moods (ok) VALUES (1)", nullptr, 10, true, &err)) << err;
  host->identity_tick(kNow + 3500);
  EXPECT_EQ(count("grumpy"), 0);
  host->identity_tick(kNow + 4500);
  EXPECT_EQ(count("grumpy"), 1);

  // A route that never succeeds gives the event up after eight tries and moves on.
  ASSERT_TRUE(host->databases().operate("playground", "heard", "DELETE FROM moods", nullptr, 10, true, &err)) << err;
  sign_in("dee@example.com", kNow + 5000);
  int64_t t = kNow + 6000;
  for (int i = 0; i < 40; ++i, t += 11 * 60 * 1000) host->identity_tick(t);
  ASSERT_TRUE(host->databases().operate("playground", "heard", "INSERT INTO moods (ok) VALUES (1)", nullptr, 10, true, &err)) << err;
  host->identity_tick(t);
  EXPECT_EQ(count("grumpy"), 1);  // dee's sign-in was given up, not delivered late
  sign_in("eve@example.com", t + 1000);
  host->identity_tick(t + 2000);
  EXPECT_EQ(count("grumpy"), 2);
}
