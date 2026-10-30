#include <gtest/gtest.h>

#include "goober/runtime_wasm/engine.h"

using namespace goober::runtime::wasm;

namespace {

Engine& engine() {
  static Engine e;
  return e;
}

// Echoes the request document back as the body, with a header and a log line.
const char* kEcho = R"((module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_header_set" (func $header (param i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_log" (func $log (param i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "x-box")
  (data (i32.const 21) "yes")
  (data (i32.const 24) "set-cookie")
  (data (i32.const 34) "echoing")
  (func (export "goober_route_echo") (result i32)
    (local $n i32)
    (local.set $n (call $read (i32.const 1024) (call $size)))
    (drop (call $header (i32.const 16) (i32.const 5) (i32.const 21) (i32.const 3)))
    (if (i32.ne (call $header (i32.const 24) (i32.const 10) (i32.const 21) (i32.const 3)) (i32.const -1))
      (then (unreachable)))
    (drop (call $log (i32.const 1) (i32.const 34) (i32.const 7)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 1024) (local.get $n)))
    (i32.const 0))
  (func (export "goober_route_spin") (result i32)
    (loop $l (br $l))
    (i32.const 0))
  (func (export "goober_route_silent") (result i32) (i32.const 0))
  (func (export "goober_route_fails") (result i32) (i32.const 7))
  (func (export "goober_route_bad_pointer") (result i32)
    (if (i32.ne (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 65530) (i32.const 100)) (i32.const -1))
      (then (unreachable)))
    (i32.const 0))
  (func (export "goober_route_grow") (result i32)
    ;; Ask for 32 MiB more; with a 1 MiB limit memory.grow must return -1.
    (if (i32.ne (memory.grow (i32.const 512)) (i32.const -1)) (then (unreachable)))
    (drop (call $respond (i32.const 204) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0)))
    (i32.const 0))
))";

// Writes the time and 8 random bytes into the body.
const char* kDeterminism = R"((module
  (import "goober" "goober_now_ms" (func $now (result i64)))
  (import "goober" "goober_random_fill" (func $rand (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (func (export "goober_route_go") (result i32)
    (i64.store (i32.const 0) (call $now))
    (drop (call $rand (i32.const 8) (i32.const 8)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 16)))
    (i32.const 0))
))";

std::shared_ptr<const CompiledBox> compile(const char* wat, std::vector<std::string> exports) {
  std::string err;
  auto box = engine().compile(wat, exports, &err);
  EXPECT_TRUE(box) << err;
  return box;
}

}  // namespace

TEST(Engine, EchoRoundTrip) {
  auto box = compile(kEcho, {"goober_route_echo"});
  CallInput in{R"({"hello":"world"})", 1, 2};
  auto r = engine().call(*box, "goober_route_echo", in, {});
  ASSERT_EQ(r.outcome, CallResult::Outcome::kOk) << r.error;
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.content_type, "application/json");
  EXPECT_EQ(r.body, in.request_json);
  ASSERT_EQ(r.headers.size(), 1u);
  EXPECT_EQ(r.headers[0].first, "x-box");
  ASSERT_EQ(r.logs.size(), 1u);
  EXPECT_EQ(r.logs[0].text, "echoing");
  EXPECT_GT(r.fuel_used, 0u);
}

TEST(Engine, EpochTimeoutStopsAnInfiniteLoop) {
  auto box = compile(kEcho, {"goober_route_spin"});
  CallLimits limits;
  limits.timeout_ms = 20;
  limits.fuel = 1'000'000'000'000;
  auto r = engine().call(*box, "goober_route_spin", {}, limits);
  EXPECT_EQ(r.outcome, CallResult::Outcome::kTimeout) << r.error;
}

TEST(Engine, FuelStopsAnInfiniteLoop) {
  auto box = compile(kEcho, {"goober_route_spin"});
  CallLimits limits;
  limits.timeout_ms = 10'000;
  limits.fuel = 10'000;
  auto r = engine().call(*box, "goober_route_spin", {}, limits);
  EXPECT_EQ(r.outcome, CallResult::Outcome::kOutOfFuel) << r.error;
}

TEST(Engine, ReturnCodesAndMissingResponses) {
  auto box = compile(kEcho, {});
  EXPECT_EQ(engine().call(*box, "goober_route_silent", {}, {}).outcome, CallResult::Outcome::kNoResponse);
  EXPECT_EQ(engine().call(*box, "goober_route_fails", {}, {}).outcome, CallResult::Outcome::kFailed);
  EXPECT_EQ(engine().call(*box, "goober_route_nope", {}, {}).outcome, CallResult::Outcome::kHostError);
}

