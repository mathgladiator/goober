#include <gtest/gtest.h>

#include "goober/site/site.h"

using namespace goober;
namespace fs = std::filesystem;

class SiteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "site";
    fs::remove_all(root);
    w = std::make_unique<wal::HostWal>(root);
    store = std::make_unique<content::ContentStore>(*w);
    s = std::make_unique<site::Site>(*store);
  }
  void put(const std::string& path, const std::string& data) {
    std::string err;
    ASSERT_TRUE(store->put("t", path, data, &err)) << err;
  }
  site::Result get(const std::string& path, std::map<std::string, std::string> query = {}, std::string accept = "") {
    site::Visit v;
    v.tenant = "t";
    v.path = path;
    v.query = std::move(query);
    v.accept = std::move(accept);
    v.origin = "https://example.com";
    v.authorize = [this](const boost::json::value&) { return allow; };
    return s->visit(v);
  }
  fs::path root;
  std::unique_ptr<wal::HostWal> w;
  std::unique_ptr<content::ContentStore> store;
  std::unique_ptr<site::Site> s;
  int allow = 200;
};

TEST(SitePaths, UrlsAndPrivacy) {
  EXPECT_EQ(*site::page_url("index.md"), "/");
  EXPECT_EQ(*site::page_url("blog/index.html"), "/blog");
  EXPECT_EQ(*site::page_url("blog/hello.md"), "/blog/hello");
  EXPECT_FALSE(site::page_url("css/site.css"));
  EXPECT_TRUE(site::is_private_path("_templates/page.html"));
  EXPECT_TRUE(site::is_private_path("a/_b/c"));
  EXPECT_FALSE(site::is_private_path("a/b_c"));
  // Generator output directories are served; anything private inside them still isn't.
  EXPECT_FALSE(site::is_private_path("_astro/index.abc123.css"));
  EXPECT_FALSE(site::is_private_path("_next/static/chunks/main.js"));
  EXPECT_TRUE(site::is_private_path("_next/_secret/x"));
  EXPECT_TRUE(site::is_private_path("_astro"));
  EXPECT_TRUE(site::is_private_path("_nextish/x"));
  EXPECT_EQ(site::kind_of("a.html", "---\ntitle: x\n---\n"), site::Kind::kFragment);
  EXPECT_EQ(site::kind_of("a.html", "<!doctype html>"), site::Kind::kDocument);
}

TEST_F(SiteTest, MarkdownPagesUseTheBuiltInLayout) {
  put("about.md", "---\ntitle: About us\ndate: 2026-10-26\n---\nWe **fix** things.\n");
  auto r = get("/about");
  ASSERT_EQ(r.status, 200) << r.error;
  EXPECT_EQ(r.kind, site::Kind::kMarkdown);
  EXPECT_NE(r.body.find("<title>About us</title>"), std::string::npos);
  EXPECT_NE(r.body.find("<h1>About us</h1>"), std::string::npos);
  EXPECT_NE(r.body.find("<p>We <strong>fix</strong> things.</p>"), std::string::npos);
  EXPECT_NE(r.body.find("2026-10-26"), std::string::npos);
  bool csp = false;
  for (auto& [k, v] : r.headers) csp = csp || (k == "content-security-policy" && v.find("script-src 'self'") != std::string::npos);
  EXPECT_TRUE(csp);
  // The .md path redirects to the clean URL, and an agent can read the source.
  EXPECT_EQ(get("/about.md").status, 301);
  auto src = get("/about", {}, "text/markdown");
  EXPECT_EQ(src.kind, site::Kind::kSource);
  EXPECT_EQ(src.body.substr(0, 4), "---\n");
}

TEST_F(SiteTest, DocumentsAreServedAsWrittenAndFragmentsAreWrapped) {
  put("full.html", "<!doctype html><title>Mine</title><script>x()</script>");
  put("frag.html", "---\ntitle: Frag\n---\n<section>hi {{not a tag}}</section>");
  auto doc = get("/full");
  EXPECT_EQ(doc.kind, site::Kind::kDocument);
  EXPECT_EQ(doc.body, "<!doctype html><title>Mine</title><script>x()</script>");
  auto frag = get("/frag.html");
  ASSERT_EQ(frag.status, 200) << frag.error;
  EXPECT_NE(frag.body.find("<section>hi {{not a tag}}</section>"), std::string::npos);
  EXPECT_NE(frag.body.find("<title>Frag</title>"), std::string::npos);
}

