#include <gtest/gtest.h>

#include "goober/site/includes.h"
#include "goober/site/pwa.h"
#include "goober/site/site.h"

using namespace goober;
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {
const char* kShop = R"({"scope": "/shop/", "manifest": {"name": "KC Meat Shop", "short_name": "Meat",
  "theme_color": "#8b1e1e", "background_color": "#fff",
  "icons": [{"src": "/icons/192.png", "sizes": "192x192", "type": "image/png"},
            {"src": "/icons/512.png", "sizes": "512x512", "type": "image/png", "purpose": "any maskable"}],
  "shortcuts": [{"name": "Orders", "url": "/shop/orders"}]},
  "offline": {"fallback": "/shop/offline", "precache": ["/css/shop.css"]}})";
}  // namespace

TEST(Pwa, AnAppIsCheckedAndItsManifestFilledIn) {
  std::string err;
  std::vector<std::string> warnings;
  auto app = site::parse_app("shop", kShop, &err, &warnings);
  ASSERT_TRUE(app) << err;
  EXPECT_TRUE(warnings.empty());
  EXPECT_EQ(app->scope, "/shop/");
  auto m = site::manifest_for(*app);
  EXPECT_EQ(m["scope"], "/shop/");
  EXPECT_EQ(m["start_url"], "/shop/");
  EXPECT_EQ(m["id"], "/shop/");
  EXPECT_EQ(m["display"], "standalone");
  EXPECT_EQ(m["name"], "KC Meat Shop");

  // The whole domain, with nothing but a name: an app, with what's missing for install named.
  warnings.clear();
  auto whole = site::parse_app("site", R"({"manifest": {"name": "KC Meat Up"}})", &err, &warnings);
  ASSERT_TRUE(whole) << err;
  EXPECT_EQ(whole->scope, "/");
  EXPECT_EQ(warnings.size(), 2u);

  for (const char* bad : {
           R"({"scope": "shop", "manifest": {"name": "x"}})",                        // not a path
           R"({"scope": "/shop", "manifest": {"name": "x"}})",                       // no trailing slash
           R"({"scope": "/identity/", "manifest": {"name": "x"}})",                  // the host's
           R"({"scope": "/_data/", "manifest": {"name": "x"}})",                     // private
           R"({"manifest": {}})",                                                     // no name
           R"({"manifest": {"name": "x", "scope": "/"}})",                           // scope is the app's
           R"({"manifest": {"name": "x", "colour": "red"}})",                        // a typo
           R"({"scope": "/shop/", "manifest": {"name": "x", "start_url": "/blog"}})", // out of scope
           R"({"manifest": {"name": "x", "icons": [{"src": "https://cdn.example/i.png"}]}})",
           R"({"manifest": {"name": "x", "theme_color": "\"><script>"}})",
           R"({"scope": "/shop/", "manifest": {"name": "x"}, "worker": {"src": "/shop/a/sw.js"}})",  // below its scope
           R"({"manifest": {"name": "x"}, "flavor": 1})"}) {
    EXPECT_FALSE(site::parse_app("x", bad, &err)) << bad;
  }
  EXPECT_FALSE(site::parse_app("Bad Name", R"({"manifest": {"name": "x"}})", &err));
  auto own = site::parse_app("x", R"({"scope": "/shop/", "manifest": {"name": "x"}, "worker": {"src": "/shop/sw.js"}})", &err);
  ASSERT_TRUE(own) << err;
  EXPECT_EQ(own->worker, "own");
}

