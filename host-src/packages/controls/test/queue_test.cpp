// MCP's strict rate and fixed queue (design/machine/047.Controls.md § 14): calls counted per
// JSON-RPC call, the rate per agent, first in first out per agent, round-robin across agents and
// queues, the running caps, the wait limit, a full queue, a client gone while waiting, slots
// released on every path, the rows, the health ratios, and the report.

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <thread>

#include "goober/controls/gate.h"
#include "goober/controls/queue.h"

using namespace goober;
using namespace goober::controls;

namespace {

constexpr int64_t kT0 = 1'800'000'000'000;  // a five-minute bucket's start

struct Harness {
  int64_t now = kT0;
  std::shared_ptr<McpQueue> q;
  std::vector<std::string> ran;             // labels, in the order they started
  std::vector<std::shared_ptr<Slot>> held;  // running calls' slots, by start order
  std::vector<std::pair<std::string, Refusal>> refused;
  explicit Harness(int pool) : q(McpQueue::make(pool, [this] { return now; })) {}

  Admission send(const std::string& label, const std::string& agent, QueueKind kind = QueueKind::kMcp,
                 const std::string& scope = "example.com", RpcCalls calls = full(1), std::function<bool()> gone = {},
                 int64_t bytes = 100, const std::string& cls = "agent") {
    Call c;
    c.kind = kind;
    c.scope = kind == QueueKind::kOperator ? "" : scope;
    c.tenant = kind == QueueKind::kOperator ? "" : "example";
    c.agent = agent;
    c.cls = cls;
    c.calls = calls;
    c.bytes = bytes;
    c.gone = std::move(gone);
    c.run = [this, label](std::shared_ptr<Slot> s) {
      ran.push_back(label);
      held.push_back(std::move(s));
    };
    c.refuse = [this, label](Refusal r) { refused.emplace_back(label, r); };
    return q->admit(std::move(c));
  }
  // Releases the slot of the call that started as `label`.
  void finish(const std::string& label) {
    for (size_t i = 0; i < ran.size(); ++i) {
      if (ran[i] == label && held[i]) {
        auto s = std::move(held[i]);
        s->release();
        return;
      }
    }
    ADD_FAILURE() << label << " isn't running";
  }
  static RpcCalls full(int n) {
    RpcCalls c;
    c.full = n;
    return c;
  }
  static RpcCalls cheap(int n) {
    RpcCalls c;
    c.cheap = n;
    return c;
  }
};

}  // namespace

TEST(RpcCallsTest, CountsEachCallAndTellsFullFromCheap) {
  auto one = count_rpc_calls(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"x","arguments":{"method":"ping"}}})");
  EXPECT_EQ(one.full, 1);
  EXPECT_EQ(one.cheap, 0);
  EXPECT_FALSE(one.batch);
  EXPECT_EQ(one.id, "7");
  auto list = count_rpc_calls(R"( {"id":"a-1","method":"tools/list"})");
  EXPECT_EQ(list.full, 0);
  EXPECT_EQ(list.cheap, 1);
  EXPECT_EQ(list.id, "\"a-1\"");
  // A batch: each request counts, and a bare value in it is a cheap call (the server answers it).
  auto batch = count_rpc_calls(
      R"([{"id":1,"method":"tools/call","params":{"name":"a"}},{"method":"notifications/initialized"},)"
      R"({"id":2,"method":"resources/read","params":{"uri":"x"}},{"id":3,"method":"ping"},5,"x",[1]])");
  EXPECT_TRUE(batch.batch);
  EXPECT_EQ(batch.full, 2);
  EXPECT_EQ(batch.cheap, 5);
  EXPECT_EQ(batch.id, "");  // a batch has no one id to answer
  // A method nested in params is no method; a string with an escaped quote doesn't end early.
  auto nested = count_rpc_calls(R"({"id":1,"params":{"method":"tools/call","s":"a\"}"},"method":"initialize"})");
  EXPECT_EQ(nested.full, 0);
  EXPECT_EQ(nested.cheap, 1);
  // Nothing countable is one cheap call; an id that isn't plain JSON isn't echoed.
  EXPECT_EQ(count_rpc_calls("not json").calls(), 1);
  EXPECT_EQ(count_rpc_calls("").calls(), 1);
  EXPECT_EQ(count_rpc_calls("{\"id\":\"a\nb\",\"method\":\"tools/call\"}").id, "");
  EXPECT_EQ(count_rpc_calls(R"({"id":true,"method":"tools/call"})").id, "");
  EXPECT_DOUBLE_EQ(batch.cost(0.2), 2 + 5 * 0.2);
}

