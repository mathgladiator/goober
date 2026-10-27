// Mutation fuzzing of every parser that reads text a person, an agent, or a box controls
// (design/machine/013.SecurityReview.md § Next time). Each target must neither crash nor
// take more than three seconds on any input.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "fuzz.h"
#include "goober/identity/identity.h"
#include "goober/json/json.h"
#include "goober/manifest/manifest.h"
#include "goober/mcp/mcp.h"
#include "goober/render/frontmatter.h"
#include "goober/render/markdown.h"
#include "goober/render/mustache.h"
#include "goober/router/path.h"
#include "goober/server_http/multipart.h"
#include "goober/server_http/server.h"
#include "goober/timers/cron.h"
#include "goober/types/types.h"
#include "goober/web/html.h"
#include "goober/web/robots.h"

using namespace goober;
namespace bj = boost::json;

TEST(Fuzz, Markdown) {
  fuzz::run("markdown",
            {"# Title\n\nSome *emphasis* and **strong** and `code`.\n\n- a\n- b\n  - c\n\n> quote\n",
             "| a | b |\n|---|---|\n| 1 | 2 |\n", "[link](https://x.example \"t\") ![img](https://x.example/i.png)\n",
             "```js\ncode\n```\n1. one\n2. two\n\n---\n<div>raw</div>\n[ref]: https://r.example\n[ref]\n"},
            {"*", "**", "_", "~~", "`", "```", "[", "]", "(", ")", "![", ">", "- ", "1. ", "#", "\n", "\n\n", "|", "\\",
             "<", "&amp;", "javascript:", "[x]: ", "    ", "- [ ] ", "\t"},
            fuzz::iterations(3000), [](const std::string& in) { (void)render::markdown_to_html(in); });
}

TEST(Fuzz, Mustache) {
  bj::object data{{"title", "<T>"}, {"list", bj::array{bj::object{{"x", 1}}, bj::object{{"x", 2}}}},
                  {"flag", true}, {"body", "<p>trusted</p>"}};
  render::TemplateOptions o;
  o.trusted_html = {"body"};
  o.partial = [](std::string_view name) -> std::optional<std::string> {
    if (name == "p") return std::string("{{> p}}{{title}}");
    if (name == "q") return std::string("{{#list}}{{x}}{{/list}}");
    return std::nullopt;
  };
  fuzz::run("mustache",
            {"<h1>{{title}}</h1>{{#list}}<li>{{x}}</li>{{/list}}{{^flag}}no{{/flag}}{{{body}}}",
             "{{> q}}{{! comment }}{{&title}}{{.}}", "{{#flag}}{{> p}}{{/flag}}"},
            {"{{", "}}", "{{#", "{{/", "{{^", "{{>", "{{{", "}}}", "{{!", "{{=", "list", "flag", "title", ".", "p", "q"},
            fuzz::iterations(3000), [&](const std::string& in) {
              std::string err;
              (void)render::check_mustache(in, &err);
              (void)render::render_mustache(in, data, o, &err);
            });
}

TEST(Fuzz, FrontMatter) {
  fuzz::run("frontmatter",
            {"---\ntitle: A\ntags:\n  - x\n  - y\nrequirements: [[\"owner\"]]\ndraft: false\n---\nbody",
             "---\nkey: \"quoted \\\" value\"\nn: 3\n...\nrest"},
            {"---\n", "...\n", ": ", "  - ", "[", "]", "{", "}", "\"", "'", "#", "\n", "\\"}, fuzz::iterations(3000),
            [](const std::string& in) {
              std::string err;
              (void)render::split_front_matter(in, &err);
            });
}

TEST(Fuzz, TypesDocumentsAndValues) {
  std::string doc = R"({"openapi":"3.1.0","components":{"schemas":{
    "A":{"type":"object","required":["n"],"additionalProperties":false,"properties":{
      "n":{"type":"integer","format":"int32","minimum":0},"s":{"type":"string","maxLength":5,"enum":["a","b"]},
      "l":{"type":"array","maxItems":3,"items":{"$ref":"#/components/schemas/B"}}}},
    "B":{"type":"object","additionalProperties":false,"properties":{"t":{"type":"string","format":"date-time"}}}}}})";
  fuzz::run("types-parse", {doc}, {"{", "}", "[", "]", "\"type\"", "\"$ref\"", "\"#/components/schemas/A\"", ",", ":", "\"object\"",
                                   "\"array\"", "\"maxItems\":", "1e308", "-", "null"},
            fuzz::iterations(2000), [](const std::string& in) {
              std::string err;
              (void)types::Types::parse(in, &err);
            });
  std::string err;
  auto t = types::Types::parse(doc, &err);
  ASSERT_TRUE(t) << err;
  fuzz::run("types-check", {R"({"n":1,"s":"a","l":[{"t":"2026-10-27T13:00:00Z"}]})", R"({"n":-1})"},
            {"{", "}", "[", "]", "\"n\":", "\"l\":", "null", "1e999", "\"", ",", "9223372036854775808"},
            fuzz::iterations(3000), [&](const std::string& in) {
              std::string e;
              auto v = json::parse(in, &e);
              if (v) (void)t->check("A", *v, &e);
            });
}

