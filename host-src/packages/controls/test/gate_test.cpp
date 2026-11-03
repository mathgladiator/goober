// The controls gate (design/machine/047.Controls.md § 5, § 6): each refusal and its status,
// observe refusing nothing but counting, by: user, slots released on every path, and the rows.

#include <gtest/gtest.h>

// Requests are written with designated initializers, naming only what each test changes.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include "goober/controls/gate.h"

using namespace goober;
using namespace goober::controls;

namespace {

const HostLimits kLimits{8 << 20, 30000};
constexpr int64_t kT0 = 1'800'000'000'000;  // a bucket's start

std::shared_ptr<const DomainControls> compiled(std::string_view file) {
  std::string err;
  auto v = json::parse(file, &err);
  EXPECT_TRUE(v) << err;
  auto d = compile(v->as_object(), "example.com", "example", std::nullopt, kLimits, &err);
  EXPECT_TRUE(d) << err;
  return std::make_shared<const DomainControls>(std::move(*d));
}

struct Req {
  std::string method = "get";
  std::vector<std::string> path;
  std::string api = "";
  std::string client = "192.0.2.1";
  std::string user;
  int64_t body = 0;
  int64_t now = kT0;
};

Verdict ask(Gate& g, const DomainControls& d, const Req& r) {
  Ask a;
  a.method = r.method;
  a.path = &r.path;
  a.api = r.api;
  a.client = r.client;
  std::string user = r.user;
  a.user = [user] { return user; };
  a.body_bytes = r.body;
  a.now_ms = r.now;
  return g.check(d, a);
}

operations::ControlRow row_of(Gate& g, const std::string& id) {
  for (auto& r : g.take_all()) {
    if (r.control == id) return r;
  }
  return {};
}

}  // namespace