TEST(Pwa, TheLongestScopeWinsAndTheHeadTagsNameTheApp) {
  std::string err;
  std::vector<site::App> apps{*site::parse_app("site", R"({"manifest": {"name": "Site"}})", &err),
                              *site::parse_app("shop", kShop, &err)};
  EXPECT_EQ(site::app_for(apps, "/")->name, "site");
  EXPECT_EQ(site::app_for(apps, "/blog/x")->name, "site");
  EXPECT_EQ(site::app_for(apps, "/shop/")->name, "shop");
  EXPECT_EQ(site::app_for(apps, "/shop")->name, "shop");  // the scope's own page, without its slash
  EXPECT_EQ(site::app_for(apps, "/shop/orders/7")->name, "shop");
  EXPECT_EQ(site::app_for(apps, "/shopping")->name, "site");
  EXPECT_FALSE(site::app_for({apps[1]}, "/blog"));
  auto tags = site::head_tags(apps[1]);
  EXPECT_NE(tags.find(R"(<link rel="manifest" href="/goober/pwa/shop/manifest.webmanifest">)"), std::string::npos);
  EXPECT_NE(tags.find(R"(<meta name="theme-color" content="#8b1e1e">)"), std::string::npos);
  EXPECT_NE(tags.find(R"(<link rel="apple-touch-icon" href="/icons/512.png">)"), std::string::npos);
  EXPECT_NE(tags.find(R"(<script src="/goober/pwa/shop/register.js" defer></script>)"), std::string::npos);
}

TEST(Pwa, TheWorkerAndRegistrationCarryTheirConfig) {
  std::string err;
  auto app = *site::parse_app("shop", kShop, &err);
  auto sw = site::service_worker(app, "abc123");
  EXPECT_NE(sw.find("\"scope\":\"/shop/\""), std::string::npos);
  EXPECT_NE(sw.find("\"precache\":[\"/shop/\",\"/shop/offline\",\"/css/shop.css\"]"), std::string::npos);
  EXPECT_NE(sw.find("\"version\":\"" + app.etag + "-abc123\""), std::string::npos);
  EXPECT_NE(sw.find("\"/identity/\""), std::string::npos);  // never cached
  EXPECT_NE(sw.find("addEventListener('push'"), std::string::npos);
  auto reg = site::register_script(app, "BKEY");
  EXPECT_NE(reg.find("\"worker\":\"/goober/pwa/shop/sw.js\""), std::string::npos);
  EXPECT_NE(reg.find("\"push_key\":\"BKEY\""), std::string::npos);
  auto quiet = *site::parse_app("q", R"({"manifest": {"name": "q"}, "push": false})", &err);
  EXPECT_NE(site::register_script(quiet, "BKEY").find("\"push_key\":\"\""), std::string::npos);
}

TEST(Pwa, ThePwaMarkerExpandsForTheAppInScopeAndIsNeverMissing) {
  auto root = fs::path(GOOBER_TEST_TMP) / "pwa_site";
  fs::remove_all(root);
  wal::HostWal w(root);
  content::ContentStore store(w);
  std::string err;
  ASSERT_TRUE(store.put("t", "_apps/shop.json", kShop, &err)) << err;
  auto in = site::expand_includes(store, "t", "<head><!--#include pwa--></head>", "/shop/cart", false);
  EXPECT_NE(in.html.find("/goober/pwa/shop/manifest.webmanifest"), std::string::npos) << in.html;
  EXPECT_TRUE(in.missing.empty());
  auto out = site::expand_includes(store, "t", "<head><!--#include pwa--></head>", "/blog", false);
  EXPECT_EQ(out.html, "<head></head>");
  EXPECT_TRUE(out.missing.empty());
  // A tenant's own pwa partial wins.
  ASSERT_TRUE(store.put("t", "_partials/pwa.html", "<link rel=\"manifest\" href=\"/mine.webmanifest\">", &err));
  EXPECT_NE(site::expand_includes(store, "t", "<!--#include pwa-->", "/shop/", false).html.find("/mine.webmanifest"),
            std::string::npos);
  // The built-in base template carries the marker, so a page in scope gets the tags.
  ASSERT_TRUE(store.remove("t", "_partials/pwa.html", &err));
  ASSERT_TRUE(store.put("t", "shop/index.md", "# Shop", &err));
  site::Site s(store);
  site::Visit v;
  v.tenant = "t";
  v.path = "/shop";
  v.accept = "text/html";
  auto r = s.visit(v);
  ASSERT_EQ(r.status, 200) << r.error;
  EXPECT_NE(r.body.find("/goober/pwa/shop/manifest.webmanifest"), std::string::npos);
}
