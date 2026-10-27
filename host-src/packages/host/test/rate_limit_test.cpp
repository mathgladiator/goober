// Rate limits per client address (PROGRESS.md Phase 3).

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/host/host.h"
#include "goober/host/rate_limit.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace fs = std::filesystem;

TEST(RateLimiter, BurstThenSteadyRate) {
  host::RateLimiter l;
  host::RateRule rule{60, 3};  // one a second, three at once
  int64_t t = 1'000'000;
  EXPECT_EQ(l.take("b", "k", rule, t), 0);
  EXPECT_EQ(l.take("b", "k", rule, t), 0);
  EXPECT_EQ(l.take("b", "k", rule, t), 0);
  EXPECT_EQ(l.take("b", "k", rule, t), 1);      // wait a second
  EXPECT_EQ(l.take("b", "other", rule, t), 0);  // keys are apart
  EXPECT_EQ(l.take("c", "k", rule, t), 0);      // and buckets
  EXPECT_EQ(l.take("b", "k", rule, t + 1000), 0);
  EXPECT_EQ(l.take("b", "k", rule, t + 1000), 1);
  EXPECT_EQ(l.take("b", "k", rule, t + 60'000), 0);  // refilled, capped at the burst
}

namespace {
int post(uint16_t port, const std::string& target, const std::string& body,
         std::vector<std::pair<std::string, std::string>> headers = {}, std::string* retry = nullptr) {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(bhttp::verb::post, target, 11);
  req.set(bhttp::field::host, "localhost");
  req.set(bhttp::field::content_type, "application/x-www-form-urlencoded");
  for (const auto& [k, v] : headers) req.set(k, v);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  if (retry) *retry = std::string(res[bhttp::field::retry_after]);
  return int(res.result_int());
}
}  // namespace

TEST(RateLimits, SignInGuessingIsSlowedPerAddress) {
  auto root = fs::path(GOOBER_TEST_TMP) / "rate_limits";
  fs::remove_all(root);
  std::string err;
  auto cfg = config::parse_config(R"({"listen": {"port": 0, "behind-proxy": true}, "storage": "storage",
      "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1}})",
                                  root, &err);
  ASSERT_TRUE(cfg) << err;
  host::Host h(config::local_mode(cfg));
  uint16_t port = h.start();
  int limited = 0;
  std::string retry;
  for (int i = 0; i < 15; ++i) {
    if (post(port, "/identity/email/verify", "email=a%40b.c&code=000000", {{"x-forwarded-for", "203.0.113.7"}}, &retry) == 429) {
      ++limited;
    }
  }
  EXPECT_EQ(limited, 5);  // a burst of 10, then refused
  EXPECT_FALSE(retry.empty());
  // Another client, as the proxy reports it, still gets in; a forged earlier hop changes nothing.
  EXPECT_NE(post(port, "/identity/email/verify", "email=a%40b.c&code=000000", {{"x-forwarded-for", "198.51.100.1"}}), 429);
  EXPECT_EQ(post(port, "/identity/email/verify", "email=a%40b.c&code=000000",
                 {{"x-forwarded-for", "198.51.100.9, 203.0.113.7"}}),
            429);
}