TEST(Fuzz, Manifests) {
  fuzz::run("manifest",
            {R"({"box":"b","domains":["*"],"types":"t.json","data":{"sqlite3":{"s":{"schema":"x1","database":"d","pattern":"sql",
              "tables":{"t":{"read":true,"insert":true,"update":["a"]}}}}},
              "external":{"e":{"base":"https://api.example","credentials":"keys/system/k","methods":{"m":{"verb":"post","path":"/x/{id}",
              "require":{"body.to":"{!email}"}}}}},
              "routes":{"http":{"/a/{id : int64}":{"get":{"name":"a","requirements":[["owner"],["admin/p_{id}"]],"query":{"q":{"type":"string"}}}}},
              "mcp":{"t":{"tool":{"description":"d","requirements":[]}}},"timer":{"x":{"name":"x","cron":"0 6 * * *"}},
              "internal":{"/i":{"post":{"name":"i","callers":["c"]}}}}})"},
            {"{", "}", "[", "]", "\"", ",", ":", "{!", "}", "/", "{id}", "\"*\"", "\"requirements\"", "\"public\":true",
             "\"cron\":\"", "*/", "\"routes\"", "null", "0"},
            fuzz::iterations(2000), [](const std::string& in) {
              std::string err;
              (void)manifest::parse_manifest(in, &err);
            });
}

TEST(Fuzz, PathsQueriesAndPatterns) {
  fuzz::run("paths", {"/a/b%20c/d", "//x///y/", "/%2e%2e/%2F", "?a=1&b=%zz&c"},
            {"/", "%", "%2f", "%00", "..", ".", "+", "&", "=", "?", "#", "%e2%80%ae"}, fuzz::iterations(3000),
            [](const std::string& in) {
              (void)router::normalize_path(in);
              (void)router::parse_query(in);
              std::string err;
              (void)router::parse_pattern(in, &err);
            });
}

TEST(Fuzz, CronAndInstants) {
  auto zone = timers::find_zone("America/Chicago");
  fuzz::run("cron", {"0 6 * * *", "*/15 9-17 * * 1-5", "5,35 0 1 1,7 0"},
            {" ", "*", "/", "-", ",", "0", "59", "60", "7", "31"}, fuzz::iterations(2000), [&](const std::string& in) {
              std::string err;
              if (auto c = timers::Cron::parse(in, &err)) {
                (void)c->next(1793000000000, zone);
                (void)c->previous(1793000000000, zone);
              }
              (void)timers::parse_instant_ms(in);
            });
}

TEST(Fuzz, RedirectUris) {
  identity::Client c{"x", "x", {"http://127.0.0.1/cb", "com.example.app:/cb", "https://app.example.com/cb"}, false, "", 0};
  fuzz::run("redirects", {"http://127.0.0.1:5000/cb", "com.example.app:/cb", "https://app.example.com/cb"},
            {":", "/", "@", "#", "?", "..", "%", "127.0.0.1", "[::1]", "localhost", "\\", " "}, fuzz::iterations(3000),
            [&](const std::string& in) {
              (void)identity::redirect_allowed(c, in);
              (void)identity::is_valid_redirect_uri(in);
            });
}

TEST(Fuzz, McpJsonRpc) {
  mcp::McpServer server("fuzz", "1", "fuzzing");
  server.add_tool({"echo", "echoes", mcp::object_schema({}, {}), [](const bj::object& a) { return mcp::ToolResult::ok(a); }});
  fuzz::run("mcp",
            {R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"a":1}}})",
             R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})",
             R"([{"jsonrpc":"2.0","method":"notifications/initialized"}])"},
            {"{", "}", "[", "]", "\"", ",", ":", "\"id\"", "\"method\"", "\"params\"", "null", "true"},
            fuzz::iterations(3000), [&](const std::string& in) { (void)server.handle(in); });
}