TEST(ControlsGate, RateRefusesWith429AndRetryAfter) {
  Gate g;
  auto d = compiled(R"({"endpoints":[{"id":"login","match":"POST /login","mode":"enforce",
                                       "set":{"rate":{"per-minute":6,"burst":2}}}]})");
  Req r{.method = "post", .path = {"login"}};
  EXPECT_FALSE(ask(g, *d, r).refused);
  EXPECT_FALSE(ask(g, *d, r).refused);
  auto third = ask(g, *d, r);
  EXPECT_TRUE(third.refused);
  EXPECT_EQ(third.status, 429);
  EXPECT_EQ(third.reason, Reason::kRate);
  EXPECT_EQ(third.control, "login");
  EXPECT_EQ(third.retry_after_s, 10);  // one token every ten seconds
  // Another client has its own bucket; an unmatched path meets nothing.
  EXPECT_FALSE(ask(g, *d, Req{.method = "post", .path = {"login"}, .client = "192.0.2.2"}).refused);
  EXPECT_FALSE(ask(g, *d, Req{.method = "post", .path = {"other"}}).matched);
  // Ten seconds later, one more.
  EXPECT_FALSE(ask(g, *d, Req{.method = "post", .path = {"login"}, .now = kT0 + 10'000}).refused);
  auto row = row_of(g, "login");
  EXPECT_EQ(row.matched, 5);
  EXPECT_EQ(row.allowed, 4);
  EXPECT_EQ(row.rate, 1);
  EXPECT_EQ(sketch_count(row.clients), 1);
  EXPECT_EQ(row.top_n, 1);
}

TEST(ControlsGate, ByAllSharesOneBucketByUserCountsTheAccount) {
  Gate g;
  auto all = compiled(R"({"defaults":{"mode":"enforce","set":{"rate":{"per-minute":1,"burst":1,"by":"all"}}}})");
  EXPECT_FALSE(ask(g, *all, Req{.client = "a"}).refused);
  EXPECT_TRUE(ask(g, *all, Req{.client = "b"}).refused);  // everyone shares it

  Gate u;
  auto by_user = compiled(R"({"defaults":{"mode":"enforce","set":{"rate":{"per-minute":1,"burst":1,"by":"user"}}}})");
  // One account from two addresses: one budget.
  EXPECT_FALSE(ask(u, *by_user, Req{.client = "a", .user = "4294967296"}).refused);
  EXPECT_TRUE(ask(u, *by_user, Req{.client = "b", .user = "4294967296"}).refused);
  // Another account from the first address: its own.
  EXPECT_FALSE(ask(u, *by_user, Req{.client = "a", .user = "4294967297"}).refused);
  // Nobody signed in: the client's.
  EXPECT_FALSE(ask(u, *by_user, Req{.client = "a"}).refused);
  EXPECT_TRUE(ask(u, *by_user, Req{.client = "a"}).refused);
}

TEST(ControlsGate, ConcurrencyRefusesWith503AndReleasesOnEveryPath) {
  Gate g;
  auto d = compiled(R"({"apis":{"box:shop":{"mode":"enforce","set":{"concurrent":{"max":2,"per-client":1}}}}})");
  Req a{.api = "box:shop", .client = "a"}, b{.api = "box:shop", .client = "b"}, c{.api = "box:shop", .client = "c"};
  auto first = ask(g, *d, a);
  ASSERT_TRUE(first.ticket);
  auto again = ask(g, *d, a);  // the same client's second at once
  EXPECT_TRUE(again.refused);
  EXPECT_EQ(again.status, 503);
  EXPECT_EQ(again.reason, Reason::kConcurrent);
  EXPECT_EQ(again.retry_after_s, 1);
  auto second = ask(g, *d, b);
  ASSERT_TRUE(second.ticket);
  EXPECT_EQ(g.in_flight("example.com", "box:shop"), 2);
  EXPECT_TRUE(ask(g, *d, c).refused);  // the API is full
  first.ticket->done(200, 1000, kT0);
  first.ticket->done(200, 1000, kT0);  // a second done does nothing
  EXPECT_EQ(g.in_flight("example.com", "box:shop"), 1);
  // A ticket dropped without done (a handler that threw) gives its slot back too.
  second.ticket.reset();
  EXPECT_EQ(g.in_flight("example.com", "box:shop"), 0);
  EXPECT_FALSE(ask(g, *d, c).refused);
  auto row = row_of(g, "box:shop");
  EXPECT_EQ(row.concurrent, 2);
  EXPECT_EQ(row.peak, 2);
  EXPECT_EQ(row.hist.total(), 1);  // only the one that finished is timed
}

TEST(ControlsGate, BodyAndClosedRefuseWithTheirStatus) {
  Gate g;
  auto d = compiled(R"({"endpoints":[
      {"id":"upload","match":"POST /upload","mode":"enforce","set":{"body-max-bytes":100}},
      {"id":"gone","match":"* /old/{rest*}","mode":"enforce","set":{"closed":{"status":410,"message":"Moved for good."}}},
      {"id":"pause","match":"* /shop/{rest*}","mode":"enforce","set":{"closed":{"retry-after-s":600}}}]})");
  auto big = ask(g, *d, Req{.method = "post", .path = {"upload"}, .body = 101});
  EXPECT_TRUE(big.refused);
  EXPECT_EQ(big.status, 413);
  EXPECT_EQ(big.reason, Reason::kBody);
  EXPECT_FALSE(ask(g, *d, Req{.method = "post", .path = {"upload"}, .body = 100}).refused);
  auto gone = ask(g, *d, Req{.path = {"old", "page"}});
  EXPECT_EQ(gone.status, 410);
  EXPECT_EQ(gone.message, "Moved for good.");
  EXPECT_EQ(gone.retry_after_s, -1);
  auto pause = ask(g, *d, Req{.method = "post", .path = {"shop", "cart"}});
  EXPECT_EQ(pause.status, 503);
  EXPECT_EQ(pause.reason, Reason::kClosed);
  EXPECT_EQ(pause.retry_after_s, 600);
}

