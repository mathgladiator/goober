// Controls as operators write them (design/machine/047.Controls.md § 2 to § 4): parsing, the
// matcher and its specificity, the per-property merge, and validation errors.

#include <gtest/gtest.h>

#include "goober/controls/spec.h"

using namespace goober;
using namespace goober::controls;

namespace {

const HostLimits kLimits{8 << 20, 30000};

bj::object obj(std::string_view text) {
  std::string err;
  auto v = json::parse(text, &err);
  EXPECT_TRUE(v && v->is_object()) << err;
  return v->as_object();
}

std::string props_error(std::string_view set) {
  std::string err;
  auto p = parse_props(obj(set), kLimits, "set", &err);
  EXPECT_FALSE(p) << set;
  return err;
}

std::vector<std::string> path(std::initializer_list<const char*> segs) { return {segs.begin(), segs.end()}; }

DomainControls compiled(std::string_view file, std::optional<Control> host = std::nullopt) {
  std::string err;
  auto d = compile(obj(file), "example.com", "example", host, kLimits, &err);
  EXPECT_TRUE(d) << err;
  return d ? std::move(*d) : DomainControls{};
}

}  // namespace

TEST(ControlsSpec, PropertiesParseWithDefaultsAndOff) {
  std::string err;
  auto p = parse_props(obj(R"({"rate":{"per-minute":60},"concurrent":{"max":4},"body-max-bytes":1024,
                              "timeout-ms":"off","closed":true})"),
                       kLimits, "set", &err);
  ASSERT_TRUE(p) << err;
  EXPECT_TRUE(p->rate.on());
  EXPECT_EQ(p->rate.value.per_minute, 60);
  EXPECT_EQ(p->rate.value.burst, 15);  // a quarter minute's worth
  EXPECT_EQ(p->rate.value.by, By::kClient);
  EXPECT_EQ(p->concurrent.value.max, 4);
  EXPECT_EQ(p->body_max_bytes.value, 1024);
  EXPECT_TRUE(p->timeout_ms.named);
  EXPECT_TRUE(p->timeout_ms.off);
  EXPECT_EQ(p->closed.value.status, 503);
  auto back = props_json(*p);
  EXPECT_EQ(back.at("timeout-ms"), "off");
  EXPECT_EQ(back.at("rate").at("burst"), 15);
  EXPECT_EQ(back.at("rate").at("by"), "client");
}

TEST(ControlsSpec, ValidationNamesTheKey) {
  EXPECT_NE(props_error(R"({"ratee":{}})").find("unknown key ratee"), std::string::npos);
  EXPECT_NE(props_error(R"({"rate":{"per-minute":60,"burts":3}})").find("set.rate: unknown key burts"), std::string::npos);
  EXPECT_NE(props_error(R"({"rate":{}})").find("per-minute is required"), std::string::npos);
  EXPECT_NE(props_error(R"({"rate":{"per-minute":0}})").find("0.1 to 1000000"), std::string::npos);
  EXPECT_NE(props_error(R"({"rate":{"per-minute":5,"by":"ip"}})").find("client, user, or all"), std::string::npos);
  EXPECT_NE(props_error(R"({"concurrent":{}})").find("max, per-client, or both"), std::string::npos);
  EXPECT_NE(props_error(R"({"concurrent":{"max":2,"per-client":3}})").find("can't be more than max"), std::string::npos);
  EXPECT_NE(props_error(R"({"body-max-bytes":9999999999})").find("may only lower it"), std::string::npos);
  EXPECT_NE(props_error(R"({"timeout-ms":30001})").find("limits.timeout-ms.max"), std::string::npos);
  EXPECT_NE(props_error(R"({"closed":{"status":404}})").find("503, 410, or 451"), std::string::npos);
  EXPECT_NE(props_error(R"({"closed":false})").find("not false"), std::string::npos);
  EXPECT_NE(props_error(R"({"closed":{"message":"two\nlines"}})").find("one line"), std::string::npos);
}

TEST(ControlsSpec, MatchesParseOrSayWhy) {
  std::string err;
  EXPECT_FALSE(Pattern::parse("post /cart", &err));
  EXPECT_NE(err.find("uppercase"), std::string::npos);
  EXPECT_FALSE(Pattern::parse("POST cart", &err));
  EXPECT_FALSE(Pattern::parse("POST /a//b", &err));
  EXPECT_NE(err.find("empty segment"), std::string::npos);
  EXPECT_FALSE(Pattern::parse("GET /a/{rest*}/b", &err));
  EXPECT_NE(err.find("last segment"), std::string::npos);
  EXPECT_FALSE(Pattern::parse("GET /a/{x-y}", &err));
  EXPECT_FALSE(Pattern::parse("GET  /a", &err));
  ASSERT_TRUE(Pattern::parse("* /", &err)) << err;

  auto p = *Pattern::parse("POST /cart/{id}", &err);
  EXPECT_TRUE(p.matches("post", path({"cart", "7"})));
  EXPECT_FALSE(p.matches("get", path({"cart", "7"})));
  EXPECT_FALSE(p.matches("post", path({"cart"})));
  EXPECT_FALSE(p.matches("post", path({"cart", "7", "x"})));
  auto get = *Pattern::parse("GET /feed", &err);
  EXPECT_TRUE(get.matches("head", path({"feed"})));  // GET covers HEAD
  auto rest = *Pattern::parse("* /docs/{rest*}", &err);
  EXPECT_TRUE(rest.matches("get", path({"docs"})));  // the rest may be empty
  EXPECT_TRUE(rest.matches("delete", path({"docs", "a", "b"})));
  EXPECT_FALSE(rest.matches("get", path({"doc"})));
  auto root = *Pattern::parse("GET /", &err);
  EXPECT_TRUE(root.matches("get", {}));
  EXPECT_FALSE(root.matches("get", path({"a"})));
}

