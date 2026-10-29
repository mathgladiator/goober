#include <gtest/gtest.h>

#include "goober/site/includes.h"
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
  EXPECT_NE(r.body.find("<title>About us · "), std::string::npos);
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
  EXPECT_NE(frag.body.find("<title>Frag · "), std::string::npos);
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
  // Valid Atom needs an author; tags become categories.
  EXPECT_NE(feed.body.find("<author><name>"), std::string::npos);
  EXPECT_NE(feed.body.find("<category term=\"news\"/>"), std::string::npos);
}

TEST_F(SiteTest, APagesJsonLdFieldIsTheHostBuiltTag) {
  put("_templates/post.html", "<head>{{{json_ld}}}</head><h1>{{title}}</h1>{{{body}}}");
  put("updates/hi.md",
      "---\ntitle: I'm back\ntemplate: post\njson_ld: {\"@type\": \"Article\", \"headline\": \"I'm back </script>\"}\n---\nHello.");
  auto r = get("/updates/hi");
  ASSERT_EQ(r.status, 200) << r.error;
  EXPECT_NE(r.body.find("<script type=\"application/ld+json\">{\"@type\":\"Article\",\"headline\":\"I'm back \\u003c/script\\u003e\"}</script>"),
            std::string::npos)
      << r.body;
  EXPECT_NE(r.body.find("<h1>I&#39;m back</h1>"), std::string::npos) << r.body;
  // Anything but JSON is left out, never placed as markup.
  put("updates/bad.md", "---\ntemplate: post\njson_ld: <b>x</b>\n---\nx");
  auto bad = get("/updates/bad");
  EXPECT_EQ(bad.body.find("<b>x</b>"), std::string::npos) << bad.body;
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

  // json_ld: the host writes the script tag from the box's JSON, and no value can end it.
  std::map<std::string, std::string> ld_templates{{"place", "<head>{{{json_ld}}}</head><p>{{{name}}}</p>"}};
  std::string ld_text = R"({"@context":"https://schema.org","name":"Rye </script><script>alert(1)</script> & Co"})";
  auto ld = s->render_named("t", "place", boost::json::object{{"json_ld", ld_text}, {"name", "<i>Rye</i>"}}, &ld_templates, &err);
  ASSERT_TRUE(ld) << err;
  EXPECT_EQ(*ld, "<head><script type=\"application/ld+json\">{\"@context\":\"https://schema.org\",\"name\":\"Rye "
                 "\\u003c/script\\u003e\\u003cscript\\u003ealert(1)\\u003c/script\\u003e \\u0026 Co\"}</script></head>"
                 "<p>&lt;i&gt;Rye&lt;/i&gt;</p>");
  // An object works the same; anything that isn't JSON renders nothing.
  auto obj = s->render_named("t", "place", boost::json::object{{"json_ld", boost::json::object{{"a", "<"}}}}, &ld_templates, &err);
  EXPECT_EQ(*obj, "<head><script type=\"application/ld+json\">{\"a\":\"\\u003c\"}</script></head><p></p>");
  auto junk = s->render_named("t", "place", boost::json::object{{"json_ld", "<script>alert(1)</script>"}}, &ld_templates, &err);
  EXPECT_EQ(*junk, "<head></head><p></p>");
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

// Shared partials (design/machine/027.Partials.md): the active link.
TEST(Partials, TheBestFittingLinkIsMarked) {
  const std::string nav = R"(<nav><a href="/" data-match-exact="/">Home</a>)"
                          R"(<a href="/places" class="nav" data-match="/places">Places</a>)"
                          R"(<a href="/places/kc" data-match="/places/kc">KC</a>)"
                          R"(<a href=/future data-match=/future class=x data-key="future">Future</a></nav>)";
  // Home only on "/" itself.
  auto home = site::mark_active(nav, "/", "");
  EXPECT_NE(home.find(R"(<a href="/" data-match-exact="/" aria-current="page" class="active">)"), std::string::npos) << home;
  EXPECT_EQ(home.find("active", home.find("Home")), home.find("active", home.find("Home")));
  EXPECT_EQ(site::mark_active(nav, "/about", ""), nav);  // nothing fits
  // A prefix covers what's below it; the longest fit wins; class is appended to, query ignored.
  auto place = site::mark_active(nav, "/places/joes?x=1", "");
  EXPECT_NE(place.find(R"(class="nav active" data-match="/places" aria-current="page">)"), std::string::npos) << place;
  auto kc = site::mark_active(nav, "/places/kc/", "");
  EXPECT_NE(kc.find(R"(data-match="/places/kc" aria-current="page" class="active">KC)"), std::string::npos) << kc;
  EXPECT_EQ(kc.find(R"(class="nav active")"), std::string::npos) << kc;
  // A prefix isn't a string prefix: /placesx isn't under /places.
  EXPECT_EQ(site::mark_active(nav, "/placesx", ""), nav);
  // An unquoted class gets quoted; a page's nav-key wins over the path.
  auto keyed = site::mark_active(nav, "/places", "future");
  EXPECT_NE(keyed.find(R"(class="x active" data-key="future" aria-current="page">Future)"), std::string::npos) << keyed;
  EXPECT_EQ(keyed.find(R"(class="nav active")"), std::string::npos) << keyed;
}

TEST_F(SiteTest, PartialsAreSplicedIntoPagesOnce) {
  put("_partials/nav.html", R"(<a href="/about" data-match="/about">About</a><!--#include nav-->)");
  put("about.html", "<!doctype html><html><body><!--#include nav--><main>About</main><!--#include  footer --></body></html>");
  auto r = get("/about");
  ASSERT_EQ(r.status, 200) << r.error;
  // Spliced raw, marked active, one level only (the partial's own marker stays), missing ones empty.
  EXPECT_EQ(r.body, R"(<!doctype html><html><body><a href="/about" data-match="/about" aria-current="page" class="active">About</a>)"
                    R"(<!--#include nav--><main>About</main></body></html>)");
  bool flagged = false;
  for (const auto& [k, v] : r.headers) flagged |= k == "x-goober-partial-missing" && v == "footer";
  EXPECT_TRUE(flagged);
  EXPECT_FALSE(r.restricted);
  // One partial edit changes the page's ETag, with no page write.
  std::string before = r.etag;
  put("_partials/nav.html", R"(<a href="/about" data-match="/about">About us</a>)");
  EXPECT_NE(get("/about").etag, before);
  EXPECT_EQ(site::include_names("<!--#include nav--> <!--#include nav --> <!--#include Bad--> <!--#include footer-->"),
            (std::vector<std::string>{"nav", "footer"}));

  // nav@auth for signed-in visitors; either way the page is private to a cookie.
  put("_partials/nav@auth.html", "<a href=\"/me\">Me</a>");
  EXPECT_TRUE(get("/about").restricted);
  site::Visit v;
  v.tenant = "t";
  v.path = "/about";
  v.user = boost::json::object{{"user_id", "1"}};
  v.authorize = [](const boost::json::value&) { return 200; };
  auto signed_in = s->visit(v);
  EXPECT_NE(signed_in.body.find("<a href=\"/me\">Me</a>"), std::string::npos) << signed_in.body;
  EXPECT_TRUE(signed_in.restricted);
}