// 047 § 14: only an exact, escape-free cheap method is cheap. Every spelling a JSON parser reads
// as tools/call (an escaped slash, unicode escapes, an escaped key, a method named twice) and any
// method not on the cheap list counts full.
TEST(RpcCallsTest, EscapedMethodsAndKeysCountFull) {
  EXPECT_EQ(count_rpc_calls(R"({"id":1,"method":"tools\/call","params":{"name":"x"}})").full, 1);
  // Unicode escapes, built from a lone backslash so nothing on the way decodes them.
  const std::string u = std::string("\\") + "u";
  EXPECT_EQ(count_rpc_calls(R"({"id":1,"method":"tools)" + u + R"(002fcall"})").full, 1);
  EXPECT_EQ(count_rpc_calls(R"({"id":1,"method":")" + u + R"(0074ools/call"})").full, 1);
  EXPECT_EQ(count_rpc_calls(R"({"id":1,"meth)" + u + R"(006fd":"tools/call"})").full, 1);
  EXPECT_EQ(count_rpc_calls(R"({"id":1,"method":"p)" + u + R"(0069ng"})").full, 1);  // even a cheap one, escaped
  // Sanity: those really are a backslash and a u.
  EXPECT_EQ((R"("p)" + u + R"(0069ng")").substr(2, 2), std::string("\\") + "u");
  EXPECT_EQ(count_rpc_calls(R"({"id":1,"method":"tools/call","method":"ping"})").full, 1);
  EXPECT_EQ(count_rpc_calls(R"({"id":1,"method":"ping","method":"tools/call"})").full, 1);
  EXPECT_EQ(count_rpc_calls(R"({"id":1,"method":"something/else"})").full, 1);
  EXPECT_EQ(count_rpc_calls(R"({"id":1,"method":"Tools/List"})").full, 1);
  auto batch = count_rpc_calls(R"([{"id":1,"method":"tools\/call"},{"id":2,"method":"ping"},{"method":"notifications/initialized"}])");
  EXPECT_EQ(batch.full, 1);
  EXPECT_EQ(batch.cheap, 2);
  // The cheap ones, exactly spelled, stay cheap; an escape inside params doesn't matter.
  for (const char* m : {"initialize", "ping", "tools/list", "notifications/cancelled", "resources/list", "prompts/list"}) {
    auto c = count_rpc_calls(std::string(R"({"id":1,"params":{"s":"a\/b"},"method":")") + m + "\"}");
    EXPECT_EQ(c.full, 0) << m;
    EXPECT_EQ(c.cheap, 1) << m;
  }
}

TEST(McpQueueTest, SizesItsCapsFromThePool) {
  EXPECT_EQ(queue_running_for(1), 1);
  EXPECT_EQ(queue_running_for(4), 2);
  EXPECT_EQ(queue_running_for(16), 8);
  EXPECT_EQ(total_running_for(1), 1);
  EXPECT_EQ(total_running_for(2), 1);  // one worker always left to everything else
  EXPECT_EQ(total_running_for(4), 3);
  EXPECT_EQ(total_running_for(8), 6);
  EXPECT_EQ(total_running_for(16), 12);
}

