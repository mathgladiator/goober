#include "goober/web/html.h"

#include <gtest/gtest.h>

#include "goober/json/json.h"

using namespace goober;
using namespace goober::web;

namespace {

const char* kRanch = R"html(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8"><title>Hill Ranch &amp; Butcher</title>
<meta name="description" content="Grass-fed beef near Fresno">
<meta property="og:title" content="Hill Ranch">
<link rel="canonical" href="/">
<link rel="alternate" type="application/rss+xml" href="/feed.xml">
<script src="https://cdn.shopify.com/s/shop.js"></script>
<script type="application/ld+json">
{"@context":"https://schema.org","@graph":[
 {"@type":"LocalBusiness","name":"Hill Ranch","telephone":"555-0100"},
 {"@type":"Product","name":"Quarter beef","offers":{"@type":"Offer","price":"950.00","priceCurrency":"USD"}}]}
</script>
<style>p { color: red }</style>
</head>
<body>
<nav><a href="/">Home</a> <a href="shop/">Shop</a> <a href="mailto:hi@hill.example">Email</a></nav>
<h1>Hill <em>Ranch</em></h1>
<p>Grass-fed, <b>dry-aged</b> beef.<br>Pickup Saturdays.
<p>Second paragraph &mdash; no closing tag
<ul><li>Steaks<li>Roasts<ul><li>Chuck</li></ul><li>Ground</ul>
<div class="products">
  <div class="product-card" data-sku="q1"><h3 class="title">Quarter beef</h3><span class="price">$950</span><a href="/p/quarter">More</a></div>
  <div class="product-card sale" data-sku="h1"><h3 class="title">Half beef</h3><span class="price">$1,800</span><a href="/p/half">More</a></div>
  <div class="product-card"><h3 class="title">Sold out</h3></div>
</div>
<table><caption>Prices</caption><tr><th>Cut</th><th>Price</th></tr><tr><td>Ribeye</td><td>$24/lb</td></tr><tr><td>Ground</td><td>$9/lb</tr></table>
<pre>  keep   this
spacing</pre>
<script>document.write("<p>not text</p>")</script>
</body></html>)html";

}  // namespace

TEST(Html, ParsesRealWorldMarkupTolerantly) {
  auto doc = Document::parse(kRanch);
  EXPECT_EQ(doc.elements("li").size(), 4u);  // unclosed <li>s close each other
  EXPECT_EQ(doc.elements("p").size(), 2u);   // the second <p> closes the first
  // The nested list belongs to "Roasts", not to the outer list.
  auto lis = doc.elements("li");
  EXPECT_EQ(doc.node(doc.node(lis[2]).parent).tag, "ul");
  EXPECT_EQ(doc.node(doc.node(doc.node(lis[2]).parent).parent).tag, "li");
  // Script text is raw and never a tag.
  auto scripts = doc.elements("script");
  EXPECT_EQ(doc.node(doc.node(scripts.back()).children[0]).text, R"(document.write("<p>not text</p>"))");
}

TEST(Html, TextViewIsWhatAPersonReads) {
  auto doc = Document::parse(kRanch);
  std::string text = view_text(doc);
  EXPECT_NE(text.find("Hill Ranch\n\nGrass-fed, dry-aged beef.\nPickup Saturdays."), std::string::npos) << text;
  EXPECT_NE(text.find("Second paragraph \xE2\x80\x94 no closing tag"), std::string::npos);
  EXPECT_EQ(text.find("color: red"), std::string::npos);
  EXPECT_EQ(text.find("not text"), std::string::npos);
  EXPECT_EQ(text.find("Quarter beef\",\"offers"), std::string::npos);  // JSON-LD isn't text
  EXPECT_NE(text.find("  keep   this\nspacing"), std::string::npos);  // pre keeps its spacing
}

TEST(Html, MarkdownView) {
  auto doc = Document::parse(kRanch);
  auto base = *parse_url("https://hill.example/index.html");
  std::string md = view_markdown(doc, base);
  EXPECT_NE(md.find("# Hill *Ranch*"), std::string::npos) << md;
  EXPECT_NE(md.find("[Shop](https://hill.example/shop/)"), std::string::npos) << md;
  EXPECT_NE(md.find("**dry-aged**"), std::string::npos) << md;
  EXPECT_NE(md.find("- Steaks"), std::string::npos) << md;
  EXPECT_NE(md.find("  - Chuck"), std::string::npos) << md;
  EXPECT_NE(md.find("| Cut | Price |\n| --- | --- |\n| Ribeye | $24/lb |"), std::string::npos) << md;
  EXPECT_NE(md.find("```\n  keep   this\nspacing\n```"), std::string::npos) << md;
}

