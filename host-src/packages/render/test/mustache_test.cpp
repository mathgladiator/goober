#include <gtest/gtest.h>

#include <chrono>

#include "goober/render/mustache.h"

using namespace goober;
using namespace goober::render;

namespace {
std::string R(std::string_view t, std::string_view data_json, TemplateOptions o = {}) {
  std::string err;
  auto data = json::parse(data_json, &err);
  EXPECT_TRUE(data) << err;
  auto out = render_mustache(t, *data, o, &err);
  EXPECT_TRUE(out) << err;
  return out.value_or("<error: " + err + ">");
}
}  // namespace

TEST(Mustache, VariablesEscapeEverything) {
  EXPECT_EQ(R("Hi {{name}}!", R"({"name":"<b>&\"'"})"), "Hi &lt;b&gt;&amp;&quot;&#39;!");
  EXPECT_EQ(R("{{a.b.c}} {{n}} {{f}} {{t}} {{missing}}.", R"({"a":{"b":{"c":"deep"}},"n":7,"f":1.5,"t":true})"),
            "deep 7 1.5 true .");
  // Triple braces don't unescape a value the host didn't render.
  EXPECT_EQ(R("{{{name}}}{{& name}}", R"({"name":"<i>"})"), "&lt;i&gt;&lt;i&gt;");
}

TEST(Mustache, TrustedHtmlOnlyAtTheTop) {
  TemplateOptions o;
  o.trusted_html = {"body"};
  EXPECT_EQ(R("<main>{{{body}}}</main>|{{body}}", R"({"body":"<p>hi</p>"})", o),
            "<main><p>hi</p></main>|&lt;p&gt;hi&lt;/p&gt;");
  // A "body" inside a list item is someone's data, so it stays escaped.
  EXPECT_EQ(R("{{#pages}}{{{body}}}{{/pages}}", R"({"body":"<p>top</p>","pages":[{"body":"<script>"}]})", o),
            "&lt;script&gt;");
}

TEST(Mustache, SectionsListsAndInverted) {
  const char* data = R"({"pages":[{"t":"a"},{"t":"b"}],"none":[],"yes":true,"no":false,"empty":"","obj":{"x":1},"tags":["x","y"]})";
  EXPECT_EQ(R("{{#pages}}[{{t}}]{{/pages}}", data), "[a][b]");
  EXPECT_EQ(R("{{#none}}x{{/none}}{{^none}}nothing{{/none}}", data), "nothing");
  EXPECT_EQ(R("{{#yes}}Y{{/yes}}{{#no}}N{{/no}}{{^no}}n{{/no}}{{#empty}}E{{/empty}}", data), "Yn");
  EXPECT_EQ(R("{{#obj}}{{x}}{{/obj}}", data), "1");
  EXPECT_EQ(R("{{#tags}}<{{.}}>{{/tags}}", data), "<x><y>");
  // Names resolve outward through the stack.
  EXPECT_EQ(R("{{#pages}}{{t}}-{{site}};{{/pages}}", R"({"site":"S","pages":[{"t":"a"}]})"), "a-S;");
}

TEST(Mustache, StandaloneTagsTakeTheirLine) {
  EXPECT_EQ(R("<ul>\n  {{#items}}\n  <li>{{.}}</li>\n  {{/items}}\n</ul>\n", R"({"items":["a","b"]})"),
            "<ul>\n  <li>a</li>\n  <li>b</li>\n</ul>\n");
  EXPECT_EQ(R("a\n{{! a comment }}\nb", "{}"), "a\nb");
  EXPECT_EQ(R("a {{#x}}inline{{/x}} b", R"({"x":true})"), "a inline b");
}

TEST(Mustache, PartialsAndDepth) {
  TemplateOptions o;
  o.partial = [](std::string_view name) -> std::optional<std::string> {
    if (name == "item") return std::string("<li>{{t}}</li>\n");
    if (name == "loop") return std::string("{{> loop}}");
    return std::nullopt;
  };
  EXPECT_EQ(R("<ul>\n  {{> item}}\n</ul>", R"({"t":"x"})", o), "<ul>\n  <li>x</li>\n</ul>");
  EXPECT_EQ(R("[{{> nope}}]", "{}", o), "[]");
  std::string err;
  EXPECT_FALSE(render_mustache("{{> loop}}", boost::json::object(), o, &err));
  EXPECT_NE(err.find("deeper"), std::string::npos);
}

TEST(Mustache, MalformedTemplatesAreErrors) {
  std::string err;
  EXPECT_FALSE(check_mustache("{{#a}}open", &err));
  EXPECT_FALSE(check_mustache("{{#a}}x{{/b}}", &err));
  EXPECT_FALSE(check_mustache("{{unclosed", &err));
  EXPECT_FALSE(check_mustache("{{=<% %>=}}", &err));
  EXPECT_TRUE(check_mustache("plain {{x}} {{{y}}} {{#s}}{{/s}}", &err)) << err;
}

TEST(Mustache, OutputAndPartialsAreBounded) {
  TemplateOptions o;
  // Each partial includes the next twice: 2^16 copies without a bound.
  o.partial = [](std::string_view name) -> std::optional<std::string> {
    int n = std::stoi(std::string(name.substr(1)));
    if (n >= 16) return std::string(1000, 'x');
    std::string next = "p" + std::to_string(n + 1);
    return "{{> " + next + "}}{{> " + next + "}}";
  };
  std::string err;
  EXPECT_FALSE(render_mustache("{{> p0}}", boost::json::object{}, o, &err));
  EXPECT_FALSE(err.empty());
}

// Security review 4 (S4-1, S4-3): nesting is bounded, so rendering can't exhaust the stack,
// and parsing is linear, so a long one-line template can't pin a thread.
TEST(Mustache, HostileTemplatesAreBounded) {
  std::string deep;
  for (int i = 0; i < 10000; ++i) deep += "{{#now}}";
  for (int i = 0; i < 10000; ++i) deep += "{{/now}}";
  std::string err;
  auto data = json::parse(R"({"now":true})", &err);
  EXPECT_FALSE(render_mustache(deep, *data, {}, &err));
  EXPECT_NE(err.find("nest deeper"), std::string::npos) << err;

  std::string ok_depth;
  for (int i = 0; i < 64; ++i) ok_depth += "{{#now}}";
  ok_depth += "x";
  for (int i = 0; i < 64; ++i) ok_depth += "{{/now}}";
  EXPECT_EQ(R(ok_depth, R"({"now":true})"), "x");

  std::string wide;
  for (int i = 0; i < 100000; ++i) wide += "{{a}}";
  auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(render_mustache(wide, *json::parse(R"({"a":"b"})", &err), {}, &err)) << err;
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  // Standalone lines still work right after a standalone tag.
  EXPECT_EQ(R("{{#a}}\n  {{/a}}\nz", R"({"a":true})"), "z");
}
