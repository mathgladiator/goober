#pragma once

// A tolerant HTML parser and the views the crawler offers (800.Crawl.md § What a page
// becomes; design/machine/017.Crawl.md). It parses what real pages are, not what the spec
// wishes they were: unclosed tags, stray end tags, missing quotes. It is not the HTML5
// tree-construction algorithm, and it never runs scripts. Every walk is bounded: depth,
// nodes, and output sizes, so a hostile page costs a bounded amount of work.

#include <boost/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "goober/web/url.h"

namespace goober::web {

namespace bj = boost::json;

struct Node {
  enum class Kind { kDocument, kElement, kText };
  Kind kind = Kind::kElement;
  std::string tag;  // lowercase, for elements
  std::vector<std::pair<std::string, std::string>> attrs;  // names lowercase, values decoded
  std::string text;  // for text nodes, decoded (raw for script and style)
  int parent = -1;
  std::vector<int> children;

  const std::string* attr(std::string_view name) const;
  bool has_class(std::string_view cls) const;
};

class Document {
 public:
  static constexpr size_t kMaxNodes = 200'000;
  static constexpr int kMaxDepth = 256;

  // Parses HTML. Text past kMaxNodes is dropped; nesting past kMaxDepth is flattened.
  static Document parse(std::string_view html);

  const Node& node(int i) const { return nodes_[size_t(i)]; }
  int root() const { return 0; }
  size_t size() const { return nodes_.size(); }
  // The base for links: the page's URL, or its <base href> when it has one.
  Url base_for(const Url& page) const;

  // The visible text under a node: whitespace collapsed, blocks on their own lines.
  std::string text_of(int node, size_t max_bytes = 1 << 20) const;
  // The node as HTML again, at most max_bytes.
  std::string outer_html(int node, size_t max_bytes = 64 << 10) const;

  std::vector<int> elements(std::string_view tag) const;

 private:
  std::vector<Node> nodes_;
};

// A CSS selector, in the subset scrapers use: type, *, #id, .class, [attr], [attr=v],
// [attr~=v], [attr|=v], [attr^=v], [attr$=v], [attr*=v], :first-child, :last-child,
// :nth-child(n), descendant and child (>) combinators, and comma groups.
class Selector {
 public:
  static std::optional<Selector> parse(std::string_view text, std::string* error);
  bool matches(const Document& doc, int node) const;
  // Elements matching, in document order, under `scope` (not the scope itself).
  std::vector<int> select(const Document& doc, int scope, size_t limit) const;

 private:
  struct Attr {
    std::string name, op, value;
  };
  struct Compound {
    std::string tag;  // empty or * for any
    std::vector<std::string> ids, classes;
    std::vector<Attr> attrs;
    int nth = 0;          // :nth-child(n), 1-based; 0 for none
    bool last = false;    // :last-child
    char combinator = 0;  // how it joins the compound before it: ' ' or '>', 0 for the first
  };
  bool match_compound(const Document& doc, int node, const Compound& c) const;
  bool match_from(const Document& doc, int node, const std::vector<Compound>& chain, size_t at,
                  std::unordered_map<int64_t, bool>& memo) const;
  std::vector<std::vector<Compound>> groups_;
};

// The views (800.Crawl.md). Each is bounded in what it returns.
std::string view_text(const Document& doc);
std::string view_markdown(const Document& doc, const Url& base);
bj::array view_links(const Document& doc, const Url& base);
bj::object view_meta(const Document& doc, const Url& base);
bj::array view_jsonld(const Document& doc);
bj::array view_tables(const Document& doc);
// {tag, text, attrs, html} per match. nullopt with `error` for a bad selector.
std::optional<bj::array> view_select(const Document& doc, std::string_view selector, std::string* error);
// Records from {"items": selector, "fields": {name: "sel", "sel@attr", "@attr", or ""}}.
std::optional<bj::array> view_extract(const Document& doc, const Url& base, const bj::value& spec, std::string* error);

// Decodes character references in text: &amp;, &#38;, &#x26;, and the common named ones.
std::string decode_entities(std::string_view text);

}  // namespace goober::web