// The crawler reads pages anyone on the web wrote (design/machine/017.Crawl.md).
TEST(Fuzz, HtmlViewsSelectorsAndRobots) {
  auto base = *web::parse_url("https://ranch.example/shop/");
  std::string err;
  auto spec = *json::parse(R"({"items":"li","fields":{"t":"","h":"a@href","c":"@class"}})", &err);
  fuzz::run("html",
            {"<!doctype html><html><head><title>T &amp; U</title><meta name=description content=d>"
             "<script type=application/ld+json>{\"@graph\":[{\"@type\":\"Product\"}]}</script></head>"
             "<body><h1>H</h1><p>a<b>b</b><ul><li class=x><a href=../p>l</a><li>m</ul><table><tr><th>a<td>b</table>"
             "<pre> x </pre><blockquote><p>q</blockquote></body></html>"},
            {"<", ">", "</", "<p>", "<li>", "<table>", "<tr>", "<td>", "</table>", "<script>", "</script>", "<!--",
             "-->", "&", "&#x", ";", "\"", "'", "=", "<a href=", "<img src=", "<pre>", "<base href=//x/>", "<ul>",
             "<blockquote>", "<h1>", "\xEF\xBB\xBF", "<!", "<?", "/>", "<style>"},
            fuzz::iterations(2000), [&](const std::string& in) {
              auto doc = web::Document::parse(in);
              auto b = doc.base_for(base);
              (void)web::view_text(doc);
              (void)web::view_markdown(doc, b);
              (void)web::view_links(doc, b);
              (void)web::view_meta(doc, b);
              (void)web::view_jsonld(doc);
              (void)web::view_tables(doc);
              std::string e;
              (void)web::view_select(doc, "ul > li a, .x", &e);
              (void)web::view_extract(doc, b, spec, &e);
            });
  fuzz::run("selectors", {"div.a > p#b [x^=y]:first-child, li:nth-child(2)"},
            {" ", ">", ",", ".", "#", "[", "]", "=", "^=", "*", ":", "(", ")", "\"", "a", "-"}, fuzz::iterations(3000),
            [&](const std::string& in) {
              std::string e;
              auto doc = web::Document::parse("<div class=a><p id=b x=yz>t</p></div>");
              if (auto s = web::Selector::parse(in, &e)) (void)s->select(doc, doc.root(), 10);
            });
  fuzz::run("robots", {"User-agent: *\nDisallow: /a*b$\nAllow: /a\nCrawl-delay: 1\nSitemap: https://x/s.xml\n"},
            {"User-agent:", "Disallow:", "Allow:", "*", "$", "/", "\n", "#", "Crawl-delay:", "goober"},
            fuzz::iterations(3000), [](const std::string& in) {
              auto r = web::Robots::parse(in, "goober");
              (void)r.allowed("/a/b/c?d=e");
            });
  fuzz::run("urls", {"https://a.example:8080/x/../y?q=1#f"}, {"/", "..", ".", "?", "#", "%", "@", ":", "[", "]", " ", "//"},
            fuzz::iterations(3000), [&](const std::string& in) {
              (void)web::parse_url(in);
              (void)web::resolve(base, in);
            });
}

// The attachment upload form's parser (design/machine/007.Content.md §7).
TEST(Fuzz, Multipart) {
  const std::string ct = "multipart/form-data; boundary=XyZ";
  fuzz::run("multipart",
            {"--XyZ\r\nContent-Disposition: form-data; name=\"alt\"\r\n\r\na cat\r\n--XyZ\r\nContent-Disposition: "
             "form-data; name=\"file\"; filename=\"cat.png\"\r\nContent-Type: image/png\r\n\r\n\x89PNG\r\n--XyZ--\r\n"},
            {"--XyZ", "\r\n", "\r\n\r\n", "Content-Disposition: form-data; ", "name=", "filename=", "\"", ";", "--"},
            fuzz::iterations(3000), [&](const std::string& in) {
              std::string err;
              (void)server::http::parse_multipart(in, ct, &err);
              (void)server::http::parse_multipart(in, in.substr(0, 200), &err);
            });
}

// The HTTP server itself, over real sockets: malformed requests never take it down, and it
// still answers a good request afterwards.
TEST(Fuzz, HttpServer) {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  server::http::ServerOptions o;
  o.port = 0;
  o.threads = 2;
  o.idle_timeout_seconds = 2;
  o.body_max_bytes = 4096;
  server::http::Server server(o, [](server::http::Request req, server::http::Responder respond) {
    server::http::Response res;
    res.headers.emplace_back("content-type", "text/plain");
    res.body = req.method + " " + req.path + " " + std::to_string(req.body.size());
    respond(std::move(res));
  });
  server.start();
  auto send = [&](const std::string& raw) -> std::string {
    asio::io_context io;
    beast::tcp_stream stream(io);
    beast::error_code ec;
    stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.port()), ec);
    if (ec) return "connect failed";
    stream.expires_after(std::chrono::seconds(3));
    asio::write(stream.socket(), asio::buffer(raw), ec);
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    std::string out;
    char buf[4096];
    while (true) {
      size_t n = stream.socket().read_some(asio::buffer(buf), ec);
      if (ec || n == 0) break;
      out.append(buf, n);
      if (out.size() > (1 << 20)) break;
    }
    return out;
  };
  fuzz::run("http-server",
            {"GET /a?b=c HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
             "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello",
             "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"},
            {"\r\n", ": ", "Content-Length: ", "Transfer-Encoding: chunked\r\n", "Host: ", "GET ", "POST ", "HTTP/1.1",
             "HTTP/1.0", "%00", "%", "?", "/", "\0", "ffffffff\r\n", "99999999999999999999", " ", "\t",
             "Expect: 100-continue\r\n", "Connection: keep-alive\r\n"},
            fuzz::iterations(300), [&](const std::string& in) { (void)send(in); });
  auto alive = send("GET /still HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_NE(alive.find("get /still 0"), std::string::npos) << alive;
}
