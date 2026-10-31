// End to end: pages, templates, and attachments over HTTP and the operator MCP tools
// (design/machine/007.Content.md).

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/base/hash.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

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
  req.set(bhttp::field::host, "localhost");
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

const std::string kIend = std::string("\0\0\0\0IEND\xAE\x42\x60\x82", 12);
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "not-really-a-png" + kIend;
// A PNG-shaped file of exactly `size` bytes: the signature, filler, and the IEND chunk.
std::string png_of(size_t size) { return std::string("\x89PNG\r\n\x1a\n", 8) + std::string(size - 20, 'x') + kIend; }

const char* kJobWat = R"((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 64) "{\"id\":\"7\",\"quantity\":3,\"note\":\"<b>hi</b>\"}")
  (func (export "goober_route_job") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 64) (i32.const 42)))
    (i32.const 0))
))";

}  // namespace

class SiteRoutesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "content_routes";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"localhost": {"tenant": "playground", "owner": "boss@example.com"}},
        "operations": {"token": "test-token-0123456789"},
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
    std::string err;
    auto doc = json::parse(r.body, &err);
    EXPECT_TRUE(doc) << r.body;
    const auto& result = doc->at("result");
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::object{{"error", text}};
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(SiteRoutesTest, AgentBuildsASiteWithPagesTemplatesAndAListing) {
  auto put = tool("page_put", {{"tenant", "playground"}, {"path", "/"}, {"format", "markdown"},
                               {"body", "# Welcome\n\nHello **world**."}, {"fields", bj::object{{"summary", "Home"}}}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  EXPECT_EQ(put.at("file"), "index.md");
  auto home = fetch(port, bhttp::verb::get, "/", {{"accept", "text/html"}});
  ASSERT_EQ(home.status, 200) << home.body;
  EXPECT_NE(home.body.find("<strong>world</strong>"), std::string::npos);
  EXPECT_NE(home.headers["content-security-policy"].find("script-src 'self'"), std::string::npos);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/", {{"if-none-match", home.headers["etag"]}}).status, 304);

  // A listing over posts, with a tenant template.
  auto t = tool("template_put", {{"tenant", "playground"}, {"name", "list"},
                                 {"template", "<ul>{{#pages}}<li><a href=\"{{url}}\">{{title}}</a></li>{{/pages}}</ul>"}});
  ASSERT_FALSE(t.as_object().contains("error")) << json::dump(t);
  EXPECT_TRUE(tool("template_put", {{"tenant", "playground"}, {"name", "bad"}, {"template", "{{#x}}"}})
                  .as_object()
                  .contains("error"));
  tool("page_put", {{"tenant", "playground"}, {"path", "/blog"}, {"format", "markdown"}, {"body", "Posts."},
                    {"fields", bj::object{{"list", "blog"}}}});
  tool("page_put", {{"tenant", "playground"}, {"path", "/blog/first"}, {"format", "markdown"}, {"body", "One."},
                    {"title", "First \"post\""}, {"fields", bj::object{{"date", "2026-10-01"}}}});
  tool("page_put", {{"tenant", "playground"}, {"path", "/blog/second"}, {"format", "fragment"},
                    {"body", "<p>Two.</p>"}, {"title", "Second"}, {"fields", bj::object{{"date", "2026-10-02"}}}});
  auto blog = fetch(port, bhttp::verb::get, "/blog");
  ASSERT_EQ(blog.status, 200) << blog.body;
  EXPECT_EQ(blog.body, "<ul><li><a href=\"/blog/second\">Second</a></li><li><a href=\"/blog/first\">First "
                       "&quot;post&quot;</a></li></ul>");
  // Switching a page's format leaves one page at the URL.
  auto again = tool("page_put", {{"tenant", "playground"}, {"path", "/blog/first"}, {"format", "document"},
                                 {"body", "<!doctype html><p>Whole page</p>"}});
  EXPECT_EQ(again.at("replaced").at(0), "blog/first.md");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/blog/first").body, "<!doctype html><p>Whole page</p>");

  auto preview = tool("page_render", {{"verbose", true}, {"tenant", "playground"}, {"path", "/blog"}});
  EXPECT_EQ(preview.at("status"), 200);
  EXPECT_EQ(preview.at("kind"), "listing");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/_templates/list.html").status, 404);
  auto guide = tool("site_guide", {});
  EXPECT_NE(std::string(guide.as_string()).find("attachment_put"), std::string::npos);
}

TEST_F(SiteRoutesTest, AttachmentsUploadServeAndReportUse) {
  // Upload with an operator token, as a raw body.
  auto up = fetch(port, bhttp::verb::post, "/attachment/upload?name=cat.png&alt=A%20cat",
                  {{"authorization", "Bearer test-token-0123456789"}, {"content-type", "image/png"}}, kPng);
  ASSERT_EQ(up.status, 201) << up.body;
  std::string err;
  auto a = json::parse(up.body, &err);
  EXPECT_EQ(a->at("url"), "/attachment/4294967297.png");
  auto img = fetch(port, bhttp::verb::get, "/attachment/4294967297.png");
  EXPECT_EQ(img.status, 200);
  EXPECT_EQ(img.body, kPng);
  EXPECT_EQ(img.headers["content-type"], "image/png");
  EXPECT_NE(img.headers["cache-control"].find("immutable"), std::string::npos);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/attachment/4294967297.jpg").status, 404);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/attachment/4294967298.png").status, 404);

  // Uploads and the manager need an editor; HTML is refused whatever it claims to be.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/attachment/upload", {}, kPng).status, 401);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/attachment/", {{"accept", "text/html"}}).status, 303);
  auto html = fetch(port, bhttp::verb::post, "/attachment/upload",
                    {{"authorization", "Bearer test-token-0123456789"}, {"content-type", "image/png"}},
                    "<html><script>alert(1)</script>");
  EXPECT_EQ(html.status, 400);

  // A multipart form, as a browser sends it.
  std::string body = "--XyZ\r\nContent-Disposition: form-data; name=\"alt\"\r\n\r\nAn SVG\r\n"
                     "--XyZ\r\nContent-Disposition: form-data; name=\"file\"; filename=\"d.svg\"\r\n"
                     "Content-Type: image/svg+xml\r\n\r\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>\r\n--XyZ--\r\n";
  auto form = fetch(port, bhttp::verb::post, "/attachment/upload",
                    {{"authorization", "Bearer test-token-0123456789"},
                     {"content-type", "multipart/form-data; boundary=XyZ"}},
                    body);
  ASSERT_EQ(form.status, 201) << form.body;
  auto svg = fetch(port, bhttp::verb::get, "/attachment/4294967298.svg");
  EXPECT_EQ(svg.headers["content-disposition"].substr(0, 10), "attachment");
  EXPECT_NE(svg.headers["content-security-policy"].find("sandbox"), std::string::npos);

  // Over MCP: base64 in, and the list knows what uses each one.
  auto put = tool("attachment_put", {{"tenant", "playground"}, {"base64", base::base64_encode(kPng)}, {"alt", "dog"}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  EXPECT_EQ(put.at("url"), "/attachment/4294967299.png");
  EXPECT_EQ(put.at("markdown"), "![dog](/attachment/4294967299.png)");
  tool("page_put", {{"tenant", "playground"}, {"path", "/pets"}, {"format", "markdown"}, {"body", "![dog](/attachment/4294967299.png)"}});
  auto unused = tool("attachment_list", {{"tenant", "playground"}, {"unused", true}});
  EXPECT_EQ(unused.as_array().size(), 2u);
  // An id given as a string of digits, as ids are elsewhere in JSON, works too.
  EXPECT_EQ(tool("attachment_get", {{"tenant", "playground"}, {"id", "4294967299"}}).at("url"), "/attachment/4294967299.png");
  EXPECT_TRUE(tool("attachment_get", {{"tenant", "playground"}, {"id", "12x"}}).as_object().contains("error"));
  auto refused = tool("attachment_delete", {{"tenant", "playground"}, {"id", "4294967299"}});
  EXPECT_NE(std::string(refused.at("error").as_string()).find("pets.md"), std::string::npos);
  EXPECT_EQ(tool("attachment_delete", {{"tenant", "playground"}, {"id", 4294967299}, {"force", true}}).at("deleted"), 4294967299);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/attachment/4294967299.png").status, 404);
  EXPECT_TRUE(tool("attachment_put", {{"tenant", "playground"}, {"url", "http://example.com/x.png"}})
                  .as_object()
                  .contains("error"));
}

TEST_F(SiteRoutesTest, PagesWithRequirementsAndBoxRoutesWithTemplates) {
  tool("page_put", {{"tenant", "playground"}, {"path", "/handbook"}, {"format", "markdown"}, {"body", "Secret."},
                    {"fields", bj::object{{"requirements", bj::array{bj::array{"owner"}}}}}});
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/handbook", {{"accept", "text/html"}}).status, 303);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/handbook").status, 401);

  std::string err;
  std::string manifest = R"({"box":"jobs","domains":["*"],
      "templates": {"job": "<article>Job {{id}}: {{quantity}} {{note}}{{#user}}x{{/user}}</article>"},
      "routes":{"http":{"/jobs/{id : int64}":{"get":{"name":"job","template":"job","public":true}}}}})";
  ASSERT_TRUE(host->boxes().upload(manifest, kJobWat, &err)) << err;
  auto as_json = fetch(port, bhttp::verb::get, "/jobs/7", {{"accept", "application/json"}});
  EXPECT_EQ(as_json.headers["content-type"], "application/json");
  auto as_html = fetch(port, bhttp::verb::get, "/jobs/7", {{"accept", "text/html,application/json"}});
  EXPECT_EQ(as_html.headers["content-type"], "text/html; charset=utf-8");
  EXPECT_EQ(as_html.body, "<article>Job 7: 3 &lt;b&gt;hi&lt;/b&gt;</article>");
}

