// Mutation fuzzing of every parser that reads text a person, an agent, or a box controls
// (design/machine/013.SecurityReview.md § Next time). Each target must neither crash nor
// take more than three seconds on any input.

#include "goober/calendar/ics.h"
#include "goober/calendar/rrule.h"
#include "goober/libraries/libraries.h"
#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "fuzz.h"
#include <cstring>
#include <filesystem>

#include "goober/backup/backup.h"
#include "goober/base/files.h"
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
#include "goober/source/source.h"
#include "goober/timers/cron.h"
#include "goober/types/types.h"
#include "goober/web/html.h"
#include "goober/web/robots.h"
#include "goober/signing/oauth1.h"
#include "goober/site/includes.h"
#include "goober/specs/specs.h"
#include "goober/vault/vault.h"

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

// Diffs between revisions of agents' source (design/machine/020.Source.md): bounded time and
// memory on any pair of texts, and an empty diff only for equal ones.
TEST(Fuzz, UnifiedDiff) {
  fuzz::run("unified diff", {"one\ntwo\nthree\nfour\n", "a\n\nb\n\n\nc", "fn main() {\n    hi();\n}\n"},
            {"\n", "x\n", "same\n", "\r\n", "two\n"}, fuzz::iterations(2000), [](const std::string& in) {
              size_t cut = in.size() / 2;
              std::string a = in.substr(0, cut), b = in.substr(cut);
              std::string d = source::unified_diff(a, b, "a", "b");
              if ((a == b) != d.empty()) throw std::runtime_error("diff emptiness disagrees with equality");
            });
}

// Restore points and moments people type (design/machine/019.Backups.md §5).
TEST(Fuzz, BackupNamesAndMoments) {
  fuzz::run("backup names",
            {"nightly/2026-10-27T03-30-00Z.tar.gz.sealed", "increments/2026-10-27T03-30-00Z/2026-10-27T09-00-00Z/MANIFEST.json.sealed",
             "2026-10-26T18:00", "2026-10-26"},
            {"/", "-", "T", "Z", ":", "nightly/", "increments/", ".sealed", "99", "0"}, fuzz::iterations(3000),
            [](const std::string& in) {
              (void)backup::parse_moment(in);
              (void)backup::parse_stamp(in);
              auto points = backup::restore_points({in, "nightly/2026-10-27T03-30-00Z.tar.gz.sealed"});
              (void)backup::choose_point(points, 0);
            });
}

// A delta or a sealed file from a bucket is checked before use: damage is an error, never a
// crash or a write past the declared size.
TEST(Fuzz, PageDeltasAndSealedFiles) {
  auto dir = std::filesystem::path(GOOBER_TEST_TMP) / "fuzz-backup";
  std::filesystem::create_directories(dir);
  std::string db(4096 * 3, 'a');
  std::memcpy(db.data(), "SQLite format 3\0\x10\x00", 18);
  base::write_file_durable(dir / "base.db", db);
  std::string err;
  auto map = backup::map_pages(dir / "base.db", &err);
  std::string changed = db;
  changed[5000] = 'Z';
  base::write_file_durable(dir / "now.db", changed);
  backup::DeltaInfo info;
  backup::write_delta(&*map, dir / "now.db", dir / "delta", &info, &err);
  std::string delta = *base::read_file(dir / "delta");
  backup::Key key = backup::new_key();
  backup::seal_file(key, dir / "delta", dir / "sealed", &err);
  std::string sealed = *base::read_file(dir / "sealed");
  fuzz::run("page deltas", {delta}, {"\n", "{", "}", "\"bytes\":", "\"page_size\":", "0", "65536", "\xff\xff\xff\xff"},
            fuzz::iterations(500), [&](const std::string& in) {
              base::write_file_durable(dir / "d", in);
              std::filesystem::path base = dir / "base.db";
              std::string e;
              if (backup::apply_delta(&base, dir / "d", dir / "out.db", &e) &&
                  std::filesystem::file_size(dir / "out.db") > (1u << 26)) {
                throw std::runtime_error("a delta grew a database past any sane size");
              }
            });
  fuzz::run("sealed files", {sealed}, {"GOOBERS1", "\0", "\xff"}, fuzz::iterations(500), [&](const std::string& in) {
    base::write_file_durable(dir / "s", in);
    std::string e;
    if (in != sealed && backup::unseal_file(key, dir / "s", dir / "plain", &e)) {
      throw std::runtime_error("a changed sealed file was accepted");
    }
  });
}