TEST(McpQueueTest, AStrictRatePerAgentCountsEveryCall) {
  Harness h(64);
  int ran = 0;
  auto quick = [&](const std::string& agent, RpcCalls calls, QueueKind kind = QueueKind::kMcp) {
    Call c;
    c.kind = kind;
    c.scope = kind == QueueKind::kMcp ? "example.com" : "";
    c.agent = agent;
    c.cls = "agent";
    c.calls = calls;
    c.run = [&](std::shared_ptr<Slot>) { ++ran; };  // done at once: the slot is dropped
    return h.q->admit(std::move(c));
  };
  for (int i = 0; i < 30; ++i) EXPECT_EQ(quick("a", Harness::full(1)).outcome, Outcome::kRan) << i;
  auto refused = quick("a", Harness::full(1));
  EXPECT_EQ(refused.outcome, Outcome::kRate);
  EXPECT_EQ(refused.retry_after_s, 1);  // two a second refill
  EXPECT_EQ(ran, 30);
  // Another agent has its own bucket.
  EXPECT_EQ(quick("b", Harness::full(1)).outcome, Outcome::kRan);
  // A second refills two calls; a batch of three is three calls and waits for them.
  h.now += 1000;
  EXPECT_EQ(quick("a", Harness::full(3)).outcome, Outcome::kRate);
  EXPECT_EQ(quick("a", Harness::full(2)).outcome, Outcome::kRan);
  // Cheap calls cost a fifth: an agent's 30 tokens are 150 tools/list.
  for (int i = 0; i < 150; ++i) EXPECT_EQ(quick("c", Harness::cheap(1)).outcome, Outcome::kRan) << i;
  EXPECT_EQ(quick("c", Harness::cheap(1)).outcome, Outcome::kRate);
  // The operator MCP's rate is its own and wider: a burst of 120.
  for (int i = 0; i < 120; ++i) EXPECT_EQ(quick("a", Harness::full(1), QueueKind::kOperator).outcome, Outcome::kRan) << i;
  EXPECT_EQ(quick("a", Harness::full(1), QueueKind::kOperator).outcome, Outcome::kRate);
  auto rows = h.q->take_all();
  int64_t refused_rate = 0, offered = 0;
  for (const auto& r : rows) refused_rate += r.refused_rate, offered += r.offered;
  EXPECT_EQ(refused_rate, 1 + 3 + 1 + 1);
  EXPECT_EQ(offered, 30 + 1 + 1 + 3 + 2 + 151 + 121);
}

TEST(McpQueueTest, AnAgentsCallsPastItsRunningLimitWaitInOrder) {
  Harness h(64);
  for (int i = 1; i <= 5; ++i) h.send("a" + std::to_string(i), "a");
  EXPECT_EQ(h.ran, (std::vector<std::string>{"a1", "a2"}));  // N = 2 running
  EXPECT_EQ(h.q->stats().waiting, 3);
  h.finish("a2");
  EXPECT_EQ(h.ran.back(), "a3");
  h.finish("a1");
  h.finish("a3");
  EXPECT_EQ(h.ran, (std::vector<std::string>{"a1", "a2", "a3", "a4", "a5"}));
  EXPECT_EQ(h.q->stats().waiting, 0);
  EXPECT_EQ(h.q->stats().running, 2);
  EXPECT_TRUE(h.refused.empty());
}

TEST(McpQueueTest, WaitingCallsAreSharedRoundRobinAcrossAgents) {
  Harness h(2);  // K = 1, T = 1: one MCP call at a time
  for (int i = 1; i <= 5; ++i) h.send("a" + std::to_string(i), "a");
  for (int i = 1; i <= 2; ++i) h.send("b" + std::to_string(i), "b");
  for (int i = 1; i <= 2; ++i) h.send("c" + std::to_string(i), "c");
  ASSERT_EQ(h.ran, std::vector<std::string>{"a1"});
  // The flooding agent front-loaded five; the others still get every other turn.
  for (size_t i = 0; i < 8; ++i) h.finish(h.ran.back());
  EXPECT_EQ(h.ran, (std::vector<std::string>{"a1", "b1", "c1", "a2", "b2", "c2", "a3", "a4", "a5"}));
}