// Images up to 1 MiB from the attachments page, more with an operator's token; larger is
// refused with what the limit is, past the host's general 1 MiB body limit either way.
TEST_F(SiteRoutesTest, AttachmentUploadsTakeAFullMegabyte) {
  std::string png = png_of(1 << 20);  // exactly 1 MiB
  std::string body = "--XyZ\r\nContent-Disposition: form-data; name=\"alt\"\r\n\r\nA big one\r\n"
                     "--XyZ\r\nContent-Disposition: form-data; name=\"file\"; filename=\"big.png\"\r\n"
                     "Content-Type: image/png\r\n\r\n" + png + "\r\n--XyZ--\r\n";
  auto form = fetch(port, bhttp::verb::post, "/attachment/upload",
                    {{"authorization", "Bearer test-token-0123456789"}, {"content-type", "multipart/form-data; boundary=XyZ"}},
                    body);
  ASSERT_EQ(form.status, 201) << form.body.substr(0, 200);
  // An operator's token takes more, as attachment_put does.
  std::string bigger = png_of(3 << 20);
  auto raw = fetch(port, bhttp::verb::post, "/attachment/upload?name=b.png",
                   {{"authorization", "Bearer test-token-0123456789"}, {"content-type", "image/png"}}, bigger);
  EXPECT_EQ(raw.status, 201) << raw.body.substr(0, 200);
  // Past the operator's limit: refused, saying why.
  std::string huge = png_of((32 << 20) + 1);
  auto over = fetch(port, bhttp::verb::post, "/attachment/upload?name=h.png",
                    {{"authorization", "Bearer test-token-0123456789"}, {"content-type", "image/png"}}, huge);
  EXPECT_EQ(over.status, 413);
  // Without a token, a 1 MiB form is read and then needs a signed-in editor; far more isn't read.
  auto anon = fetch(port, bhttp::verb::post, "/attachment/upload", {{"content-type", "multipart/form-data; boundary=XyZ"}}, body);
  EXPECT_EQ(anon.status, 401);
  // The host answers 413 and closes before reading it, so the sender may see the close first.
  int flood = 0;
  try {
    flood = fetch(port, bhttp::verb::post, "/attachment/upload", {{"content-type", "image/png"}}, bigger).status;
  } catch (const std::exception&) {
    flood = 413;
  }
  EXPECT_EQ(flood, 413);
}

// An image already on the site becomes an attachment by its path, or by a URL on this host
// (what an agent tried with http://127.0.0.1:8080/hero-small.jpg), without a network fetch.
TEST_F(SiteRoutesTest, AttachmentsFromTheSitesOwnFiles) {
  tool("content_put", {{"tenant", "playground"}, {"path", "hero-small.jpg"}, {"base64", base::base64_encode(kPng)}});
  auto by_file = tool("attachment_put", {{"tenant", "playground"}, {"file", "/hero-small.jpg"}, {"alt", "The ranch"}});
  ASSERT_FALSE(by_file.as_object().contains("error")) << json::dump(by_file);
  EXPECT_EQ(by_file.at("name"), "hero-small.jpg");
  EXPECT_EQ(by_file.at("content_type"), "image/png");  // from the bytes, not the name
  auto by_url = tool("attachment_put", {{"tenant", "playground"}, {"url", "http://127.0.0.1:8080/hero-small.jpg"}});
  ASSERT_FALSE(by_url.as_object().contains("error")) << json::dump(by_url);
  auto by_domain = tool("attachment_put", {{"tenant", "playground"}, {"url", "https://localhost/hero-small.jpg?v=2"}});
  ASSERT_FALSE(by_domain.as_object().contains("error")) << json::dump(by_domain);
  auto missing = tool("attachment_put", {{"tenant", "playground"}, {"file", "/nope.jpg"}});
  EXPECT_NE(std::string(missing.at("error").as_string()).find("content_list"), std::string::npos);
  auto plain = tool("attachment_put", {{"tenant", "playground"}, {"url", "http://example.com/x.png"}});
  EXPECT_NE(std::string(plain.at("error").as_string()).find("give file"), std::string::npos);
  auto two = tool("attachment_put", {{"tenant", "playground"}, {"file", "/hero-small.jpg"}, {"base64", "AA=="}});
  EXPECT_NE(std::string(two.at("error").as_string()).find("exactly one"), std::string::npos);
}

// Big files don't travel as base64 in a tool call: an upload URL takes one PUT of the bytes.
TEST_F(SiteRoutesTest, UploadUrlsTakeOnePutAndCutOffBase64IsNamed) {
  auto ticket = tool("attachment_upload_url", {{"tenant", "playground"}, {"name", "hero.png"}, {"alt", "The ranch"}});
  ASSERT_FALSE(ticket.as_object().contains("error")) << json::dump(ticket);
  std::string url(ticket.at("upload_url").as_string());
  EXPECT_EQ(ticket.at("method"), "PUT");
  std::string target = url.substr(url.find("/attachment/upload"));
  std::string big = png_of(2 << 20);  // past the 1 MiB a browser gets, and no token needed
  auto put = fetch(port, bhttp::verb::put, target, {{"content-type", "application/octet-stream"}}, big);
  ASSERT_EQ(put.status, 201) << put.body.substr(0, 300);
  std::string err;
  auto a = json::parse(put.body, &err);
  EXPECT_EQ(a->at("name"), "hero.png");
  EXPECT_EQ(a->at("alt"), "The ranch");
  EXPECT_EQ(fetch(port, bhttp::verb::get, std::string(a->at("url").as_string())).body, big);
  // Once only.
  EXPECT_EQ(fetch(port, bhttp::verb::put, target, {}, kPng).status, 403);
  EXPECT_EQ(fetch(port, bhttp::verb::put, "/attachment/upload?ticket=0123abcd", {}, kPng).status, 403);

  // base64 cut short on its way: refused, saying so, nothing stored.
  std::string cut = base::base64_encode(png_of(300000)).substr(0, 190000);
  auto refused = tool("attachment_put", {{"tenant", "playground"}, {"base64", cut}});
  ASSERT_TRUE(refused.as_object().contains("error")) << json::dump(refused).substr(0, 200);
  EXPECT_NE(std::string(refused.at("error").as_string()).find("cut off"), std::string::npos) << json::dump(refused);
}