// iCalendar and the libraries that read it (design/machine/831.Libraries.md): any bytes parse
// or are refused, never crash, and recurrences stay inside their limits.
TEST(Fuzz, IcalendarAndRecurrence) {
  const std::string invite =
      "BEGIN:VCALENDAR\r\nMETHOD:REQUEST\r\nBEGIN:VTIMEZONE\r\nTZID:X\r\nBEGIN:STANDARD\r\nDTSTART:16010101T020000\r\n"
      "TZOFFSETFROM:-0500\r\nTZOFFSETTO:-0600\r\nRRULE:FREQ=YEARLY;BYDAY=1SU;BYMONTH=11\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n"
      "BEGIN:VEVENT\r\nUID:a@b\r\nDTSTART;TZID=X:20261105T180000\r\nDTEND;TZID=America/Chicago:20261105T200000\r\n"
      "RRULE:FREQ=WEEKLY;BYDAY=TH,FR;COUNT=50\r\nEXDATE:20261112T000000Z\r\nATTENDEE;PARTSTAT=ACCEPTED;CN=\"A, B\":mailto:a@b.c\r\n"
      " \r\nSUMMARY:x\\, y\\n z\r\nBEGIN:VALARM\r\nTRIGGER:-PT15M\r\nEND:VALARM\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  const std::string counter =
      "BEGIN:VCALENDAR\nMETHOD:COUNTER\nBEGIN:VEVENT\nUID:a@b\nDTSTART;VALUE=DATE:20261106\nATTENDEE:mailto:z@y.x\nEND:VEVENT\n"
      "END:VCALENDAR\n";
  const auto* ics = libraries::find("ics", 1);
  fuzz::run("icalendar", {invite, counter},
            {"BEGIN:", "END:", "VCALENDAR", "VEVENT", "VTIMEZONE", "\r\n", "\n ", ";", ":", ",", "\"", "=", "TZID=",
             "RRULE:", "FREQ=SECONDLY", "BYSETPOS=-1", "BYDAY=-53MO", "BYWEEKNO=53", "COUNT=", "UNTIL=99991231",
             "INTERVAL=0", "DTSTART:", "Z", "T", "\\", "\\n", "PARTSTAT=", "METHOD:", "DURATION:P", "BYYEARDAY=-366"},
            fuzz::iterations(3000), [&](const std::string& in) {
              std::string err;
              if (auto p = calendar::parse(in, &err)) {
                for (const auto& c : p->components) (void)calendar::serialize(c);
              }
              libraries::Context ctx{1793286000000};
              bj::object arg{{"ics", in}, {"me", "a@b.c"}};
              for (const char* fn : {"parse", "interpret"}) (void)libraries::call(*ics, fn, json::dump(arg), ctx);
              arg["from"] = "2026-01-01T00:00:00Z";
              arg["to"] = "2028-01-01T00:00:00Z";
              (void)libraries::call(*ics, "expand", json::dump(arg), ctx);
            });
  // Rules alone, expanded far: bounded by the cap, whatever they say.
  fuzz::run("rrule", {"FREQ=DAILY;COUNT=10", "FREQ=MONTHLY;BYDAY=-1FR;BYSETPOS=1", "FREQ=YEARLY;BYWEEKNO=1,-1;BYDAY=MO"},
            {";", "=", ",", "BYDAY=", "BYMONTHDAY=", "BYHOUR=", "BYMINUTE=", "BYSECOND=", "FREQ=SECONDLY", "FREQ=YEARLY",
             "-", "1", "53", "366", "MO", "SU", "WKST=", "INTERVAL=", "BYSETPOS="},
            fuzz::iterations(2000), [&](const std::string& in) {
              std::string err;
              if (auto r = calendar::parse_rule(in, &err)) {
                auto e = calendar::expand(calendar::Civil{2026, 1, 31, 9, 0, 0}, *r, calendar::Civil{2030, 1, 1}, 2000,
                                          [](const calendar::Civil& c) { return calendar::civil_seconds(c) * 1000; });
                EXPECT_LE(e.instances.size(), 2000u);
              }
            });
}