TEST(McpQueueTest, AQueueAndAllMcpHaveRunningCapsFromThePool) {
  Harness h(4);  // K = 2 per queue, T = 3 in all
  for (int i = 0; i < 4; ++i) h.send("x" + std::to_string(i), "agent" + std::to_string(i), QueueKind::kMcp, "one.example");
  EXPECT_EQ(h.q->stats(QueueKind::kMcp, "one.example").running, 2);
  EXPECT_EQ(h.q->stats(QueueKind::kMcp, "one.example").waiting, 2);
  for (int i = 0; i < 2; ++i) h.send("y" + std::to_string(i), "agent" + std::to_string(i), QueueKind::kMcp, "two.example");
  EXPECT_EQ(h.q->stats(QueueKind::kMcp, "two.example").running, 1);  // T reached
  h.send("op", "grok", QueueKind::kOperator);
  EXPECT_EQ(h.q->stats(QueueKind::kOperator, "").running, 0);
  EXPECT_EQ(h.q->stats().running, 3);
  // A slot freed on one domain goes to the next queue in turn, not back to the busiest one.
  h.finish("x0");
  EXPECT_EQ(h.q->stats().running, 3);
  EXPECT_EQ(h.ran.back(), "op");
}

TEST(McpQueueTest, PastItsWaitingLimitAnAgentIsRefusedAndOthersAreNot) {
  Harness h(64);
  for (int i = 0; i < 30; ++i) EXPECT_EQ(h.send("a" + std::to_string(i), "a").outcome, i < 2 ? Outcome::kRan : Outcome::kQueued);
  EXPECT_EQ(h.send("over", "a").outcome, Outcome::kRate);  // the rate's burst of 30 comes first
  h.now += 10'000;  // tokens again, well inside the wait
  for (int i = 30; i < 34; ++i) EXPECT_EQ(h.send("a" + std::to_string(i), "a").outcome, Outcome::kQueued);
  // With tokens to spare, the 35th call is refused for the queue being full (2 running, 32 waiting).
  auto full = h.send("over", "a");
  EXPECT_EQ(full.outcome, Outcome::kFull);
  EXPECT_EQ(full.retry_after_s, 1);
  EXPECT_EQ(h.send("b1", "b").outcome, Outcome::kRan);
  // The bodies waiting are bounded too: 16 MiB an agent on /mcp.
  EXPECT_EQ(h.send("big", "c", QueueKind::kMcp, "example.com", Harness::full(1), {}, 10).outcome, Outcome::kRan);
  EXPECT_EQ(h.send("big", "c", QueueKind::kMcp, "example.com", Harness::full(1), {}, 10).outcome, Outcome::kRan);
  EXPECT_EQ(h.send("big", "c", QueueKind::kMcp, "example.com", Harness::full(1), {}, 10ll << 20).outcome, Outcome::kQueued);
  EXPECT_EQ(h.send("big", "c", QueueKind::kMcp, "example.com", Harness::full(1), {}, 10ll << 20).outcome, Outcome::kFull);
}

TEST(McpQueueTest, ACallThatWaitsTooLongFailsAndIsCounted) {
  Harness h(2);
  h.send("run", "a");
  h.send("wait", "b");
  h.send("op1", "grok", QueueKind::kOperator);
  h.now += 30'000;
  h.q->sweep();
  EXPECT_TRUE(h.refused.empty());  // exactly 30 s is still in time
  h.now += 1;
  h.q->sweep();
  ASSERT_EQ(h.refused.size(), 1u);
  EXPECT_EQ(h.refused[0], std::make_pair(std::string("wait"), Refusal::kWaited));
  // The operator MCP waits up to 120 s.
  h.now += 90'000;
  h.q->sweep();
  ASSERT_EQ(h.refused.size(), 2u);
  EXPECT_EQ(h.refused[1].first, "op1");
  h.finish("run");
  EXPECT_EQ(h.ran, std::vector<std::string>{"run"});  // what left never runs
  int64_t waited = 0;
  for (const auto& r : h.q->take_all()) waited += r.refused_wait;
  EXPECT_EQ(waited, 2);
}