// Without code to PUT with, an agent sends the base64 in pieces against a ticket.
TEST_F(SiteRoutesTest, UploadsInPiecesForAgentsWithoutCode) {
  auto t = tool("attachment_upload_url", {{"tenant", "playground"}, {"name", "banner.png"}, {"alt", "Banner"}});
  std::string url(t.at("upload_url").as_string());
  std::string png = png_of(600000);
  std::string b64 = base::base64_encode(png);
  size_t step = 120000;  // a multiple of 4
  bj::value last;
  for (size_t at = 0; at < b64.size(); at += step) {
    bool done = at + step >= b64.size();
    last = tool("attachment_upload_part", {{"ticket", url}, {"base64", b64.substr(at, step)}, {"done", done}});
    ASSERT_FALSE(last.as_object().contains("error")) << json::dump(last);
    if (!done) {
      EXPECT_EQ(last.at("received_bytes").as_int64(), int64_t((at + step) / 4 * 3));
    }
  }
  EXPECT_EQ(last.at("name"), "banner.png");
  EXPECT_EQ(last.at("size").as_int64(), int64_t(png.size()));
  EXPECT_EQ(last.at("pieces").as_int64(), int64_t((b64.size() + step - 1) / step));
  EXPECT_EQ(fetch(port, bhttp::verb::get, std::string(last.at("url").as_string())).body, png);
  // The ticket is spent.
  auto spent = tool("attachment_upload_part", {{"ticket", url}, {"base64", "AAAA"}, {"done", true}});
  EXPECT_NE(std::string(spent.at("error").as_string()).find("unknown, used, or expired"), std::string::npos);
  // A piece that isn't a multiple of 4 is refused before anything is kept.
  auto t2 = tool("attachment_upload_url", {{"tenant", "playground"}});
  auto odd = tool("attachment_upload_part", {{"ticket", t2.at("upload_url")}, {"base64", "AAAAA"}});
  EXPECT_NE(std::string(odd.at("error").as_string()).find("multiple of 4"), std::string::npos);
  // attachment_put says how much base64 arrived, and refuses past its limit by name.
  auto small = tool("attachment_put", {{"tenant", "playground"}, {"base64", base::base64_encode(kPng)}});
  EXPECT_EQ(small.at("base64_chars_received").as_int64(), int64_t(base::base64_encode(kPng).size()));
}

