// End to end: pages, templates, and attachments over HTTP and the operator MCP tools
// (design/machine/007.Content.md).

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/encoding.h"
#include "goober/base/time.h"
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

  auto preview = tool("page_render", {{"tenant", "playground"}, {"path", "/blog"}});
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
  EXPECT_EQ(a->at("url"), "/attachment/1.png");
  auto img = fetch(port, bhttp::verb::get, "/attachment/1.png");
  EXPECT_EQ(img.status, 200);
  EXPECT_EQ(img.body, kPng);
  EXPECT_EQ(img.headers["content-type"], "image/png");
  EXPECT_NE(img.headers["cache-control"].find("immutable"), std::string::npos);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/attachment/1.jpg").status, 404);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/attachment/2.png").status, 404);

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
  auto svg = fetch(port, bhttp::verb::get, "/attachment/2.svg");
  EXPECT_EQ(svg.headers["content-disposition"].substr(0, 10), "attachment");
  EXPECT_NE(svg.headers["content-security-policy"].find("sandbox"), std::string::npos);

  // Over MCP: base64 in, and the list knows what uses each one.
  auto put = tool("attachment_put", {{"tenant", "playground"}, {"base64", base::base64_encode(kPng)}, {"alt", "dog"}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  EXPECT_EQ(put.at("url"), "/attachment/3.png");
  EXPECT_EQ(put.at("markdown"), "![dog](/attachment/3.png)");
  tool("page_put", {{"tenant", "playground"}, {"path", "/pets"}, {"format", "markdown"}, {"body", "![dog](/attachment/3.png)"}});
  auto unused = tool("attachment_list", {{"tenant", "playground"}, {"unused", true}});
  EXPECT_EQ(unused.as_array().size(), 2u);
  auto refused = tool("attachment_delete", {{"tenant", "playground"}, {"id", 3}});
  EXPECT_NE(std::string(refused.at("error").as_string()).find("pets.md"), std::string::npos);
  EXPECT_EQ(tool("attachment_delete", {{"tenant", "playground"}, {"id", 3}, {"force", true}}).at("deleted"), 3);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/attachment/3.png").status, 404);
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
  auto preview = tool("page_render", {{"tenant", "playground"}, {"path", "/future-of-meat"}, {"drafts", true}});
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
}
