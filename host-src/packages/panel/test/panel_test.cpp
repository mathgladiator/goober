#include <gtest/gtest.h>

#include <regex>
#include <string>

#include "goober/panel/assets.h"

using goober::panel::find;

TEST(PanelAssets, EveryAssetIsThereWithItsType) {
  for (auto [path, type] : {std::pair{"/", "text/html; charset=utf-8"}, std::pair{"/panel.js", "text/javascript; charset=utf-8"},
                            std::pair{"/panel.css", "text/css; charset=utf-8"}, std::pair{"/favicon.svg", "image/svg+xml"}}) {
    const auto* a = find(path);
    ASSERT_NE(a, nullptr) << path;
    EXPECT_EQ(a->content_type, type);
    EXPECT_FALSE(a->body.empty());
    EXPECT_EQ(a->etag.size(), 18u);  // a quoted 16-hex prefix
  }
  EXPECT_EQ(find("/index.html"), nullptr);
  EXPECT_EQ(find("/nope.js"), nullptr);
}

// The CSP is script-src 'self'; style-src 'self': nothing inline may need an exception.
TEST(PanelAssets, NothingInlineAndNoMarkupFromData) {
  std::string html(find("/")->body);
  std::regex inline_script(R"(<script(?![^>]*\bsrc=)[^>]*>)");
  EXPECT_FALSE(std::regex_search(html, inline_script));
  EXPECT_EQ(html.find(" style="), std::string::npos);
  EXPECT_EQ(html.find("<style"), std::string::npos);
  EXPECT_EQ(html.find(" on"), std::string::npos);  // no onclick= and friends
  std::string js(find("/panel.js")->body);
  // The SVG namespace is a name, not a request.
  js.erase(js.find("http://www.w3.org/2000/svg"), 26);
  for (const char* banned : {"innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function"}) {
    EXPECT_EQ(js.find(banned), std::string::npos) << banned;
  }
  // Every call goes to the host's own /api/.
  EXPECT_EQ(js.find("http://"), std::string::npos);
  EXPECT_EQ(js.find("https://"), std::string::npos);
}