// Engineer B's report, item by item.
TEST_F(SiteRoutesTest, DocumentsDraftsIndexesAndWhatPageGetFinds) {
  std::string doc = "<!DOCTYPE html><html><head><title>Future</title></head><body><p>Salt &amp; pepper</p></body></html>";
  // 1. A whole document can be a draft, and need a role.
  auto put = tool("page_put", {{"tenant", "playground"}, {"path", "/future-of-meat"}, {"format", "document"}, {"body", doc},
                               {"title", "The future of meat"},
                               {"fields", bj::object{{"draft", true}, {"requirements", bj::array{bj::array{"owner"}}}}}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/future-of-meat").status, 404);  // a draft, hidden
  auto preview = tool("page_render", {{"verbose", true}, {"tenant", "playground"}, {"path", "/future-of-meat"}, {"drafts", true}});
  EXPECT_EQ(preview.at("kind"), "document");
  std::string html(preview.at("body").as_string());
  EXPECT_EQ(html, doc);  // served as written: no front matter, no template
  auto refused = tool("page_put", {{"tenant", "playground"}, {"path", "/x"}, {"format", "document"}, {"body", doc},
                                   {"template", "page"}});
  EXPECT_NE(std::string(refused.at("error").as_string()).find("takes no template"), std::string::npos);

  // 2 and 4. A trailing slash writes the directory's index, and the old file for that URL goes,
  // so the render is the new body at once.
  auto index = tool("page_put", {{"tenant", "playground"}, {"path", "/future-of-meat/"}, {"format", "document"},
                                 {"body", "<!DOCTYPE html><p>v2</p>"}});
  EXPECT_EQ(index.at("file"), "future-of-meat/index.html");
  EXPECT_EQ(index.at("replaced").at(0), "future-of-meat.html");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/future-of-meat").body, "<!DOCTYPE html><p>v2</p>");

  // 3. page_get finds documents by URL, the root included.
  tool("page_put", {{"tenant", "playground"}, {"path", "/"}, {"format", "document"}, {"body", "<!DOCTYPE html><p>home</p>"}});
  EXPECT_EQ(tool("page_get", {{"tenant", "playground"}, {"path", "/"}}).at("file"), "index.html");
  EXPECT_EQ(tool("page_get", {{"tenant", "playground"}, {"path", "/future-of-meat"}}).at("file"), "future-of-meat/index.html");

  // 5. Bodies are kept byte for byte.
  tool("page_put", {{"tenant", "playground"}, {"path", "/salt"}, {"format", "fragment"},
                    {"body", "<style>.x{color:red}</style><p>Salt &amp; &quot;pepper&quot;</p>"}});
  EXPECT_EQ(tool("page_get", {{"tenant", "playground"}, {"path", "/salt"}}).at("body"),
            "<style>.x{color:red}</style><p>Salt &amp; &quot;pepper&quot;</p>");
  // 6. The description skips the style sheet, and the footer names the site by its domain.
  auto salt = fetch(port, bhttp::verb::get, "/salt").body;
  EXPECT_NE(salt.find("<meta name=\"description\" content=\"Salt &amp; &quot;pepper&quot;\">"), std::string::npos) << salt;
  EXPECT_EQ(salt.find("color:red} Salt"), std::string::npos);
  EXPECT_NE(salt.find("© " + base::format_utc_ms(base::now_unix_ms()).substr(0, 4) + " localhost"), std::string::npos);

  // Every entity survives a fragment's round trip: stored, read back, and served, never decoded
  // (an escaped <script> stays text).
  const std::string kEntities = "<p>&amp; &lt; &gt; &quot; &#39; &lt;script&gt;alert(1)&lt;/script&gt;</p>";
  auto put5 = tool("page_put", {{"tenant", "playground"}, {"path", "/entities"}, {"format", "fragment"}, {"body", kEntities}});
  ASSERT_FALSE(put5.as_object().contains("error")) << json::dump(put5);
  EXPECT_EQ(std::string(tool("page_get", {{"tenant", "playground"}, {"path", "/entities"}}).at("body").as_string()), kEntities);
  EXPECT_NE(std::string(tool("page_render", {{"verbose", true}, {"tenant", "playground"}, {"path", "/entities"}}).at("body").as_string()).find(kEntities),
            std::string::npos);
  auto served = fetch(port, bhttp::verb::get, "/entities").body;
  EXPECT_NE(served.find(kEntities), std::string::npos) << served;
  EXPECT_EQ(served.find("<script>alert"), std::string::npos);
}

// Shared partials (design/machine/027.Partials.md): one write updates a document, a box's
// template page, and a box's own HTML that opts in, with the right link active on each.
TEST_F(SiteRoutesTest, OnePartialWriteUpdatesEveryPage) {
  const std::string nav1 = R"(<nav><a href="/" data-match-exact="/">Home</a> <a href="/jobs" data-match="/jobs">Jobs</a></nav>)";
  auto put = tool("partial_put", {{"tenant", "playground"}, {"name", "nav"}, {"html", nav1}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  EXPECT_TRUE(put.at("previous_etag").is_null());
  tool("page_put", {{"tenant", "playground"}, {"path", "/"}, {"format", "document"},
                    {"body", "<!doctype html><body><!--#include nav--><h1>Home</h1></body>"}});
  std::string err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"jobs","domains":["*"],
      "templates": {"job": "<!--#include nav--><article>Job {{id}} {{note}}</article>"},
      "routes":{"http":{"/jobs/{id : int64}":{"get":{"name":"job","template":"job","public":true}},
                        "/jobs/{id : int64}/raw":{"get":{"name":"raw","public":true,"includes":true}},
                        "/jobs/{id : int64}/plain":{"get":{"name":"plain","public":true}}}}})",
                                   R"((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "text/html; charset=utf-8")
  (data (i32.const 64) "{\"id\":\"7\",\"note\":\"<!--#include nav-->\"}")
  (data (i32.const 128) "<!--#include nav--><p>raw</p>")
  (func (export "goober_route_job") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 64) (i32.const 39))) (i32.const 0))
  (func (export "goober_route_raw") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 16) (i32.const 24) (i32.const 128) (i32.const 29))) (i32.const 0))
  (func (export "goober_route_plain") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 16) (i32.const 24) (i32.const 128) (i32.const 29))) (i32.const 0))
))", &err))
      << err;

  auto home = fetch(port, bhttp::verb::get, "/");
  EXPECT_NE(home.body.find(R"(<a href="/" data-match-exact="/" aria-current="page" class="active">Home</a>)"), std::string::npos)
      << home.body;
  auto job = fetch(port, bhttp::verb::get, "/jobs/7", {{"accept", "text/html"}});
  EXPECT_NE(job.body.find(R"(data-match="/jobs" aria-current="page" class="active">Jobs</a>)"), std::string::npos) << job.body;
  // A value that looks like a marker is escaped, so it never pulls a partial in.
  EXPECT_NE(job.body.find("&lt;!--#include nav--&gt;"), std::string::npos) << job.body;
  auto raw = fetch(port, bhttp::verb::get, "/jobs/7/raw");
  EXPECT_NE(raw.body.find(R"(class="active">Jobs</a></nav><p>raw</p>)"), std::string::npos) << raw.body;
  // Without "includes", a box's own HTML is served as it wrote it.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/jobs/7/plain").body, "<!--#include nav--><p>raw</p>");

  // One write: every page has the new nav, and new ETags.
  const std::string nav2 = R"(<nav><a href="/" data-match-exact="/">Home</a> <a href="/jobs" data-match="/jobs">All jobs</a></nav>)";
  auto second = tool("partial_put", {{"tenant", "playground"}, {"name", "nav"}, {"html", nav2}});
  EXPECT_EQ(second.at("previous_etag"), put.at("etag"));
  auto home2 = fetch(port, bhttp::verb::get, "/");
  EXPECT_NE(home2.body.find("All jobs"), std::string::npos);
  EXPECT_NE(home2.headers["etag"], home.headers["etag"]);
  EXPECT_NE(fetch(port, bhttp::verb::get, "/jobs/7", {{"accept", "text/html"}}).body.find("All jobs"), std::string::npos);
  EXPECT_NE(fetch(port, bhttp::verb::get, "/jobs/7/raw").body.find("All jobs"), std::string::npos);

  // Listed with the pages that include it and a version to roll back to.
  auto list = tool("partial_list", {{"tenant", "playground"}});
  ASSERT_EQ(list.at("partials").as_array().size(), 1u) << json::dump(list);
  const auto& entry = list.at("partials").at(0);
  EXPECT_EQ(entry.at("name"), "nav");
  EXPECT_EQ(entry.at("included_by").at(0), "/");
  ASSERT_EQ(entry.at("versions").as_array().size(), 1u);
  auto old = tool("partial_get", {{"tenant", "playground"}, {"name", "nav"}, {"version", entry.at("versions").at(0)}});
  EXPECT_EQ(std::string(old.at("html").as_string()), nav1);
  // route_check names what each path includes.
  EXPECT_EQ(tool("route_check", {{"domain", "localhost"}, {"path", "/"}}).at("partials").at(0), "nav");
  EXPECT_EQ(tool("route_check", {{"domain", "localhost"}, {"path", "/jobs/7/raw"}}).at("route").at("includes"), true);

  // Deleted: markers serve empty with a header saying so; never a 500.
  EXPECT_EQ(tool("partial_delete", {{"tenant", "playground"}, {"name", "nav"}}).at("deleted"), "_partials/nav.html");
  auto gone = fetch(port, bhttp::verb::get, "/");
  EXPECT_EQ(gone.status, 200);
  EXPECT_EQ(gone.headers["x-goober-partial-missing"], "nav");
  EXPECT_EQ(gone.body, "<!doctype html><body><h1>Home</h1></body>");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/jobs/7/raw").headers["x-goober-partial-missing"], "nav");
  EXPECT_TRUE(tool("partial_put", {{"tenant", "playground"}, {"name", "Bad Name"}, {"html", "x"}}).as_object().contains("error"));
}