TEST(Engine, BadPointersAreRefusedNotTrusted) {
  auto box = compile(kEcho, {});
  auto r = engine().call(*box, "goober_route_bad_pointer", {}, {});
  EXPECT_EQ(r.outcome, CallResult::Outcome::kNoResponse) << r.error;
}

TEST(Engine, MemoryLimitHoldsGrowth) {
  auto box = compile(kEcho, {});
  CallLimits limits;
  limits.memory_max_bytes = 1 << 20;
  auto r = engine().call(*box, "goober_route_grow", {}, limits);
  EXPECT_EQ(r.outcome, CallResult::Outcome::kOk) << r.error;
  EXPECT_EQ(r.status, 204);
}

TEST(Engine, TimeAndRandomnessAreHostControlled) {
  auto box = compile(kDeterminism, {"goober_route_go"});
  CallInput in{"{}", 1234567, 99};
  auto a = engine().call(*box, "goober_route_go", in, {});
  auto b = engine().call(*box, "goober_route_go", in, {});
  ASSERT_EQ(a.outcome, CallResult::Outcome::kOk) << a.error;
  EXPECT_EQ(a.body, b.body);
  int64_t now = 0;
  memcpy(&now, a.body.data(), 8);
  EXPECT_EQ(now, 1234567);
  in.seed = 100;
  EXPECT_NE(engine().call(*box, "goober_route_go", in, {}).body, a.body);
}

TEST(Engine, RefusesForeignImports) {
  std::string err;
  auto box = engine().compile(R"((module
    (import "wasi_snapshot_preview1" "fd_write" (func (param i32 i32 i32 i32) (result i32)))
    (memory (export "memory") 1)))",
                              {}, &err);
  EXPECT_FALSE(box);
  EXPECT_NE(err.find("wasi_snapshot_preview1"), std::string::npos);
}

TEST(Engine, RefusesWrongImportTypes) {
  std::string err;
  auto box = engine().compile(R"((module
    (import "goober" "goober_now_ms" (func (result i32)))
    (memory (export "memory") 1)))",
                              {}, &err);
  EXPECT_FALSE(box);
}

TEST(Engine, RequiresMemoryAndRouteExports) {
  std::string err;
  EXPECT_FALSE(engine().compile("(module)", {}, &err));
  EXPECT_NE(err.find("memory"), std::string::npos);
  err.clear();
  EXPECT_FALSE(engine().compile(R"((module (memory (export "memory") 1)))", {"goober_route_x"}, &err));
  EXPECT_NE(err.find("goober_route_x"), std::string::npos);
  err.clear();
  EXPECT_FALSE(engine().compile(R"((module (memory (export "memory") 1)
    (func (export "goober_route_x") (param i32) (result i32) (i32.const 0))))",
                                {"goober_route_x"}, &err));
  EXPECT_NE(err.find("i32"), std::string::npos);
}

TEST(Engine, WatWithLeadingCommentCompiles) {
  std::string err;
  EXPECT_TRUE(engine().compile(";; a comment\n(module (memory (export \"memory\") 1))", {}, &err)) << err;
}

TEST(Engine, BinaryWasmCompiles) {
  std::string wasm, err;
  ASSERT_TRUE(Engine::wat_to_wasm(kEcho, &wasm, &err)) << err;
  EXPECT_EQ(wasm.substr(0, 4), std::string("\0asm", 4));
  EXPECT_TRUE(engine().compile(wasm, {"goober_route_echo"}, &err)) << err;
}

TEST(Engine, SqlGoesThroughTheHostAndBack) {
  const char* wat = R"((module
    (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
    (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
    (memory (export "memory") 1)
    (data (i32.const 0) "application/json")
    (data (i32.const 16) "book")
    (data (i32.const 20) "SELECT 1")
    (data (i32.const 28) "[]")
    (func (export "goober_route_q") (result i32)
      (local $n i32)
      (local.set $n (call $sql (i32.const 16) (i32.const 4) (i32.const 20) (i32.const 8) (i32.const 28) (i32.const 2)))
      (drop (call $result (i32.const 1024) (local.get $n)))
      (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 1024) (local.get $n)))
      (i32.const 0))
    (func (export "goober_route_bad") (result i32)
      (if (i32.ne (call $sql (i32.const 65530) (i32.const 100) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0))
                  (i32.const -1))
        (then (unreachable)))
      (drop (call $respond (i32.const 204) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0)))
      (i32.const 0))
  ))";
  auto box = compile(wat, {"goober_route_q", "goober_route_bad"});
  CallInput in{"{}", 1, 2};
  in.sql = [](std::string_view strategy, std::string_view sql, std::string_view params) {
    return std::string(strategy) + "|" + std::string(sql) + "|" + std::string(params);
  };
  auto r = engine().call(*box, "goober_route_q", in, {});
  ASSERT_EQ(r.outcome, CallResult::Outcome::kOk) << r.error;
  EXPECT_EQ(r.body, "book|SELECT 1|[]");
  EXPECT_EQ(engine().call(*box, "goober_route_bad", in, {}).status, 204);
  CallInput none{"{}", 1, 2};
  auto n = engine().call(*box, "goober_route_q", none, {});
  EXPECT_NE(n.body.find("no databases"), std::string::npos);
}