TEST(McpQueueTest, ACallWhoseClientLeftIsRemovedNotRun) {
  Harness h(2);
  std::atomic<bool> gone1{false}, gone2{false};
  h.send("run", "a");
  h.send("w1", "b", QueueKind::kMcp, "example.com", Harness::full(1), [&] { return gone1.load(); });
  h.send("w2", "c", QueueKind::kMcp, "example.com", Harness::full(1), [&] { return gone2.load(); });
  h.send("w3", "d");
  // Gone while waiting: the sweep takes it out.
  gone1 = true;
  h.q->sweep();
  ASSERT_EQ(h.refused.size(), 1u);
  EXPECT_EQ(h.refused[0].second, Refusal::kGone);
  // Gone when its turn comes: skipped, and the next one runs.
  gone2 = true;
  h.finish("run");
  EXPECT_EQ(h.ran, (std::vector<std::string>{"run", "w3"}));
  EXPECT_EQ(h.refused.size(), 2u);
  int64_t gone = 0;
  for (const auto& r : h.q->take_all()) gone += r.gone;
  EXPECT_EQ(gone, 2);
}

TEST(McpQueueTest, EveryWayOutOfACallReleasesItsSlot) {
  Harness h(2);
  // Dropped without release(): a finish path that never ran (a handler that threw).
  {
    Call c;
    c.scope = "example.com";
    c.agent = "a";
    c.cls = "agent";
    c.calls = Harness::full(1);
    c.run = [](std::shared_ptr<Slot> s) {
      try {
        auto held = std::move(s);
        throw std::runtime_error("the tool failed");
      } catch (const std::exception&) {
      }
    };
    EXPECT_EQ(h.q->admit(std::move(c)).outcome, Outcome::kRan);
  }
  EXPECT_EQ(h.q->stats().running, 0);
  // Released twice: counted once.
  h.send("a", "a");
  h.send("b", "b");
  auto slot = h.held[0];
  slot->release();
  slot->release();
  EXPECT_EQ(h.q->stats().running, 1);
  EXPECT_EQ(h.ran.back(), "b");
  h.held.clear();
  slot.reset();
  EXPECT_EQ(h.q->stats().running, 0);
  EXPECT_EQ(h.q->stats().agents, 0u);  // nothing kept for an idle agent but its rate bucket
  // A slot outliving its queue is harmless.
  auto q2 = McpQueue::make(2);
  std::shared_ptr<Slot> kept;
  Call c;
  c.agent = "a";
  c.cls = "agent";
  c.run = [&](std::shared_ptr<Slot> s) { kept = std::move(s); };
  q2->admit(std::move(c));
  q2.reset();
  kept.reset();
}

TEST(McpQueueTest, AWaitingCallHoldsOnlyItsContinuation) {
  // Run is called only when a slot is free: what waits is a parked callback, never a thread.
  Harness h(2);
  for (int i = 0; i < 10; ++i) h.send("c" + std::to_string(i), "agent" + std::to_string(i % 5));
  EXPECT_EQ(h.ran.size(), 1u);
  EXPECT_EQ(h.q->stats().waiting, 9);
  EXPECT_EQ(h.q->stats().waiting_bytes, 900);
}

TEST(McpQueueTest, StopRefusesWhatWaitsAndEverythingAfter) {
  Harness h(2);
  h.send("run", "a");
  h.send("wait", "b");
  h.q->stop();
  ASSERT_EQ(h.refused.size(), 1u);
  EXPECT_EQ(h.refused[0].second, Refusal::kStopping);
  EXPECT_EQ(h.send("late", "c").outcome, Outcome::kFull);
  h.finish("run");
  EXPECT_EQ(h.ran, std::vector<std::string>{"run"});
}