// An observed control refuses nothing and counts what it would have refused, on its own buckets:
// the enforced level below it keeps working.
TEST(ControlsGate, ObserveRefusesNothingButCounts) {
  Gate g;
  auto d = compiled(R"({"defaults":{"mode":"enforce","set":{"rate":{"per-minute":60,"burst":3}}},
      "endpoints":[{"id":"try_tight","match":"GET /feed","mode":"observe",
                    "set":{"rate":{"per-minute":1,"burst":1},"body-max-bytes":10,"closed":true,"timeout-ms":5}}]})");
  for (int i = 0; i < 3; ++i) {
    auto v = ask(g, *d, Req{.path = {"feed"}, .body = 20});
    EXPECT_FALSE(v.refused) << i;
    EXPECT_FALSE(v.would.empty());
    EXPECT_EQ(v.id, "try_tight");
    v.ticket->done(200, 10'000, kT0);  // slower than the observed 5 ms
  }
  // The domain's enforced rate still refuses the fourth.
  auto fourth = ask(g, *d, Req{.path = {"feed"}});
  EXPECT_TRUE(fourth.refused);
  EXPECT_EQ(fourth.control, "defaults");
  auto rows = g.take_all();
  operations::ControlRow tight, defaults;
  for (auto& r : rows) (r.control == "try_tight" ? tight : defaults) = r;
  EXPECT_EQ(tight.matched, 4);
  EXPECT_EQ(tight.refused(), 0);
  EXPECT_EQ(tight.would, 4);  // once per request, whatever the reasons
  EXPECT_EQ(sketch_count(tight.would_clients), 1);
  EXPECT_EQ(defaults.rate, 1);
  EXPECT_EQ(defaults.matched, 4);
}

TEST(ControlsGate, ObservedTimeoutCountsSlowCalls) {
  Gate g;
  auto d = compiled(R"({"defaults":{"mode":"observe","set":{"timeout-ms":50}}})");
  auto quick = ask(g, *d, Req{});
  quick.ticket->done(200, 40'000, kT0);
  auto slow = ask(g, *d, Req{});
  slow.ticket->done(500, 60'000, kT0);
  auto row = row_of(g, "defaults");
  EXPECT_EQ(row.would, 1);
  EXPECT_EQ(row.n5, 1);
  EXPECT_EQ(row.allowed, 2);
}

TEST(ControlsGate, EnforcedTimeoutIsHandedToTheCall) {
  Gate g;
  auto d = compiled(R"({"apis":{"box:shop":{"mode":"enforce","set":{"timeout-ms":250}}}})");
  EXPECT_EQ(ask(g, *d, Req{.api = "box:shop"}).timeout_ms, 250);
  EXPECT_EQ(ask(g, *d, Req{.api = "content"}).timeout_ms, 0);
}

TEST(ControlsGate, BucketsBecomeRowsAsTheyClose) {
  Gate g;
  auto d = compiled(R"({"defaults":{"mode":"enforce"}})");
  ask(g, *d, Req{.now = kT0});
  ask(g, *d, Req{.now = kT0 + 1000});
  EXPECT_TRUE(g.take_closed(kT0 + 2000).empty());  // still open
  EXPECT_EQ(g.pending_rows("example.com").size(), 1u);
  ask(g, *d, Req{.now = kT0 + kBucketMs});
  auto rows = g.take_closed(kT0 + kBucketMs + 1);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].t, kT0);
  EXPECT_EQ(rows[0].matched, 2);
  EXPECT_EQ(g.take_all().at(0).matched, 1);
}

TEST(ControlsGate, SketchesCountDistinctClients) {
  std::string s;
  for (int i = 0; i < 40; ++i) s = sketch_add(s, "client-" + std::to_string(i));
  int64_t n = sketch_count(s);
  EXPECT_GE(n, 34);
  EXPECT_LE(n, 46);
  EXPECT_EQ(sketch_count(""), 0);
  EXPECT_EQ(sketch_count(sketch_add("", "x")), 1);
  EXPECT_EQ(operations::sketch_union(sketch_add("", "x"), sketch_add("", "x")), sketch_add("", "x"));
}