TEST(Engine, TablesInstancesAndStatusesAreBounded) {
  // A table grown past 10,000 elements, or an interim 1xx status, is refused without harm.
  const char* wat = R"((module
    (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
    (memory (export "memory") 1)
    (table 1 funcref)
    (data (i32.const 0) "text/plain")
    (func (export "goober_route_grow_table") (result i32)
      (if (i32.ne (table.grow (ref.null func) (i32.const 100000)) (i32.const -1)) (then (unreachable)))
      (if (i32.ne (call $respond (i32.const 101) (i32.const 0) (i32.const 10) (i32.const 0) (i32.const 0)) (i32.const -1))
        (then (unreachable)))
      (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 10) (i32.const 0) (i32.const 0)))
      (i32.const 0))
  ))";
  auto box = compile(wat, {"goober_route_grow_table"});
  auto r = engine().call(*box, "goober_route_grow_table", {}, {});
  EXPECT_EQ(r.outcome, CallResult::Outcome::kOk) << r.error;
  EXPECT_EQ(r.status, 200);
  std::string err;
  EXPECT_FALSE(engine().compile(R"((module (memory (export "memory") 1) (table 200000 funcref)))", {}, &err));
}

// What a box hands the host is bounded before it's copied: a long header value, a long content
// type, and headers that reach past the box's own answer (refresh, clear-site-data, HSTS).
TEST(Engine, HeadersAreBoundedAndTheFarReachingOnesAreTheHosts) {
  const char* wat = R"((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_header_set" (func $header (param i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "x-big")
  (data (i32.const 16) "refresh")
  (data (i32.const 32) "clear-site-data")
  (data (i32.const 48) "strict-transport-security")
  (data (i32.const 80) "text/plain")
  (func (export "goober_route_go") (result i32)
    ;; 9000 bytes of value: over the 8 KiB a header may be.
    (if (i32.ne (call $header (i32.const 0) (i32.const 5) (i32.const 1024) (i32.const 9000)) (i32.const -1)) (then (unreachable)))
    (if (i32.ne (call $header (i32.const 16) (i32.const 7) (i32.const 80) (i32.const 4)) (i32.const -1)) (then (unreachable)))
    (if (i32.ne (call $header (i32.const 32) (i32.const 15) (i32.const 80) (i32.const 4)) (i32.const -1)) (then (unreachable)))
    (if (i32.ne (call $header (i32.const 48) (i32.const 25) (i32.const 80) (i32.const 4)) (i32.const -1)) (then (unreachable)))
    ;; A content type of 300 bytes is refused; a short one answers.
    (if (i32.ne (call $respond (i32.const 200) (i32.const 1024) (i32.const 300) (i32.const 0) (i32.const 0)) (i32.const -1)) (then (unreachable)))
    (drop (call $respond (i32.const 200) (i32.const 80) (i32.const 10) (i32.const 0) (i32.const 0)))
    (i32.const 0))
))";
  auto box = compile(wat, {"goober_route_go"});
  CallInput in{"{}", 1, 2};
  auto r = engine().call(*box, "goober_route_go", in, {});
  ASSERT_EQ(r.outcome, CallResult::Outcome::kOk) << r.error;
  EXPECT_TRUE(r.headers.empty());
  EXPECT_EQ(r.content_type, "text/plain");
}

TEST(Engine, AKeyedSeedIsUnpredictableFromTheSeedAlone) {
  auto box = compile(kDeterminism, {"goober_route_go"});
  CallInput plain{"{}", 5, 42};
  CallInput keyed{"{}", 5, 42};
  keyed.random_key = std::array<uint8_t, 32>{1, 2, 3};
  CallInput other_key{"{}", 5, 42};
  other_key.random_key = std::array<uint8_t, 32>{9};
  auto a = engine().call(*box, "goober_route_go", plain, {});
  auto b = engine().call(*box, "goober_route_go", keyed, {});
  auto b2 = engine().call(*box, "goober_route_go", keyed, {});
  auto c = engine().call(*box, "goober_route_go", other_key, {});
  ASSERT_EQ(b.outcome, CallResult::Outcome::kOk) << b.error;
  EXPECT_EQ(b.body, b2.body);              // the same key and seed replay
  EXPECT_NE(a.body.substr(8), b.body.substr(8));  // the seed alone doesn't give the bytes
  EXPECT_NE(b.body.substr(8), c.body.substr(8));  // nor does another key
}