TEST(McpQueueTest, RowsCountPerQueueAndClassInFiveMinuteBuckets) {
  Harness h(2);
  h.send("p1", "person", QueueKind::kMcp, "example.com", Harness::full(1), {}, 1, "person");
  h.send("a1", "a");
  h.send("a2", "a");
  h.now += 1500;
  h.finish("p1");  // a1 waited 1.5 s
  h.send("op", "grok", QueueKind::kOperator, "", Harness::full(1), {}, 1, "operator");
  EXPECT_TRUE(h.q->take_closed().empty());  // the bucket is still open
  h.now = kT0 + kBucketMs;
  auto rows = h.q->take_closed();
  ASSERT_EQ(rows.size(), 3u);
  std::map<std::string, operations::McpQueueRow> by;
  for (auto& r : rows) by[r.queue + "/" + r.cls] = r;
  const auto& agent = by.at("mcp/agent");
  EXPECT_EQ(agent.t, kT0);
  EXPECT_EQ(agent.domain, "example.com");
  EXPECT_EQ(agent.tenant, "example");
  EXPECT_EQ(agent.offered, 2);
  EXPECT_EQ(agent.queued, 2);
  EXPECT_EQ(agent.ran, 1);
  EXPECT_EQ(agent.peak_waiting, 2);
  EXPECT_EQ(agent.waited.total(), 1);
  EXPECT_GE(agent.waited.percentile(0.5), 1'500'000);
  EXPECT_EQ(by.at("mcp/person").ran, 1);
  EXPECT_EQ(by.at("mcp/person").peak_running, 1);
  EXPECT_EQ(by.at("operator/operator").queued, 1);
  EXPECT_EQ(by.at("operator/operator").domain, "");
  EXPECT_TRUE(h.q->take_closed().empty());
}

TEST(McpQueueTest, HealthIsRatiosOverTheLastFiveMinutes) {
  Harness h(2);
  h.send("a1", "a");
  h.send("a2", "a");  // waits
  auto health = h.q->health();
  EXPECT_DOUBLE_EQ(health.at("waited_ratio").as_double(), 0.5);
  EXPECT_DOUBLE_EQ(health.at("refused_ratio").as_double(), 0.0);
  EXPECT_DOUBLE_EQ(health.at("busy_ratio").as_double(), 1.0);
  EXPECT_FALSE(health.contains("offered"));  // no counts: /goober/ready is public
  h.now += 6 * 60'000;
  health = h.q->health();
  EXPECT_DOUBLE_EQ(health.at("waited_ratio").as_double(), 0.0);
}