TEST(Html, LinksMetaJsonLdTables) {
  auto doc = Document::parse(kRanch);
  auto base = *parse_url("https://hill.example/");
  auto links = view_links(doc, base);
  std::string l = json::dump(links);
  EXPECT_NE(l.find(R"({"url":"https://hill.example/shop/","text":"Shop"})"), std::string::npos) << l;
  EXPECT_NE(l.find(R"("url":"mailto:hi@hill.example")"), std::string::npos) << l;

  auto meta = view_meta(doc, base);
  EXPECT_EQ(meta.at("title"), "Hill Ranch & Butcher");
  EXPECT_EQ(meta.at("description"), "Grass-fed beef near Fresno");
  EXPECT_EQ(meta.at("canonical"), "https://hill.example/");
  EXPECT_EQ(meta.at("lang"), "en");
  EXPECT_EQ(meta.at("og").at("title"), "Hill Ranch");
  EXPECT_EQ(meta.at("platform"), "shopify");
  EXPECT_EQ(meta.at("feeds").at(0).at("url"), "https://hill.example/feed.xml");

  auto ld = view_jsonld(doc);
  ASSERT_EQ(ld.size(), 2u);  // the @graph, flattened
  EXPECT_EQ(ld[1].at("offers").at("price"), "950.00");

  auto tables = view_tables(doc);
  ASSERT_EQ(tables.size(), 1u);
  EXPECT_EQ(tables[0].at("caption"), "Prices");
  EXPECT_EQ(json::dump(tables[0].at("headers")), R"(["Cut","Price"])");
  EXPECT_EQ(json::dump(tables[0].at("rows")), R"([["Ribeye","$24/lb"],["Ground","$9/lb"]])");
}

TEST(Html, SelectorsAndExtraction) {
  auto doc = Document::parse(kRanch);
  auto base = *parse_url("https://hill.example/");
  std::string err;
  auto pick = [&](const char* sel) {
    auto s = Selector::parse(sel, &err);
    EXPECT_TRUE(s) << sel << ": " << err;
    return s ? s->select(doc, doc.root(), 100).size() : size_t(999);
  };
  EXPECT_EQ(pick(".product-card"), 3u);
  EXPECT_EQ(pick("div.product-card.sale"), 1u);
  EXPECT_EQ(pick("[data-sku]"), 2u);
  EXPECT_EQ(pick("[data-sku^=h]"), 1u);
  EXPECT_EQ(pick(".products > .product-card .price"), 2u);
  EXPECT_EQ(pick("body > .price"), 0u);
  EXPECT_EQ(pick("ul > li:first-child"), 2u);
  EXPECT_EQ(pick("li:last-child"), 2u);  // Chuck, and Ground
  EXPECT_EQ(pick("tr:nth-child(3) td"), 2u);  // the caption is the first child
  EXPECT_EQ(pick("h1, h3"), 4u);
  for (const char* bad : {"a + b", "a ~ b", "a:hover", "[x", "", "#", ".a[", "a::before"}) {
    EXPECT_FALSE(Selector::parse(bad, &err)) << bad;
  }

  auto sel = view_select(doc, ".price", &err);
  ASSERT_TRUE(sel) << err;
  EXPECT_EQ(sel->at(0).at("text"), "$950");
  EXPECT_EQ(sel->at(0).at("html"), R"(<span class="price">$950</span>)");

  auto spec = json::parse(R"({"items":".product-card","fields":{"name":".title","price":".price",
      "url":"a@href","sku":"@data-sku"}})", &err);
  auto recs = view_extract(doc, base, *spec, &err);
  ASSERT_TRUE(recs) << err;
  ASSERT_EQ(recs->size(), 3u);
  EXPECT_EQ(json::dump(recs->at(1)),
            R"({"name":"Half beef","price":"$1,800","url":"https://hill.example/p/half","sku":"h1"})");
  EXPECT_TRUE(recs->at(2).at("price").is_null());
  EXPECT_FALSE(view_extract(doc, base, *json::parse(R"({"items":"a+b","fields":{"x":""}})", &err), &err));
}

TEST(Html, EntitiesAndHostileInput) {
  EXPECT_EQ(decode_entities("a &amp b &lt;c&gt; &#65;&#x42; &bogus; &#0; &#x110000;"),
            "a & b <c> AB &bogus; \xEF\xBF\xBD \xEF\xBF\xBD");
  // Deep nesting is flattened, not recursed into without bound.
  std::string deep;
  for (int i = 0; i < 100000; ++i) deep += "<div>";
  deep += "x";
  auto doc = Document::parse(deep);
  EXPECT_EQ(view_text(doc), "x");
  std::string err;
  auto s = Selector::parse("div div div div div div div div div div div div div div div div div div div div div div div "
                           "div div div div div div div div div",
                           &err);
  ASSERT_TRUE(s) << err;
  EXPECT_FALSE(s->select(doc, doc.root(), 10).empty());  // memoized: quick, not exponential
  // Unclosed everything, stray end tags, attributes without quotes or values.
  auto messy = Document::parse("<p class=a b=c><a href=/x>link</b></div><img src=1 alt>tail<");
  EXPECT_NE(view_text(messy).find("tail"), std::string::npos);
  auto huge = Document::parse(std::string(1 << 20, '<'));
  EXPECT_LE(view_text(huge).size(), size_t(1 << 20));
}

TEST(Html, SelectorGroupsAreCapped) {
  std::string sel = "section p";
  for (int i = 1; i < 64; ++i) sel += ", zz" + std::to_string(i);
  sel += ", p";
  std::string err;
  EXPECT_FALSE(Selector::parse(sel, &err));
}
