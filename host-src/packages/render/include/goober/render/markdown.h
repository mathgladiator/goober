#pragma once

#include <string>
#include <string_view>

namespace goober::render {

struct MarkdownOptions {
  // Raw HTML in the text passes through only when this is set. It is off by default,
  // since markdown often carries text a person typed (070.UI.md § Markdown).
  bool allow_html = false;
  // Gives each heading an id made from its text, like id="getting-started".
  bool heading_ids = true;
};

// Renders markdown to HTML. It covers the CommonMark blocks (ATX and setext headings,
// paragraphs, block quotes, ordered and bullet lists, fenced and indented code,
// thematic breaks, HTML blocks), the inlines (emphasis, strong, code spans, links,
// images, reference links, autolinks, hard breaks, backslash escapes, entities), and
// from GitHub's flavor, tables, task list items, strikethrough, and bare URLs.
// Link and image targets with a scheme other than http, https, mailto, or tel (and
// data:image/ for images) become "#", so markdown can't carry a javascript: link.
std::string markdown_to_html(std::string_view markdown, const MarkdownOptions& options = {});

// The text of the first top-level heading, without markup, or "" when there is none.
std::string markdown_title(std::string_view markdown);

// The first paragraph's text, without markup, cut at a word boundary to at most
// `max_chars` bytes and ended with "…" when cut. Headings, code, and lists are skipped.
std::string markdown_summary(std::string_view markdown, size_t max_chars = 240);

// HTML to plain text: tags dropped, the common entities decoded, whitespace collapsed.
std::string html_to_text(std::string_view html);

}  // namespace goober::render