TEST(McpQueueTest, TheReportSaysWhatTheQueueDid) {
  operations::McpQueueRow r;
  r.t = kT0;
  r.queue = "mcp";
  r.domain = "example.com";
  r.cls = "agent";
  r.offered = 100;
  r.queued = 40;
  r.ran = 80;
  r.refused_rate = 10;
  r.refused_full = 6;
  r.refused_wait = 4;
  r.peak_waiting = 32;
  r.waited.add(2'000'000);
  auto rep = queue_report(QueueKind::kMcp, "example.com", {r}, "1h", kT0 + 60'000, 4);
  EXPECT_EQ(rep.at("totals").at("refused").as_int64(), 20);
  EXPECT_EQ(rep.at("by_class").at("agent").at("ran").as_int64(), 80);
  EXPECT_EQ(rep.at("limits").at("queue_running").as_int64(), 2);
  EXPECT_EQ(rep.at("points").as_array().size(), 12u);
  std::string verdicts = json::dump(rep.at("verdicts"));
  EXPECT_NE(verdicts.find("at the strict rate"), std::string::npos) << verdicts;
  EXPECT_NE(verdicts.find("with the queue full"), std::string::npos);
  EXPECT_NE(verdicts.find("waited past 30 s"), std::string::npos);
  auto empty = queue_report(QueueKind::kOperator, "", {}, "12h", kT0, 4);
  EXPECT_EQ(json::dump(empty.at("verdicts")), R"(["no MCP calls in 12h"])");
  EXPECT_FALSE(empty.contains("domain"));
}

// A misbehaving agent floods from many threads; a well-behaved one still starts promptly.
TEST(McpQueueTest, AFloodingAgentDoesNotStarveAPoliteOne) {
  auto q = McpQueue::make(4);  // K = 2, T = 3
  // A small pool of workers: run posts here, as the host posts to its pool.
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool stopping = false;
  std::vector<std::thread> workers;
  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([&] {
      while (true) {
        std::function<void()> job;
        {
          std::unique_lock lock(mu);
          cv.wait(lock, [&] { return stopping || !jobs.empty(); });
          if (jobs.empty()) return;
          job = std::move(jobs.front());
          jobs.pop_front();
        }
        job();
      }
    });
  }
  auto post = [&](std::function<void()> f) {
    {
      std::lock_guard lock(mu);
      jobs.push_back(std::move(f));
    }
    cv.notify_one();
  };
  std::atomic<int> flood_ran{0}, flood_refused{0};
  auto send = [&](const std::string& agent, std::function<void()> done) {
    Call c;
    c.scope = "example.com";
    c.agent = agent;
    c.cls = "agent";
    c.calls.full = 1;
    c.run = [&, done](std::shared_ptr<Slot> s) {
      post([s, done]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // a slow tool
        done();
        s->release();
      });
    };
    c.refuse = [](Refusal) {};
    return q->admit(std::move(c)).outcome;
  };
  std::vector<std::thread> flooders;
  for (int t = 0; t < 8; ++t) {
    flooders.emplace_back([&] {
      for (int i = 0; i < 20; ++i) {
        auto o = send("flood", [&] { ++flood_ran; });
        if (o == Outcome::kRate || o == Outcome::kFull) ++flood_refused;
      }
    });
  }
  for (auto& f : flooders) f.join();
  // The flood: 30 admitted by the rate, the rest refused; 2 running and 28 waiting behind.
  EXPECT_EQ(flood_refused.load(), 160 - 30);
  std::atomic<bool> polite_done{false};
  auto started = std::chrono::steady_clock::now();
  // The flood holds its queue's two slots; the polite call is next in turn, ahead of the 28.
  EXPECT_EQ(send("polite", [&] { polite_done = true; }), Outcome::kQueued);
  while (!polite_done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto took = std::chrono::steady_clock::now() - started;
  EXPECT_LT(took, std::chrono::milliseconds(200));
  // And a second polite call, made while the flood still waits, starts on the next free slot.
  std::atomic<bool> second_done{false};
  send("polite", [&] { second_done = true; });
  started = std::chrono::steady_clock::now();
  while (!second_done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(200));
  while (q->stats().running > 0 || q->stats().waiting > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(flood_ran.load(), 30);
  {
    std::lock_guard lock(mu);
    stopping = true;
  }
  cv.notify_all();
  for (auto& w : workers) w.join();
}

// The operator MCP's queue holds a whole burst: a push the rate admits is never refused as full.
TEST(McpQueueTest, TheOperatorQueueHoldsItsWholeBurst) {
  Harness h(64);
  for (int i = 0; i < 120; ++i) {
    EXPECT_EQ(h.send("p" + std::to_string(i), "grok", QueueKind::kOperator).outcome, i < 2 ? Outcome::kRan : Outcome::kQueued) << i;
  }
  h.now += 2'000;  // 20 tokens more, well inside the 120 s wait
  for (int i = 120; i < 130; ++i) EXPECT_EQ(h.send("p" + std::to_string(i), "grok", QueueKind::kOperator).outcome, Outcome::kQueued);
  EXPECT_EQ(h.send("over", "grok", QueueKind::kOperator).outcome, Outcome::kFull);  // 2 running, 128 waiting
}