TEST_F(SiteTest, TenantTemplatesLayoutsPartialsAndData) {
  put("_data/site.json", R"({"title":"Bob's","nav":[{"label":"Blog","url":"/blog"}]})");
  put("_templates/base.html", "<html><title>{{title}} | {{site.title}}</title>{{> nav}}<main>{{{body}}}</main></html>");
  put("_templates/nav.html", "<nav>{{#site.nav}}<a href=\"{{url}}\">{{label}}</a>{{/site.nav}}</nav>");
  put("_templates/post.html", "---\nlayout: base\n---\n<article><h1>{{title}}</h1>{{{body}}}<i>{{mood}}</i></article>");
  put("hello.md", "---\ntemplate: post\ntitle: Hi <there>\nmood: \"<b>happy</b>\"\n---\nText.");
  auto r = get("/hello");
  ASSERT_EQ(r.status, 200) << r.error;
  EXPECT_EQ(r.body,
            "<html><title>Hi &lt;there&gt; | Bob&#39;s</title><nav><a href=\"/blog\">Blog</a></nav><main><article><h1>Hi "
            "&lt;there&gt;</h1><p>Text.</p>\n<i>&lt;b&gt;happy&lt;/b&gt;</i></article></main></html>");
  // Templates and data are never served.
  EXPECT_EQ(get("/_templates/post.html").status, 404);
  EXPECT_EQ(get("/_data/site.json").status, 404);
  // A missing template is a 500 with a reason.
  put("broken.md", "---\ntemplate: nope\n---\nx");
  auto b = get("/broken");
  EXPECT_EQ(b.status, 500);
  EXPECT_NE(b.error.find("no template named nope"), std::string::npos);
}

TEST_F(SiteTest, ListingsSortPageFilterAndSkipDrafts) {
  put("blog/index.md", "---\ntitle: Blog\nlist: blog\nper-page: 2\nfeed: true\n---\nAll posts.");
  put("blog/a.md", "---\ntitle: A\ndate: 2026-01-01\ntags: [\"news\"]\n---\nFirst.");
  put("blog/b.md", "---\ntitle: B\ndate: 2026-03-01\n---\nSecond.");
  put("blog/c/index.md", "---\ntitle: C\ndate: 2026-02-01\ntags: [\"news\"]\nimage: /attachment/1.png\n---\nThird.");
  put("blog/draft.md", "---\ntitle: D\ndate: 2027-01-01\ndraft: true\n---\nNot yet.");
  put("blog/secret.md", "---\ntitle: S\ndate: 2026-04-01\nrequirements: [[\"owner\"]]\n---\nShh.");
  put("blog/deep/x/y.md", "deep");
  allow = 403;
  auto r = get("/blog");
  ASSERT_EQ(r.status, 200) << r.error;
  EXPECT_EQ(r.kind, site::Kind::kListing);
  auto b = r.body.find(">B</a>"), c = r.body.find(">C</a>");
  EXPECT_NE(b, std::string::npos);
  EXPECT_NE(c, std::string::npos);
  EXPECT_LT(b, c);
  EXPECT_EQ(r.body.find(">A</a>"), std::string::npos);  // on page 2
  EXPECT_EQ(r.body.find(">D</a>"), std::string::npos);
  EXPECT_EQ(r.body.find(">S</a>"), std::string::npos);  // the visitor couldn't open it
  EXPECT_NE(r.body.find("Page 1 of 2"), std::string::npos);
  EXPECT_NE(r.body.find("/blog?page=2"), std::string::npos);
  auto p2 = get("/blog", {{"page", "2"}});
  EXPECT_NE(p2.body.find(">A</a>"), std::string::npos);
  EXPECT_EQ(get("/blog", {{"page", "3"}}).status, 404);
  auto tagged = get("/blog", {{"tag", "news"}});
  EXPECT_NE(tagged.body.find(">C</a>"), std::string::npos);
  EXPECT_NE(tagged.body.find(">A</a>"), std::string::npos);
  EXPECT_EQ(tagged.body.find(">B</a>"), std::string::npos);
  EXPECT_EQ(get("/blog/draft").status, 404);
  EXPECT_EQ(get("/blog/secret").status, 403);
  allow = 200;
  EXPECT_EQ(get("/blog/secret").status, 200);

  auto feed = get("/blog/feed.xml");
  ASSERT_EQ(feed.status, 200) << feed.error;
  EXPECT_EQ(feed.kind, site::Kind::kFeed);
  EXPECT_NE(feed.body.find("<link href=\"https://example.com/blog/b\"/>"), std::string::npos);
  EXPECT_NE(feed.body.find("<updated>2026-04-01T00:00:00Z</updated>"), std::string::npos);
}

TEST_F(SiteTest, DirectoriesAreListedOnlyWithADirectoryTemplate) {
  put("notes/one.md", "# One\n\nFirst note.");
  put("notes/two.md", "# Two\n\nSecond note.");
  EXPECT_EQ(get("/notes").status, 404);
  put("_templates/directory.html", "<h1>{{title}}</h1>{{#pages}}<a href=\"{{url}}\">{{title}}</a>{{/pages}}");
  auto r = get("/notes");
  ASSERT_EQ(r.status, 200) << r.error;
  EXPECT_EQ(r.kind, site::Kind::kDirectory);
  EXPECT_NE(r.body.find("<h1>Notes</h1>"), std::string::npos);
  EXPECT_NE(r.body.find("<a href=\"/notes/one\">One</a>"), std::string::npos);
}

TEST_F(SiteTest, RedirectsStaticFilesAndRenderNamed) {
  put("old.md", "---\nredirect: /new\n---\n");
  put("gone.md", "---\nredirect: x\nredirect-status: 410\n---\n");
  put("css/site.css", "body{}");
  auto r = get("/old");
  EXPECT_EQ(r.status, 301);
  EXPECT_EQ(r.location, "/new");
  EXPECT_EQ(get("/gone").status, 410);
  auto css = get("/css/site.css");
  EXPECT_EQ(css.status, 200);
  EXPECT_EQ(css.content_type, "text/css; charset=utf-8");

  std::string err;
  std::map<std::string, std::string> inline_templates{{"job", "<b>{{id}}</b>{{{body}}}"}};
  auto html = s->render_named("t", "job", boost::json::object{{"id", "<7>"}, {"body", "<script>"}}, &inline_templates, &err);
  ASSERT_TRUE(html) << err;
  EXPECT_EQ(*html, "<b>&lt;7&gt;</b>");
}

TEST_F(SiteTest, BrokenFrontMatterFailsClosed) {
  allow = 200;
  // Each of these meant to be private, and none may be served or listed as a public page.
  put("a.md", "---\ntitle: A\nrequirements: [[\"owner\"]\n---\nsecret");  // bad JSON
  put("b.md", "---\ntitle: B\nrequirements: [[\"owner\"]]\nsecret, never closed");
  put("c.md", "---\ntitle: C\ndraft: True\n---\nwip");
  put("d.html", "--- \ntitle: D\nrequirements: []\n---\n<p>secret</p>");  // trailing space: still front matter
  put("notes/index.md", "---\ntitle: Notes\nlist: notes\n---\n");
  put("notes/hidden.md", "---\ntitle: Hidden\nrequirements: [[\"owner\"]\n---\nsecret");
  for (const char* path : {"/a", "/b", "/c"}) {
    auto r = get(path);
    EXPECT_EQ(r.status, 500) << path;
    EXPECT_FALSE(r.error.empty()) << path;
  }
  allow = 401;
  EXPECT_EQ(get("/d").status, 401);  // its requirements hold
  allow = 200;
  auto notes = get("/notes");
  EXPECT_EQ(notes.status, 200);
  EXPECT_EQ(notes.body.find("Hidden"), std::string::npos);  // a broken page isn't listed
}