// The most specific override wins: more literal segments, then fewer wildcards, then a named
// method over *, then the order they were listed in.
TEST(ControlsSpec, TheMostSpecificOverrideWins) {
  auto d = compiled(R"({"endpoints":[
      {"id":"any_api","match":"* /api/{rest*}","mode":"enforce"},
      {"id":"star_cart","match":"* /api/cart/{id}","mode":"enforce"},
      {"id":"post_cart","match":"POST /api/cart/{id}","mode":"enforce"},
      {"id":"post_cart_again","match":"POST /api/cart/{id}","mode":"enforce"},
      {"id":"cart_rest","match":"POST /api/cart/{rest*}","mode":"enforce"},
      {"id":"checkout","match":"POST /api/cart/checkout","mode":"enforce"}]})");
  auto winner = [&](const char* method, std::initializer_list<const char*> segs) {
    auto c = d.chain(method, path(segs), "");
    return c[3] ? c[3]->id : std::string("(none)");
  };
  EXPECT_EQ(winner("post", {"api", "cart", "checkout"}), "checkout");  // three literals
  EXPECT_EQ(winner("post", {"api", "cart", "7"}), "post_cart");       // named method, listed first
  EXPECT_EQ(winner("get", {"api", "cart", "7"}), "star_cart");
  EXPECT_EQ(winner("post", {"api", "cart", "7", "items"}), "cart_rest");
  EXPECT_EQ(winner("get", {"api", "other"}), "any_api");
  EXPECT_EQ(winner("get", {"elsewhere"}), "(none)");
}

TEST(ControlsSpec, OverridesNamingAnApiMatchOnlyIt) {
  auto d = compiled(R"({"apis":{"box:shop":{"mode":"enforce","set":{"rate":{"per-minute":100}}}},
      "endpoints":[{"id":"shop_only","api":"box:shop","match":"* /{rest*}","mode":"observe"}]})");
  EXPECT_TRUE(d.api_scoped);
  auto shop = d.chain("get", path({"x"}), "box:shop");
  ASSERT_TRUE(shop[2]);
  EXPECT_EQ(shop[2]->id, "box:shop");
  ASSERT_TRUE(shop[3]);
  auto content = d.chain("get", path({"x"}), "content");
  EXPECT_FALSE(content[2]);
  EXPECT_FALSE(content[3]);
}

// Per property, the most specific level wins; an observed level more specific than the enforced
// one is checked beside it, never in its place; "off" lifts a property at its level.
TEST(ControlsSpec, EachPropertyMergesOnItsOwn) {
  std::string err;
  auto host = parse_host_defaults(obj(R"({"rate":{"per-minute":600},"body-max-bytes":4096})"), kLimits, &err);
  ASSERT_TRUE(host) << err;
  auto d = compiled(R"({"defaults":{"mode":"enforce","set":{"rate":{"per-minute":300},"concurrent":{"max":50}}},
      "apis":{"mcp":{"mode":"enforce","set":{"concurrent":"off"}}},
      "endpoints":[{"id":"tools","match":"POST /mcp","mode":"observe","set":{"rate":{"per-minute":10}}}]})",
                    host);
  auto chain = d.chain("post", path({"mcp"}), "mcp");
  auto rate = effective(chain, Property::kRate);
  ASSERT_TRUE(rate.enforced);
  EXPECT_EQ(rate.enforced->id, "defaults");
  EXPECT_EQ(rate.enforced->level, Level::kDomain);  // the domain's 300 over the host's 600
  ASSERT_TRUE(rate.observed);
  EXPECT_EQ(rate.observed->id, "tools");
  auto body = effective(chain, Property::kBodyMaxBytes);
  ASSERT_TRUE(body.enforced);
  EXPECT_EQ(body.enforced->level, Level::kHost);
  EXPECT_EQ(monitored_id(*body.enforced), "defaults");
  auto conc = effective(chain, Property::kConcurrent);
  ASSERT_TRUE(conc.enforced);
  EXPECT_EQ(conc.enforced->id, "mcp");
  EXPECT_TRUE(conc.enforced->props.concurrent.off);
  EXPECT_FALSE(effective(chain, Property::kClosed).enforced);
}

TEST(ControlsSpec, StoredFilesCompileWholeOrNotAtAll) {
  std::string err;
  EXPECT_FALSE(compile(obj(R"({"endpoints":[{"id":"a","match":"GET /a","mode":"enforce"},
                                            {"id":"a","match":"GET /b","mode":"enforce"}]})"),
                       "example.com", "example", std::nullopt, kLimits, &err));
  EXPECT_NE(err.find("two overrides have the id a"), std::string::npos);
  EXPECT_FALSE(compile(obj(R"({"apis":{"box:Shop":{"mode":"enforce"}}})"), "example.com", "example", std::nullopt, kLimits, &err));
  EXPECT_NE(err.find("box:<box name>"), std::string::npos);
  EXPECT_FALSE(compile(obj(R"({"defaults":{"set":{}}})"), "example.com", "example", std::nullopt, kLimits, &err));
  EXPECT_NE(err.find("mode must be enforce or observe"), std::string::npos);
  EXPECT_FALSE(compile(obj(R"({"endpoints":[{"id":"defaults","match":"GET /a","mode":"enforce"}]})"), "example.com", "example",
                       std::nullopt, kLimits, &err));
  EXPECT_NE(err.find("names a control the host gives"), std::string::npos);
  EXPECT_FALSE(compile(obj(R"({"extra":1})"), "example.com", "example", std::nullopt, kLimits, &err));
  EXPECT_NE(err.find("unknown key extra"), std::string::npos);
}