// OpenAPI documents an agent puts (034 § 2): parsing, $refs (including cycles), and finding ops.
TEST(Fuzz, OpenApiSpecs) {
  fuzz::run("specs",
            {R"({"openapi":"3.1.0","info":{"title":"T","version":"1"},"servers":[{"url":"https://a.example/v1"}],"paths":{)"
             R"("/w/{id}":{"parameters":[{"$ref":"#/components/parameters/id"}],"get":{"operationId":"getW"},)"
             R"("post":{"requestBody":{"$ref":"#/components/requestBodies/b"}}}},"components":{"parameters":{"id":)"
             R"({"name":"id","in":"path","required":true}},"requestBodies":{"b":{"content":{"multipart/form-data":{}}}}}})",
             R"({"swagger":"2.0","basePath":"/api/v3","host":"x.example","paths":{"/a":{"get":{"operationId":"a",)"
             R"("parameters":[{"name":"f","in":"formData","type":"file"}]}}}})",
             R"({"openapi":"3.0.0","paths":{"/c":{"$ref":"#/paths/~1c"}}})"},
            {"{", "}", "\"$ref\":\"#/\"", "\"#/components/x\"", "~1", "~0", "\"paths\"", "\"in\":\"path\"", "[", "]", ",", ":",
             "\"get\"", "\"operationId\"", "null", "\"/{x}\""},
            fuzz::iterations(3000), [](const std::string& in) {
              std::string err;
              if (auto s = specs::parse(in, &err)) {
                for (const char* ref : {"getW", "GET /w/{id}", "POST /a", "a", ""}) (void)s->find(ref);
              }
            });
}

// Include markers with parameters, in any served HTML (027 § 7).
TEST(Fuzz, IncludeMarkers) {
  fuzz::run("includes",
            {R"(<nav><!--#include nav--></nav><!--#include card title="Ribs & brisket" href="/ribs" badge="new"-->)",
             R"(<a href="/x" data-match="/x" class=a>x</a><meta name="nav-key" content="k"><a data-key="k">)"},
            {"<!--#include ", "-->", "\"", "=", " ", "--", "<a ", "class=", "data-match=\"", "data-key=", ">", "<meta "},
            fuzz::iterations(3000), [](const std::string& in) {
              (void)site::include_names(in);
              (void)site::mark_active(in, "/x/y", "k");
              (void)site::mark_active(in, "/", "");
            });
}

// Sealed values come back from boxes' tables: anything that isn't the host's opens to nothing.
TEST(Fuzz, SealedValues) {
  vault::Key root{};
  for (size_t i = 0; i < root.size(); ++i) root[i] = uint8_t(i * 7);
  auto key = vault::tenant_key(root, "t");
  std::string good = vault::seal_for(key, "t", "svc", "42", R"({"kind":"key","value":"k"})");
  fuzz::run("sealed", {good, "gsv1.42.AAAA", "gsv1.."}, {"gsv1.", ".", "A", "_", "-", "=", "42", "%"}, fuzz::iterations(3000),
            [&](const std::string& in) {
              auto opened = vault::open_for(key, "t", "svc", in);
              // Only the real value opens; a mutation never does.
              if (opened) EXPECT_EQ(in, good);
            });
}

// OAuth 1.0a signing over box-supplied URLs and form fields (any-public methods).
TEST(Fuzz, OAuth1Urls) {
  signing::OAuth1 o{"ck", "cs", "t", "ts", "nonce", "1"};
  fuzz::run("oauth1", {"https://a.example/1.1/statuses/update.json?include_entities=true&b=%41+c"},
            {"%", "%zz", "%4", "+", "&", "=", "?", "#", "/", ":", "@", "%2B"}, fuzz::iterations(3000), [&](const std::string& in) {
              (void)signing::oauth1_base_string("POST", in, {{"status", in}}, o);
              (void)signing::oauth1_header("GET", in, {}, o);
            });
}
