#include <gtest/gtest.h>

#include <chrono>

#include "goober/render/markdown.h"

using namespace goober::render;

namespace {
std::string M(std::string_view md) {
  MarkdownOptions o;
  o.heading_ids = false;
  return markdown_to_html(md, o);
}
}  // namespace

TEST(Markdown, Blocks) {
  EXPECT_EQ(M("# Title\n\nHello\nworld.\n"), "<h1>Title</h1>\n<p>Hello\nworld.</p>\n");
  EXPECT_EQ(M("Setext\n======\n\nTwo\n---\n"), "<h1>Setext</h1>\n<h2>Two</h2>\n");
  EXPECT_EQ(M("## Closed ##\n"), "<h2>Closed</h2>\n");
  EXPECT_EQ(M("a\n\n***\n\nb"), "<p>a</p>\n<hr />\n<p>b</p>\n");
  EXPECT_EQ(M("> quoted\n> more\n\nafter"), "<blockquote>\n<p>quoted\nmore</p>\n</blockquote>\n<p>after</p>\n");
  EXPECT_EQ(M("```js\nlet x = 1 < 2;\n```\n"), "<pre><code class=\"language-js\">let x = 1 &lt; 2;\n</code></pre>\n");
  EXPECT_EQ(M("    indented\n    code\n"), "<pre><code>indented\ncode\n</code></pre>\n");
}

TEST(Markdown, Lists) {
  EXPECT_EQ(M("- a\n- b\n"), "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n");
  EXPECT_EQ(M("3. c\n4. d\n"), "<ol start=\"3\">\n<li>c</li>\n<li>d</li>\n</ol>\n");
  EXPECT_EQ(M("- a\n\n- b\n"), "<ul>\n<li>\n<p>a</p>\n</li>\n<li>\n<p>b</p>\n</li>\n</ul>\n");
  EXPECT_EQ(M("- a\n  - b\n  - c\n- d\n"),
            "<ul>\n<li>a\n<ul>\n<li>b</li>\n<li>c</li>\n</ul>\n</li>\n<li>d</li>\n</ul>\n");
  EXPECT_EQ(M("- [ ] todo\n- [x] done\n"),
            "<ul>\n<li><input type=\"checkbox\" disabled /> todo</li>\n"
            "<li><input type=\"checkbox\" checked disabled /> done</li>\n</ul>\n");
  EXPECT_EQ(M("- lazy\ncontinuation\n"), "<ul>\n<li>lazy\ncontinuation</li>\n</ul>\n");
}

TEST(Markdown, Inlines) {
  EXPECT_EQ(M("*em* **strong** ***both*** _u_ ~~del~~ `a<b`"),
            "<p><em>em</em> <strong>strong</strong> <em><strong>both</strong></em> <em>u</em> <del>del</del> "
            "<code>a&lt;b</code></p>\n");
  EXPECT_EQ(M("snake_case_name and 2*3*4"), "<p>snake_case_name and 2<em>3</em>4</p>\n");
  EXPECT_EQ(M("\\*not em\\* &amp; &copy; & < >"), "<p>*not em* &amp; &copy; &amp; &lt; &gt;</p>\n");
  EXPECT_EQ(M("line  \nbreak"), "<p>line<br />\nbreak</p>\n");
}

TEST(Markdown, LinksAndImages) {
  EXPECT_EQ(M("[a *b*](/x \"T\")"), "<p><a href=\"/x\" title=\"T\">a <em>b</em></a></p>\n");
  EXPECT_EQ(M("![a cat](/attachment/1.png)"), "<p><img src=\"/attachment/1.png\" alt=\"a cat\" /></p>\n");
  EXPECT_EQ(M("[ref] and [text][ref]\n\n[ref]: https://example.com \"Ex\"\n"),
            "<p><a href=\"https://example.com\" title=\"Ex\">ref</a> and "
            "<a href=\"https://example.com\" title=\"Ex\">text</a></p>\n");
  EXPECT_EQ(M("<https://a.example/x> and https://b.example/y."),
            "<p><a href=\"https://a.example/x\">https://a.example/x</a> and "
            "<a href=\"https://b.example/y\">https://b.example/y</a>.</p>\n");
  EXPECT_EQ(M("[link](https://x.example)"), "<p><a href=\"https://x.example\">link</a></p>\n");
  EXPECT_EQ(M("[not a link] [x]"), "<p>[not a link] [x]</p>\n");
}

TEST(Markdown, NothingTypedBecomesAScript) {
  EXPECT_EQ(M("<script>alert(1)</script>"), "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>\n");
  EXPECT_EQ(M("[x](javascript:alert(1)) ![y](vbscript:z)"),
            "<p><a href=\"#\">x</a> <img src=\"#\" alt=\"y\" /></p>\n");
  EXPECT_EQ(M("[x](\"onmouseover=alert(1))"), "<p><a href=\"&quot;onmouseover=alert(1)\">x</a></p>\n");
  MarkdownOptions allow;
  allow.allow_html = true;
  allow.heading_ids = false;
  EXPECT_EQ(markdown_to_html("<div class=\"x\">\nraw\n</div>\n", allow), "<div class=\"x\">\nraw\n</div>\n");
}

TEST(Markdown, Tables) {
  EXPECT_EQ(M("| a | b |\n|:--|--:|\n| 1 | *2* |\n"),
            "<table>\n<thead>\n<tr>\n<th align=\"left\">a</th>\n<th align=\"right\">b</th>\n</tr>\n</thead>\n"
            "<tbody>\n<tr>\n<td align=\"left\">1</td>\n<td align=\"right\"><em>2</em></td>\n</tr>\n</tbody>\n"
            "</table>\n");
}

TEST(Markdown, HeadingIdsTitleAndSummary) {
  EXPECT_EQ(markdown_to_html("# Getting Started!\n## Getting started\n"),
            "<h1 id=\"getting-started\">Getting Started!</h1>\n<h2 id=\"getting-started-1\">Getting started</h2>\n");
  EXPECT_EQ(markdown_title("intro\n\n# The *Real* Title\n"), "The Real Title");
  EXPECT_EQ(markdown_title("no heading"), "");
  EXPECT_EQ(markdown_summary("# Title\n\n```\ncode\n```\n\nFirst *para* &amp; more.\n\nSecond."),
            "First para & more.");
  EXPECT_EQ(markdown_summary("one two three four five", 12), "one two\xE2\x80\xA6");
  EXPECT_EQ(html_to_text("<p>a&lt;b</p>\n<p>c</p>"), "a<b c");
}

TEST(Markdown, HostileInputIsBoundedInDepthAndTime) {
  auto timed = [](const std::string& md) {
    auto start = std::chrono::steady_clock::now();
    auto html = markdown_to_html(md);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(ms, 3000) << md.substr(0, 20) << "... took " << ms << " ms";
    return html;
  };
  // Nesting past 32 becomes text instead of recursion that would overflow the stack.
  auto quotes = timed(std::string(8000, '>') + " deep");
  EXPECT_NE(quotes.find("deep"), std::string::npos);
  std::string items;
  for (int i = 0; i < 8000; ++i) items += "- ";
  timed(items + "x");
  // Inputs that make the inline scans quadratic stop at the work budget.
  std::string links, emph;
  for (int i = 0; i < 20000; ++i) links += "[a](";
  for (int i = 0; i < 80000; ++i) emph += "*a_";
  timed(links);
  timed(emph);
  // Ordinary documents are untouched by the limits.
  EXPECT_EQ(markdown_to_html("> > *hi*"), "<blockquote>\n<blockquote>\n<p><em>hi</em></p>\n</blockquote>\n</blockquote>\n");
}