// Trying a blog without writing it (Engineer B, 2026-10-28): unsaved pages, a template, and the
// listing's feed render as if written, and nothing is stored.
TEST_F(SiteRoutesTest, PageRenderPreviewsUnsavedPagesTemplatesAndFeeds) {
  bj::array pages{
      bj::object{{"path", "/updates"}, {"format", "markdown"}, {"body", "News."},
                 {"fields", bj::object{{"list", "updates"}, {"feed", true}, {"title", "Updates"}}}},
      bj::object{{"path", "/updates/hello"}, {"format", "markdown"}, {"body", "Hello there."}, {"template", "post"},
                 {"fields", bj::object{{"title", "I'm back"}, {"date", "2026-10-29"}, {"tags", bj::array{"news"}},
                                       {"json_ld", bj::object{{"@type", "Article"}, {"headline", "I'm back"}}}}}}};
  bj::object templates{{"post", "<head>{{{json_ld}}}</head><article><h1>{{title}}</h1>{{{body}}}</article>"}};
  auto post = tool("page_render", {{"verbose", true}, {"tenant", "playground"}, {"path", "/updates/hello"}, {"pages", pages}, {"templates", templates}});
  ASSERT_EQ(post.at("status").as_int64(), 200) << json::dump(post);
  std::string html(post.at("body").as_string());
  EXPECT_NE(html.find("<h1>I&#39;m back</h1>"), std::string::npos) << html;
  EXPECT_NE(html.find(R"(<script type="application/ld+json">{"@type":"Article","headline":"I'm back"}</script>)"), std::string::npos)
      << html;
  EXPECT_EQ(post.at("stored"), false);

  auto list = tool("page_render", {{"verbose", true}, {"tenant", "playground"}, {"path", "/updates"}, {"pages", pages}, {"feed", true}});
  ASSERT_EQ(list.at("status").as_int64(), 200) << json::dump(list);
  EXPECT_NE(std::string(list.at("body").as_string()).find("/updates/hello"), std::string::npos);
  std::string feed(list.at("feed").at("body").as_string());
  EXPECT_NE(feed.find("<category term=\"news\"/>"), std::string::npos) << feed;
  EXPECT_NE(feed.find("<updated>2026-10-29"), std::string::npos) << feed;

  // Nothing was written.
  EXPECT_FALSE(host->content().get("playground", "updates/hello.md"));
  EXPECT_FALSE(host->content().get("playground", "_templates/post.html"));
  EXPECT_EQ(tool("page_render", {{"verbose", true}, {"tenant", "playground"}, {"path", "/updates/hello"}}).at("status").as_int64(), 404);
  // A template that doesn't parse is refused, as template_put would.
  EXPECT_TRUE(tool("page_render", {{"verbose", true}, {"tenant", "playground"}, {"path", "/"}, {"templates", bj::object{{"x", "{{#a}}"}}}})
                  .as_object()
                  .contains("error"));
}

TEST_F(SiteRoutesTest, TemplateGetGivesTheBuiltInsAndContentPutChecksBytes) {
  auto page = tool("template_get", {{"tenant", "playground"}, {"name", "list"}});
  EXPECT_EQ(page.at("source"), "built-in") << json::dump(page);
  EXPECT_NE(std::string(page.at("template").as_string()).find("{{#pages}}"), std::string::npos);

  std::string csv = "a,b\r\n1,2\r\n";
  std::string sha = base::sha256_hex(csv);
  auto put = tool("content_put", {{"tenant", "playground"}, {"path", "data/x.csv"}, {"text", csv}, {"sha256", sha}});
  EXPECT_EQ(put.at("bytes").as_int64(), 10) << json::dump(put);
  EXPECT_EQ(std::string(put.at("sha256").as_string()), sha);
  EXPECT_EQ(put.at("content_type"), "text/csv; charset=utf-8");
  // What a layer that drops CR would deliver: refused, saying so.
  auto bad = tool("content_put", {{"tenant", "playground"}, {"path", "data/y.csv"}, {"text", "a,b\n1,2\n"}, {"sha256", sha}});
  ASSERT_TRUE(bad.as_object().contains("error")) << json::dump(bad);
  EXPECT_NE(std::string(bad.at("error").as_string()).find("0 CR bytes"), std::string::npos);
  EXPECT_FALSE(host->content().get("playground", "data/y.csv"));
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/data/x.csv").headers["content-type"], "text/csv; charset=utf-8");
}

// The blog kcmeatup asked for (031 § 2): one page_put per post, with the built-in post template;
// the listing, feed, and sitemap follow with no other edits.
TEST_F(SiteRoutesTest, APostIsOnePagePutAndEverythingElseFollows) {
  std::string err;
  ASSERT_TRUE(host->content().put("playground", "_data/site.json",
                                  R"({"title":"KC Meat Up","description":"Meat in Kansas City.","author":"Jeffrey M. Barber"})", &err));
  // The index: a listing of every page under updates written with the post template, 2 a page.
  auto index = tool("page_put", {{"path", "/updates"}, {"format", "markdown"}, {"body", "News from the grill."},
                                 {"title", "Updates"},
                                 {"fields", bj::object{{"list", "updates"}, {"list-template", "post"}, {"per-page", 2}, {"feed", true}}}});
  ASSERT_FALSE(index.as_object().contains("error")) << json::dump(index);  // no tenant: the host has one
  for (int i = 1; i <= 3; ++i) {
    auto day = "2026-10-2" + std::to_string(i);
    auto p = tool("page_put", {{"path", "/updates/post-" + std::to_string(i)}, {"format", "markdown"},
                               {"template", "post"}, {"title", "Post number " + std::to_string(i)},
                               {"body", "Brisket *again*."},
                               {"fields", bj::object{{"date", day}, {"summary", "A summary of post " + std::to_string(i)},
                                                     {"tags", bj::array{"beef"}}, {"image", "/hero.jpg"}}}});
    ASSERT_FALSE(p.as_object().contains("error")) << json::dump(p);
  }
  tool("page_put", {{"path", "/updates/secret"}, {"format", "markdown"}, {"template", "post"}, {"title", "Not yet"},
                    {"body", "Soon."}, {"fields", bj::object{{"date", "2026-10-29"}, {"draft", true}}}});
  tool("page_put", {{"path", "/updates/about-us"}, {"format", "markdown"}, {"title", "Not a post"}, {"body", "Hi."}});

  auto post = fetch(port, bhttp::verb::get, "/updates/post-3", {{"accept", "text/html"}});
  ASSERT_EQ(post.status, 200) << post.body;
  EXPECT_NE(post.body.find("By Jeffrey M. Barber"), std::string::npos) << post.body;
  EXPECT_NE(post.body.find("<meta name=\"description\" content=\"A summary of post 3\">"), std::string::npos);
  EXPECT_NE(post.body.find("<meta property=\"og:type\" content=\"article\">"), std::string::npos);
  EXPECT_NE(post.body.find("<meta property=\"og:image\" content=\"http://localhost/hero.jpg\">"), std::string::npos);
  EXPECT_NE(post.body.find("<link rel=\"canonical\" href=\"http://localhost/updates/post-3\">"), std::string::npos);
  size_t ld = post.body.find("<script type=\"application/ld+json\">");
  ASSERT_NE(ld, std::string::npos);
  EXPECT_EQ(post.body.find("application/ld+json", ld + 40), std::string::npos);  // exactly one
  std::string script = post.body.substr(post.body.find('>', ld) + 1);
  script = script.substr(0, script.find("</script>"));
  auto parsed = json::parse(script, &err);
  ASSERT_TRUE(parsed) << script;
  EXPECT_EQ(parsed->at("@type"), "BlogPosting");
  EXPECT_EQ(parsed->at("author").at("name"), "Jeffrey M. Barber");
  EXPECT_EQ(parsed->at("datePublished"), "2026-10-23");

  // The listing: newest first, posts only, drafts hidden, 2 a page with prev/next and canonical.
  auto list = fetch(port, bhttp::verb::get, "/updates", {{"accept", "text/html"}});
  ASSERT_EQ(list.status, 200);
  EXPECT_LT(list.body.find("Post number 3"), list.body.find("Post number 2"));
  EXPECT_EQ(list.body.find("Post number 1"), std::string::npos);  // page 2
  EXPECT_EQ(list.body.find("Not yet"), std::string::npos);
  EXPECT_EQ(list.body.find("Not a post"), std::string::npos);
  EXPECT_NE(list.body.find("<link rel=\"next\" href=\"/updates?page=2\">"), std::string::npos) << list.body;
  auto page2 = fetch(port, bhttp::verb::get, "/updates?page=2", {{"accept", "text/html"}});
  EXPECT_NE(page2.body.find("Post number 1"), std::string::npos);
  EXPECT_NE(page2.body.find("<link rel=\"canonical\" href=\"http://localhost/updates?page=2\">"), std::string::npos);
  EXPECT_NE(page2.body.find("<link rel=\"prev\" href=\"/updates\">"), std::string::npos);
  // The feed and the sitemap and llms.txt follow, from the same front matter.
  auto feed = fetch(port, bhttp::verb::get, "/updates/feed.xml");
  EXPECT_NE(feed.body.find("Post number 3"), std::string::npos);
  EXPECT_NE(feed.body.find("<author><name>Jeffrey M. Barber</name></author>"), std::string::npos) << feed.body;
  // RSS 2.0 beside the Atom, both advertised in the head; every listing has them unless feed: false.
  auto rss = fetch(port, bhttp::verb::get, "/updates/rss.xml");
  EXPECT_EQ(rss.status, 200);
  EXPECT_NE(rss.headers["content-type"].find("application/rss+xml"), std::string::npos);
  EXPECT_NE(rss.body.find("<pubDate>Fri, 23 Oct 2026 00:00:00 +0000</pubDate>"), std::string::npos) << rss.body;
  EXPECT_NE(rss.body.find("<dc:creator>Jeffrey M. Barber</dc:creator>"), std::string::npos);
  EXPECT_NE(list.body.find("type=\"application/rss+xml\" title=\"Updates\" href=\"/updates/rss.xml\""), std::string::npos);
  tool("page_put", {{"path", "/plain"}, {"format", "markdown"}, {"body", "x"}, {"fields", bj::object{{"list", "updates"}}}});
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/plain/feed.xml").status, 200);
  tool("page_put", {{"path", "/quiet"}, {"format", "markdown"}, {"body", "x"},
                    {"fields", bj::object{{"list", "updates"}, {"feed", false}}}});
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/quiet/rss.xml").status, 404);
  auto sitemap = fetch(port, bhttp::verb::get, "/sitemap.xml");
  EXPECT_EQ(sitemap.status, 200);
  EXPECT_NE(sitemap.headers["content-type"].find("application/xml"), std::string::npos);
  EXPECT_NE(sitemap.body.find("<loc>http://localhost/updates/post-3</loc><lastmod>2026-10-23</lastmod>"), std::string::npos)
      << sitemap.body;
  EXPECT_EQ(sitemap.body.find("secret"), std::string::npos);
  auto llms = fetch(port, bhttp::verb::get, "/llms.txt");
  EXPECT_NE(llms.body.find("# KC Meat Up\n\n> Meat in Kansas City."), std::string::npos) << llms.body;
  EXPECT_NE(llms.body.find("- [Post number 3](http://localhost/updates/post-3): A summary of post 3"), std::string::npos);
  // A drafts preview still renders the draft.
  auto preview = tool("page_render", {{"path", "/updates/secret"}, {"drafts", true}});
  EXPECT_EQ(preview.at("status").as_int64(), 200);
  EXPECT_FALSE(preview.as_object().contains("body"));  // compact unless verbose
  EXPECT_TRUE(preview.at("problems").as_array().empty()) << json::dump(preview);
  // A box's sitemap joins by name: /sitemap.xml becomes an index.
  ASSERT_TRUE(host->content().put("playground", "_data/site.json",
                                  R"({"title":"KC Meat Up","sitemaps":["/places/sitemap.xml"]})", &err));
  auto index_map = fetch(port, bhttp::verb::get, "/sitemap.xml").body;
  EXPECT_NE(index_map.find("<sitemapindex"), std::string::npos) << index_map;
  EXPECT_NE(index_map.find("<loc>http://localhost/places/sitemap.xml</loc>"), std::string::npos);
  EXPECT_NE(fetch(port, bhttp::verb::get, "/sitemap-pages.xml").body.find("/updates/post-3"), std::string::npos);
  // A tenant's own file wins over the generated sitemap.
  ASSERT_TRUE(host->content().put("playground", "sitemap.xml", "<urlset/>", &err));
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/sitemap.xml").body, "<urlset/>");
}

TEST_F(SiteRoutesTest, EtagsGuardWritesAndPathFiltersPageList) {
  auto first = tool("page_put", {{"path", "/updates/a"}, {"format", "markdown"}, {"body", "One."}});
  tool("page_put", {{"path", "/about"}, {"format", "markdown"}, {"body", "Us."}});
  std::string etag(first.at("etag").as_string());
  auto got = tool("page_get", {{"path", "/updates/a"}});
  EXPECT_EQ(std::string(got.at("etag").as_string()), etag);
  auto stale = tool("page_put", {{"path", "/updates/a"}, {"format", "markdown"}, {"body", "Two."}, {"if_etag", "\"nope\""}});
  EXPECT_NE(std::string(stale.at("error").as_string()).find("nothing written"), std::string::npos);
  auto fresh = tool("page_put", {{"path", "/updates/a"}, {"format", "markdown"}, {"body", "Two."}, {"if_etag", etag}});
  ASSERT_FALSE(fresh.as_object().contains("error")) << json::dump(fresh);
  EXPECT_NE(std::string(fresh.at("etag").as_string()), etag);
  EXPECT_TRUE(tool("page_put", {{"path", "/updates/b"}, {"format", "markdown"}, {"body", "B."}, {"if_etag", "none"}})
                  .as_object().contains("file"));
  EXPECT_TRUE(tool("page_put", {{"path", "/updates/b"}, {"format", "markdown"}, {"body", "B2."}, {"if_etag", "none"}})
                  .as_object().contains("error"));
  // path is page_list's old name for dir; an argument nobody knows is named, not silently dropped.
  auto listed = tool("page_list", {{"path", "updates"}});
  ASSERT_TRUE(listed.is_array()) << json::dump(listed);
  EXPECT_EQ(listed.as_array().size(), 2u);
  auto odd = tool("page_get", {{"path", "/about"}, {"colour", "red"}});
  EXPECT_EQ(odd.at("ignored_arguments").at(0), "colour");
  auto etags = tool("list_etags", {{"paths", bj::array{"/updates/a", "/nowhere"}}});
  EXPECT_EQ(etags.at("etags").at("/updates/a").at("file"), "updates/a.md");
  EXPECT_TRUE(etags.at("etags").at("/nowhere").is_null());
  auto schemas = tool("tool_schemas", {});
  bool found = false;
  for (const auto& t : schemas.at("tools").as_array()) {
    if (t.at("tool") == "page_list") found = t.at("aliases").at("path") == "dir";
  }
  EXPECT_TRUE(found);
}

TEST_F(SiteRoutesTest, ContentDeployChecksBeforeAndAfter) {
  std::string body = "# Hello\n\nA [link](https://example.com).";
  std::string sha = base::sha256_hex(body);
  // A wrong sha (bytes changed on the way) writes nothing, not even the good items.
  auto bad = tool("content_deploy", {{"pages", bj::array{bj::object{{"path", "/one"}, {"format", "markdown"}, {"body", body}, {"sha256", sha}},
                                                         bj::object{{"path", "/two"}, {"format", "markdown"}, {"body", body}, {"sha256", "00"}}}}});
  EXPECT_FALSE(bad.at("ok").as_bool()) << json::dump(bad);
  EXPECT_FALSE(host->content().get("playground", "one.md"));
  auto good = tool("content_deploy", {{"pages", bj::array{bj::object{{"path", "/one"}, {"format", "markdown"}, {"body", body}, {"sha256", sha},
                                                                     {"if_etag", "none"}}}},
                                      {"files", bj::array{bj::object{{"path", "data/macros.csv"}, {"text", "a,b\r\n1,2\r\n"}}}}});
  ASSERT_TRUE(good.at("ok").as_bool()) << json::dump(good);
  const auto& lines = good.at("lines").as_array();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_TRUE(std::string(lines[0].as_string()).starts_with("✓ /one 200 text/html")) << lines[0];
  EXPECT_TRUE(std::string(lines[1].as_string()).starts_with("✓ /data/macros.csv 200 text/csv")) << lines[1];
  // A page whose markdown leaves {{ in the HTML is written but flagged.
  auto flagged = tool("content_deploy", {{"pages", bj::array{bj::object{{"path", "/three"}, {"format", "fragment"},
                                                                        {"body", "<p>{{oops}}</p><p></p>"}}}}});
  EXPECT_FALSE(flagged.at("ok").as_bool());
  std::string line(flagged.at("lines").at(0).as_string());
  EXPECT_NE(line.find("unrendered {{"), std::string::npos) << line;
  EXPECT_NE(line.find("empty <p>"), std::string::npos) << line;
  // url_check and page_lint read what visitors get.
  auto checked = tool("url_check", {{"urls", bj::array{"http://localhost/one", "https://example.com/"}}});
  EXPECT_EQ(checked.at("results").at(0).at("status").as_int64(), 200);
  EXPECT_TRUE(checked.at("results").at(1).as_object().contains("error"));  // not ours
  auto lint = tool("page_lint", {{"strict", true}, {"paths", bj::array{"/one"}}});
  ASSERT_EQ(lint.at("pages").as_array().size(), 1u);
  EXPECT_NE(json::dump(lint).find("title is"), std::string::npos);
  // A path that would name another host after the port is no path (it was an SSRF).
  auto ssrf = tool("page_lint", {{"paths", bj::array{"@10.0.0.5:8500/v1/agent/self", "//evil.example/x"}}});
  std::string dumped = json::dump(ssrf);
  EXPECT_EQ(dumped.find("\"status\":"), std::string::npos) << dumped;
  EXPECT_NE(dumped.find("a path on the site"), std::string::npos) << dumped;
}

// Engineer B's case: a child template adds to <head>, JSON-LD built in a template survives
// quotes, feeds carry each entry's image, and a template can be deleted.
TEST_F(SiteRoutesTest, ChildTemplatesFillTheHeadAndJsonIsSafeInScripts) {
  auto t = tool("template_put", {{"tenant", "playground"}, {"name", "story"},
                                 {"template", "---\nlayout: base\n---\n<!--head--><meta property=\"x:test\" content=\"{{title}}\">"
                                              "<script type=\"application/ld+json\">{\"headline\": {{{json.title}}}}</script><!--/head-->"
                                              "<article>{{{body}}}</article>"}});
  ASSERT_FALSE(t.as_object().contains("error")) << json::dump(t);
  tool("page_put", {{"path", "/s"}, {"format", "markdown"}, {"template", "story"}, {"title", "Meat \"the\" Team </script>"},
                    {"body", "Hi."}, {"fields", bj::object{{"image", "/hero.jpg"}, {"date", "2026-10-29"}}}});
  auto page = fetch(port, bhttp::verb::get, "/s", {{"accept", "text/html"}});
  ASSERT_EQ(page.status, 200) << page.body;
  size_t head_end = page.body.find("</head>");
  size_t meta = page.body.find("property=\"x:test\"");
  ASSERT_NE(meta, std::string::npos);
  EXPECT_LT(meta, head_end);  // in <head>, not after <main>
  EXPECT_NE(page.body.find("{\"headline\": \"Meat \\\"the\\\" Team \\u003c/script\\u003e\"}"), std::string::npos) << page.body;
  EXPECT_EQ(page.body.find("<!--head-->"), std::string::npos);
  // A page's front matter can't set the trusted slots.
  tool("page_put", {{"path", "/evil"}, {"format", "markdown"}, {"body", "x"},
                    {"fields", bj::object{{"head", "<script>alert(1)</script>"}, {"json", bj::object{{"title", "<b>"}}}}}});
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/evil", {{"accept", "text/html"}}).body.find("<script>alert(1)"), std::string::npos);
  // Feeds carry the image.
  tool("page_put", {{"path", "/stories"}, {"format", "markdown"}, {"body", "x"}, {"fields", bj::object{{"list", ""}, {"list-template", "story"}}}});
  auto atom = fetch(port, bhttp::verb::get, "/stories/feed.xml");
  EXPECT_NE(atom.body.find(R"(<link rel="enclosure" href="http://localhost/hero.jpg"/>)"), std::string::npos) << atom.body;
  auto rss = fetch(port, bhttp::verb::get, "/stories/rss.xml");
  EXPECT_NE(rss.body.find(R"(<media:content url="http://localhost/hero.jpg" medium="image"/>)"), std::string::npos) << rss.body;
  // template_delete.
  EXPECT_EQ(tool("template_delete", {{"name", "story"}}).at("deleted"), "_templates/story.html");
  EXPECT_TRUE(tool("template_delete", {{"name", "story"}}).as_object().contains("error"));
}

// A box page in the site's chrome (a site's engineers, 2026-10-30): a box template whose front
// matter names a layout renders into the tenant's base, with the box's title, a head block of
// its own, and the box's values escaped; a signed-out request to a route with requirements is
// refused as before, never wrapped.
TEST_F(SiteRoutesTest, BoxTemplatesRenderIntoTheTenantsLayout) {
  auto put = tool("template_put", {{"tenant", "playground"}, {"name", "base"},
                                   {"template", "<!doctype html><title>{{title}}</title>{{{head}}}<header class=\"site\">Brand</header>"
                                            "<main>{{{body}}}</main><footer class=\"site\">Foot</footer>"}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  std::string err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"acct","domains":["*"],
      "templates": {"account": "---\nlayout: base\n---\n<!--head--><meta name=\"robots\" content=\"noindex\"><!--/head--><h1>{{title}}</h1><p>{{note}}</p>"},
      "routes":{"http":{"/account":{"get":{"name":"account","template":"account","public":true}},
                        "/account/private":{"get":{"name":"account_private","template":"account","requirements":[["owner"]]}}}}})",
                                   R"((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 64) "{\"title\":\"Your account\",\"note\":\"<b>hi</b>\",\"body\":\"<script>x</script>\",\"head\":\"<script>y</script>\"}")
  (func (export "goober_route_account") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 64) (i32.const 99))) (i32.const 0))
  (func (export "goober_route_account_private") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 64) (i32.const 99))) (i32.const 0))
))", &err))
      << err;
  auto page = fetch(port, bhttp::verb::get, "/account", {{"accept", "text/html"}});
  EXPECT_EQ(page.status, 200);
  EXPECT_EQ(page.body,
            "<!doctype html><title>Your account</title><meta name=\"robots\" content=\"noindex\">"
            "<header class=\"site\">Brand</header><main><h1>Your account</h1><p>&lt;b&gt;hi&lt;/b&gt;</p></main>"
            "<footer class=\"site\">Foot</footer>")
      << page.body;
  // API callers still get the JSON.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/account", {{"accept", "application/json"}}).headers["content-type"],
            "application/json");
  // Signed out, the gate answers first: a redirect to sign in, or a 401, with none of the page.
  auto gated = fetch(port, bhttp::verb::get, "/account/private");
  EXPECT_EQ(gated.status, 401);
  EXPECT_EQ(gated.body.find("Brand"), std::string::npos);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/account/private", {{"accept", "text/html"}}).status, 303);

  // Previewed without running the box: by path, with an unsaved base layout tried first.
  auto r = tool("box_render", {{"tenant", "playground"}, {"path", "/account/private"},
                               {"data", bj::object{{"title", "Preview"}, {"note", "n"}}},
                               {"templates", bj::object{{"base", "<html><title>{{title}}</title>{{{head}}}<header>New</header>{{{body}}}</html>"}}},
                               {"verbose", true}});
  ASSERT_FALSE(r.as_object().contains("error")) << json::dump(r);
  EXPECT_EQ(r.at("status"), 200);
  EXPECT_EQ(r.at("route").at("route"), "account_private");
  EXPECT_EQ(r.at("stored"), false);
  EXPECT_EQ(std::string(r.at("body").as_string()),
            "<html><title>Preview</title><meta name=\"robots\" content=\"noindex\"><header>New</header><h1>Preview</h1><p>n</p></html>");
  // By box and template, against the saved base.
  r = tool("box_render", {{"tenant", "playground"}, {"box", "acct"}, {"template", "account"},
                          {"data", bj::object{{"title", "T"}}}, {"verbose", true}});
  EXPECT_NE(std::string(r.at("body").as_string()).find("<header class=\"site\">Brand</header>"), std::string::npos) << json::dump(r);
  EXPECT_TRUE(tool("box_render", {{"tenant", "playground"}, {"path", "/nowhere"}, {"data", bj::object{}}}).as_object().contains("error"));
  // page_render names the box route instead of a bare 404.
  r = tool("page_render", {{"tenant", "playground"}, {"path", "/account"}});
  EXPECT_EQ(r.at("status"), 404);
  EXPECT_EQ(r.at("box_route").at("box"), "acct");

  // version_upload's dry run checks everything and stores nothing.
  auto before = tool("version_list", {{"box", "acct"}});
  auto dry = tool("version_upload", {{"manifest", R"({"box":"acct","domains":["*"],"routes":{"http":{"/account":{"get":{"name":"account","public":true}}}}})"},
                                     {"wat", "(module (memory (export \"memory\") 1) (func (export \"goober_route_account\") (result i32) (i32.const 0)))"},
                                     {"dry_run", true}});
  EXPECT_EQ(dry.at("dry_run"), true) << json::dump(dry);
  EXPECT_EQ(json::dump(tool("version_list", {{"box", "acct"}})), json::dump(before));
  auto bad = tool("version_upload", {{"manifest", R"({"box":"acct","domains":["*"],"routes":{"http":{"/account":{"get":{"name":"account","public":true,"layout":"base"}}}}})"},
                                     {"wat", "(module (memory (export \"memory\") 1) (func (export \"goober_route_account\") (result i32) (i32.const 0)))"},
                                     {"dry_run", true}});
  EXPECT_TRUE(bad.as_object().contains("error")) << json::dump(bad);
}

// A site's engineers' asks (2026-10-30): every tool says which etag it gives (the stored file's,
// for if_etag, or the served body's), templates take if_etag like pages, and writes answer
// with the change ids they made.
TEST_F(SiteRoutesTest, EtagsAreLabelledTemplatesGuardWritesAndWritesNameTheirChanges) {
  auto put = tool("page_put", {{"path", "/health/anxiety"}, {"format", "markdown"}, {"body", "# Calm\n\nBreathe."}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  std::string file_etag(put.at("file_etag").as_string());
  EXPECT_EQ(put.at("etag"), put.at("file_etag"));
  ASSERT_TRUE(put.as_object().contains("change_id")) << json::dump(put);
  auto change = tool("change_get", {{"id", put.at("change_id")}});
  EXPECT_EQ(change.at("target"), "health/anxiety.md") << json::dump(change);
  EXPECT_EQ(tool("page_get", {{"path", "/health/anxiety"}}).at("file_etag").as_string(), file_etag);
  EXPECT_EQ(tool("list_etags", {{"paths", bj::array{"/health/anxiety"}}}).at("etags").at("/health/anxiety").at("file_etag").as_string(),
            file_etag);
  // What visitors get has the body's etag; both tools that look at it say so, beside the file's.
  auto served = fetch(port, bhttp::verb::get, "/health/anxiety", {{"accept", "text/html"}});
  auto render = tool("page_render", {{"tenant", "playground"}, {"path", "/health/anxiety"}});
  EXPECT_EQ(render.at("file_etag").as_string(), file_etag);
  EXPECT_EQ(render.at("body_etag"), render.at("etag"));
  auto checked = tool("url_check", {{"urls", bj::array{"http://localhost/health/anxiety"}}}).at("results").at(0);
  EXPECT_EQ(checked.at("body_etag").as_string(), served.headers["etag"]);
  EXPECT_EQ(checked.at("file_etag").as_string(), file_etag);
  EXPECT_NE(checked.at("body_etag").as_string(), file_etag);
  // content_deploy says both, per item, and its lines name them.
  auto deployed = tool("content_deploy", {{"pages", bj::array{bj::object{{"path", "/health/sleep"}, {"format", "markdown"}, {"body", "Sleep."}}}}});
  ASSERT_TRUE(deployed.at("ok").as_bool()) << json::dump(deployed);
  EXPECT_TRUE(deployed.at("items").at(0).as_object().contains("file_etag"));
  EXPECT_TRUE(deployed.at("items").at(0).as_object().contains("body_etag"));
  EXPECT_NE(std::string(deployed.at("lines").at(0).as_string()).find("file_etag"), std::string::npos);
  ASSERT_TRUE(deployed.as_object().contains("change_ids")) << json::dump(deployed);

  // Templates: an etag to read, and if_etag on writes, "none" for a new one.
  auto got = tool("template_get", {{"tenant", "playground"}, {"name", "base"}});
  EXPECT_EQ(got.at("file_etag"), "none") << json::dump(got);
  auto first = tool("template_put", {{"tenant", "playground"}, {"name", "base"}, {"template", "<main>{{{body}}}</main>"},
                                     {"if_etag", "none"}});
  ASSERT_FALSE(first.as_object().contains("error")) << json::dump(first);
  std::string t_etag(first.at("file_etag").as_string());
  EXPECT_TRUE(first.as_object().contains("change_id")) << json::dump(first);
  EXPECT_EQ(tool("template_get", {{"tenant", "playground"}, {"name", "base"}}).at("file_etag").as_string(), t_etag);
  EXPECT_EQ(tool("template_list", {{"tenant", "playground"}}).at("file_etags").at("base").as_string(), t_etag);
  EXPECT_EQ(tool("list_etags", {{"paths", bj::array{"_templates/base.html"}}}).at("etags").at("_templates/base.html").at("file_etag").as_string(),
            t_etag);
  // A teammate writes it meanwhile: the stale write is refused, nothing written.
  tool("template_put", {{"tenant", "playground"}, {"name", "base"}, {"template", "<div>{{{body}}}</div>"}, {"if_etag", t_etag}});
  auto stale = tool("template_put", {{"tenant", "playground"}, {"name", "base"}, {"template", "<p>{{{body}}}</p>"}, {"if_etag", t_etag}});
  ASSERT_TRUE(stale.as_object().contains("error"));
  EXPECT_NE(std::string(stale.at("error").as_string()).find("nothing written"), std::string::npos);
  EXPECT_EQ(tool("template_get", {{"tenant", "playground"}, {"name", "base"}}).at("template"), "<div>{{{body}}}</div>");
  EXPECT_TRUE(tool("template_put", {{"tenant", "playground"}, {"name", "base"}, {"template", "x"}, {"if_etag", "none"}})
                  .as_object().contains("error"));
  EXPECT_TRUE(tool("template_delete", {{"tenant", "playground"}, {"name", "base"}, {"if_etag", t_etag}}).as_object().contains("error"));
  auto now = tool("template_get", {{"tenant", "playground"}, {"name", "base"}}).at("file_etag");
  EXPECT_EQ(tool("template_delete", {{"tenant", "playground"}, {"name", "base"}, {"if_etag", now}}).at("deleted"), "_templates/base.html");
  // A read changes nothing, so it names no change.
  EXPECT_FALSE(tool("page_get", {{"path", "/health/anxiety"}}).as_object().contains("change_id"));
}

// A templated box route negotiates on Accept, so every answer from it says Vary: Accept,
// merged with the box's own Vary into one header, HTML or JSON, 304 included (Engineer A).
TEST_F(SiteRoutesTest, TemplatedBoxRoutesVaryOnAcceptWhicheverTheyServe) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"brands","domains":["*"],
      "templates": {"brand": "<h1>{{name}}</h1>"},
      "routes":{"http":{"/brands/rye":{"get":{"name":"brand","template":"brand","public":true}},
                        "/brands/plain":{"get":{"name":"brand_plain","public":true}}}}})",
                                   R"((module
  (import "goober" "goober_header_set" (func $header (param i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "Vary")
  (data (i32.const 48) "Cookie, ACCEPT")
  (data (i32.const 64) "{\"name\":\"Rye\"}")
  (func (export "goober_route_brand") (result i32)
    (drop (call $header (i32.const 32) (i32.const 4) (i32.const 48) (i32.const 14)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 64) (i32.const 14))) (i32.const 0))
  (func (export "goober_route_brand_plain") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 64) (i32.const 14))) (i32.const 0))
))", &err))
      << err;
  auto json_answer = fetch(port, bhttp::verb::get, "/brands/rye");
  EXPECT_EQ(json_answer.headers["content-type"], "application/json");
  EXPECT_EQ(json_answer.headers["vary"], "cookie, accept");
  auto html_answer = fetch(port, bhttp::verb::get, "/brands/rye", {{"accept", "text/html"}});
  EXPECT_EQ(html_answer.headers["content-type"], "text/html; charset=utf-8");
  EXPECT_EQ(html_answer.headers["vary"], "cookie, accept");
  // The conditional answer varies too.
  auto again = fetch(port, bhttp::verb::get, "/brands/rye", {{"if-none-match", json_answer.headers["etag"]}});
  EXPECT_EQ(again.status, 304);
  EXPECT_EQ(again.headers["vary"], "cookie, accept");
  // A route without a template negotiates nothing, and adds nothing.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/brands/plain").headers.count("vary"), 0u);
}
