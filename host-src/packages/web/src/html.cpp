#include "goober/web/html.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <functional>
#include <set>
#include <unordered_map>

#include "goober/base/encoding.h"
#include "goober/json/json.h"

namespace goober::web {
namespace {

bool in(std::string_view tag, std::initializer_list<std::string_view> set) {
  return std::find(set.begin(), set.end(), tag) != set.end();
}

bool is_void(std::string_view t) {
  return in(t, {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track",
                "wbr", "keygen"});
}
// Their text is raw: no tags inside, and no character references (script, style), or
// character references but no tags (title, textarea).
bool is_raw(std::string_view t) { return in(t, {"script", "style", "xmp", "noembed", "noframes"}); }
bool is_rcdata(std::string_view t) { return in(t, {"title", "textarea"}); }
// Opening one of these closes an open <p> (HTML's "closes a p element").
bool closes_p(std::string_view t) {
  return in(t, {"address", "article", "aside", "blockquote", "details", "dialog", "div", "dl", "fieldset", "figcaption",
                "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "main",
                "menu", "nav", "ol", "p", "pre", "section", "table", "ul", "li", "dd", "dt"});
}
// Where searches for an open element stop: what's outside a table cell or button stays put.
bool is_scope_boundary(std::string_view t) {
  return in(t, {"html", "table", "td", "th", "caption", "template", "button", "marquee", "object", "applet"});
}
bool is_heading(std::string_view t) {
  return t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6';
}
// Not shown as text.
bool is_hidden(std::string_view t) {
  return in(t, {"script", "style", "noscript", "template", "svg", "head", "iframe", "object", "canvas", "select",
                "datalist", "math"});
}
bool is_block(std::string_view t) {
  return closes_p(t) || in(t, {"tr", "table", "thead", "tbody", "tfoot", "caption", "body", "html", "br", "option",
                               "legend", "summary"});
}
bool is_paragraph_like(std::string_view t) {
  return is_heading(t) || in(t, {"p", "table", "ul", "ol", "blockquote", "pre", "dl", "figure", "hr", "section",
                                 "article"});
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

struct Named {
  const char* name;
  uint32_t cp;
};
constexpr Named kNamed[] = {
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},      {"apos", '\''},
    {"nbsp", 0xA0},    {"copy", 0xA9},     {"reg", 0xAE},      {"trade", 0x2122},  {"hellip", 0x2026},
    {"mdash", 0x2014}, {"ndash", 0x2013},  {"lsquo", 0x2018},  {"rsquo", 0x2019},  {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"bull", 0x2022},   {"middot", 0xB7},   {"deg", 0xB0},      {"frac12", 0xBD},
    {"frac14", 0xBC},  {"frac34", 0xBE},   {"times", 0xD7},    {"divide", 0xF7},   {"euro", 0x20AC},
    {"pound", 0xA3},   {"yen", 0xA5},      {"cent", 0xA2},     {"sect", 0xA7},     {"para", 0xB6},
    {"laquo", 0xAB},   {"raquo", 0xBB},    {"shy", 0xAD},      {"eacute", 0xE9},   {"egrave", 0xE8},
    {"aacute", 0xE1},  {"agrave", 0xE0},   {"ntilde", 0xF1},   {"ouml", 0xF6},     {"uuml", 0xFC},
    {"auml", 0xE4},    {"ccedil", 0xE7},   {"iexcl", 0xA1},    {"iquest", 0xBF},   {"plusmn", 0xB1},
    {"micro", 0xB5},   {"ensp", 0x2002},   {"emsp", 0x2003},   {"thinsp", 0x2009}, {"zwj", 0x200D},
    {"zwnj", 0x200C},  {"larr", 0x2190},   {"rarr", 0x2192},   {"uarr", 0x2191},   {"darr", 0x2193},
    {"check", 0x2713}, {"star", 0x2606},   {"hearts", 0x2665}, {"sup2", 0xB2},     {"sup3", 0xB3},
    {"ordm", 0xBA},    {"ordf", 0xAA},     {"acute", 0xB4},    {"cedil", 0xB8},    {"macr", 0xAF},
};

std::string collapse(std::string_view s, size_t max) {
  std::string out;
  bool space = false;
  for (unsigned char c : s) {
    if (std::isspace(c)) {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += char(c);
    if (out.size() >= max) break;
  }
  return out;
}

// Cuts at a UTF-8 boundary.
std::string clip(std::string s, size_t max) {
  if (s.size() <= max) return s;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  s.resize(n);
  return s;
}

// ---------------------------------------------------------------- the tree builder

class Builder {
 public:
  explicit Builder(std::vector<Node>& nodes) : nodes_(nodes) {
    nodes_.push_back(Node{Node::Kind::kDocument, "", {}, "", -1, {}});
    stack_.push_back(0);
  }

  bool full() const { return nodes_.size() >= Document::kMaxNodes; }

  void text(std::string t) {
    if (t.empty() || full()) return;
    int parent = stack_.back();
    auto& kids = nodes_[size_t(parent)].children;
    if (!kids.empty() && nodes_[size_t(kids.back())].kind == Node::Kind::kText) {
      auto& prev = nodes_[size_t(kids.back())].text;
      if (prev.size() < (1u << 20)) prev += t;
      return;
    }
    add(Node{Node::Kind::kText, "", {}, std::move(t), parent, {}});
  }

  // Returns the new element, or -1 when the document is full.
  int start(const std::string& tag, std::vector<std::pair<std::string, std::string>> attrs, bool self_closing) {
    if (full()) return -1;
    if (closes_p(tag)) close_in_scope("p", {});
    if (tag == "li") close_in_scope("li", {"ul", "ol"});
    if (tag == "dt" || tag == "dd") {
      close_in_scope("dt", {"dl"});
      close_in_scope("dd", {"dl"});
    }
    if (tag == "tr") close_in_scope("tr", {"table", "thead", "tbody", "tfoot"});
    if (tag == "td" || tag == "th") {
      close_in_scope("td", {"tr", "table"});
      close_in_scope("th", {"tr", "table"});
    }
    if (tag == "thead" || tag == "tbody" || tag == "tfoot") {
      for (const char* t : {"thead", "tbody", "tfoot"}) close_in_scope(t, {"table"});
    }
    if (tag == "option" && top() == "option") truncate(stack_.size() - 1);
    if (tag == "optgroup") {
      if (top() == "option") truncate(stack_.size() - 1);
      if (top() == "optgroup") truncate(stack_.size() - 1);
    }
    if (is_heading(tag) && is_heading(top())) truncate(stack_.size() - 1);
    if (tag == "a") close_in_scope("a", {});  // anchors don't nest
    if (tag == "body") close_in_scope("head", {});
    int parent = stack_.back();
    int id = add(Node{Node::Kind::kElement, tag, std::move(attrs), "", parent, {}});
    if (!is_void(tag) && !self_closing && int(stack_.size()) <= Document::kMaxDepth) push(id);
    return id;
  }

  void end(const std::string& tag) {
    // Content after </body> or </html> still belongs to the body.
    if (tag == "html" || tag == "body") return;
    // The nearest open element of that name, not past what scopes it: a table's own parts
    // stop at the table, </table> closes everything in it, and the rest stop at cells.
    if (!open_count(tag)) return;
    bool table_part = in(tag, {"tr", "td", "th", "thead", "tbody", "tfoot", "caption"});
    for (size_t i = stack_.size(); i-- > 1;) {
      const auto& t = nodes_[size_t(stack_[i])].tag;
      if (t == tag) {
        truncate(i);
        return;
      }
      if (tag == "table" ? (t == "html" || t == "template") : table_part ? t == "table" : is_scope_boundary(t)) return;
    }
  }

  // Closes the element a raw-text start tag opened.
  void pop_if(int id) {
    if (!stack_.empty() && stack_.back() == id) truncate(stack_.size() - 1);
  }

 private:
  std::string_view top() const { return nodes_[size_t(stack_.back())].tag; }

  void close_in_scope(std::string_view tag, std::initializer_list<std::string_view> stop_at) {
    if (!open_count(tag)) return;  // nothing to find: skip the walk, which deep pages make long
    for (size_t i = stack_.size(); i-- > 1;) {
      const auto& t = nodes_[size_t(stack_[i])].tag;
      if (t == tag) {
        truncate(i);
        return;
      }
      if (is_scope_boundary(t) || in(t, stop_at)) return;
    }
  }

  int add(Node n) {
    int id = int(nodes_.size());
    int parent = n.parent;
    nodes_.push_back(std::move(n));
    nodes_[size_t(parent)].children.push_back(id);
    return id;
  }

  int open_count(std::string_view tag) const {
    auto it = open_.find(std::string(tag));
    return it == open_.end() ? 0 : it->second;
  }
  void push(int id) {
    stack_.push_back(id);
    ++open_[nodes_[size_t(id)].tag];
  }
  void truncate(size_t n) {
    while (stack_.size() > n) {
      --open_[nodes_[size_t(stack_.back())].tag];
      stack_.pop_back();
    }
  }

  std::vector<Node>& nodes_;
  std::vector<int> stack_;
  std::unordered_map<std::string, int> open_;  // open elements by tag
};

size_t find_ci(std::string_view hay, std::string_view needle, size_t from) {
  if (needle.empty() || hay.size() < needle.size()) return std::string_view::npos;
  for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
    bool ok = true;
    for (size_t j = 0; j < needle.size() && ok; ++j) {
      ok = std::tolower(static_cast<unsigned char>(hay[i + j])) == needle[j];
    }
    if (ok) return i;
  }
  return std::string_view::npos;
}

bool name_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == ':' || c == '_' || c == '.';
}

// ---------------------------------------------------------------- text and markdown writers

// Accumulates text, collapsing whitespace, with pending line breaks and a per-line prefix.
class Writer {
 public:
  explicit Writer(size_t cap) : cap_(cap) {}
  bool full() const { return out_.size() >= cap_; }

  void text(std::string_view t) {
    for (unsigned char c : t) {
      if (std::isspace(c)) {
        space_ = true;
        continue;
      }
      put(char(c));
    }
  }
  // Text as it is, line breaks and all (pre).
  void literal(std::string_view t) {
    flush();
    for (char c : t) {
      if (full()) return;
      out_ += c;
      if (c == '\n') out_ += prefix_;
    }
  }
  // A marker like ** or [: placed like text, but never split from what follows.
  void mark(std::string_view m) {
    flush_space();
    out_ += m;
  }
  void breaks(int n) { newlines_ = std::max(newlines_, n); }
  void push_prefix(std::string p) { prefixes_.push_back(prefix_); prefix_ += p; }
  void pop_prefix() {
    prefix_ = prefixes_.back();
    prefixes_.pop_back();
  }
  std::string done() {
    while (!out_.empty() && std::isspace(static_cast<unsigned char>(out_.back()))) out_.pop_back();
    return clip(std::move(out_), cap_);
  }

 private:
  void flush() {
    if (out_.empty()) {
      newlines_ = 0;
      space_ = false;
      return;
    }
    if (newlines_) {
      for (int i = 0; i < newlines_; ++i) {
        out_ += '\n';
        // A blank line inside a quote keeps the quote's marker, without trailing spaces.
        if (i + 1 < newlines_) {
          std::string bare = prefix_;
          while (!bare.empty() && bare.back() == ' ') bare.pop_back();
          out_ += bare;
        }
      }
      out_ += prefix_;
      newlines_ = 0;
      space_ = false;
    }
  }
  void flush_space() {
    bool had_breaks = newlines_ > 0;
    flush();
    if (space_ && !had_breaks && !out_.empty() && out_.back() != '\n' && out_.back() != ' ') out_ += ' ';
    space_ = false;
  }
  void put(char c) {
    if (full()) return;
    flush_space();
    out_ += c;
  }
  std::string out_, prefix_;
  std::vector<std::string> prefixes_;
  size_t cap_;
  int newlines_ = 0;
  bool space_ = false;
};

void write_text(const Document& doc, int id, Writer& w, bool pre, int depth) {
  const Node& n = doc.node(id);
  if (w.full() || depth > Document::kMaxDepth + 2) return;
  if (n.kind == Node::Kind::kText) {
    pre ? w.literal(n.text) : w.text(n.text);
    return;
  }
  if (n.kind == Node::Kind::kElement && is_hidden(n.tag)) return;
  int breaks = n.kind == Node::Kind::kElement ? (is_paragraph_like(n.tag) ? 2 : is_block(n.tag) ? 1 : 0) : 0;
  if (n.tag == "td" || n.tag == "th") w.text(" ");
  if (breaks) w.breaks(breaks);
  if (n.tag == "br") return;
  for (int c : n.children) write_text(doc, c, w, pre || n.tag == "pre", depth + 1);
  if (breaks) w.breaks(breaks);
  if (n.tag == "td" || n.tag == "th") w.text(" ");
}

struct Md {
  const Document& doc;
  const Url& base;
  Writer w;
  std::vector<std::pair<bool, int>> lists;  // ordered?, counter

  std::optional<std::string> link(const Node& n, const char* attr) {
    const std::string* v = n.attr(attr);
    if (!v) return std::nullopt;
    std::string_view s = *v;
    if (s.starts_with("mailto:") || s.starts_with("tel:")) return std::string(s);
    auto u = resolve(base, s);
    if (!u) return std::nullopt;
    return u->str();
  }

  void children(const Node& n, int depth) {
    for (int c : n.children) node(c, depth + 1);
  }

  void node(int id, int depth) {
    const Node& n = doc.node(id);
    if (w.full() || depth > Document::kMaxDepth + 2) return;
    if (n.kind == Node::Kind::kText) return w.text(n.text);
    if (n.kind == Node::Kind::kDocument) return children(n, depth);
    const std::string& t = n.tag;
    if (is_hidden(t)) return;
    if (is_heading(t)) {
      w.breaks(2);
      w.mark(std::string(size_t(t[1] - '0'), '#') + " ");
      children(n, depth);
      return w.breaks(2);
    }
    if (t == "br") return w.breaks(1);
    if (t == "hr") {
      w.breaks(2);
      w.mark("---");
      return w.breaks(2);
    }
    if (t == "a") {
      auto href = link(n, "href");
      if (!href) return children(n, depth);
      w.mark("[");
      children(n, depth);
      w.mark("](" + *href + ")");
      return;
    }
    if (t == "img") {
      auto src = link(n, "src");
      if (!src) return;
      const std::string* alt = n.attr("alt");
      w.mark("![" + collapse(alt ? *alt : "", 200) + "](" + *src + ")");
      return;
    }
    if (t == "strong" || t == "b" || t == "em" || t == "i") {
      std::string m = (t == "strong" || t == "b") ? "**" : "*";
      std::string inner = collapse(doc.text_of(id, 4096), 4096);
      if (inner.empty()) return;
      w.mark(m + inner + m);
      return;
    }
    if (t == "code") {
      w.mark("`" + collapse(doc.text_of(id, 4096), 4096) + "`");
      return;
    }
    if (t == "pre") {
      w.breaks(2);
      w.mark("```");
      w.breaks(1);
      std::string raw;
      std::function<void(int, int)> gather = [&](int c, int d) {
        if (d > Document::kMaxDepth + 2 || raw.size() > (256u << 10)) return;
        const Node& k = doc.node(c);
        if (k.kind == Node::Kind::kText) raw += k.text;
        for (int g : k.children) gather(g, d + 1);
      };
      gather(id, 0);
      while (!raw.empty() && raw.front() == '\n') raw.erase(0, 1);
      while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
      w.literal(raw);
      w.breaks(1);
      w.mark("```");
      return w.breaks(2);
    }
    if (t == "ul" || t == "ol") {
      w.breaks(lists.empty() ? 2 : 1);
      lists.emplace_back(t == "ol", 0);
      children(n, depth);
      lists.pop_back();
      return w.breaks(lists.empty() ? 2 : 1);
    }
    if (t == "li") {
      w.breaks(1);
      std::string marker = "- ";
      if (!lists.empty() && lists.back().first) marker = std::to_string(++lists.back().second) + ". ";
      w.mark(marker);
      w.push_prefix(std::string(marker.size(), ' '));
      children(n, depth);
      w.pop_prefix();
      return w.breaks(1);
    }
    if (t == "blockquote") {
      w.breaks(2);
      w.push_prefix("> ");
      w.mark("> ");
      children(n, depth);
      w.pop_prefix();
      return w.breaks(2);
    }
    if (t == "table") {
      w.breaks(2);
      auto rows = view_tables_one(id);
      bool first = true;
      for (const auto& row : rows) {
        std::string line = "|";
        for (const auto& cell : row) {
          std::string c = cell;
          std::replace(c.begin(), c.end(), '|', '/');
          line += " " + c + " |";
        }
        w.mark(line);
        w.breaks(1);
        if (first) {
          std::string sep = "|";
          for (size_t i = 0; i < row.size(); ++i) sep += " --- |";
          w.mark(sep);
          w.breaks(1);
          first = false;
        }
      }
      return w.breaks(2);
    }
    int breaks = is_paragraph_like(t) ? 2 : is_block(t) ? 1 : 0;
    if (breaks) w.breaks(breaks);
    children(n, depth);
    if (breaks) w.breaks(breaks);
  }

  // The rows of one table, each a list of cell texts (nested tables stay with their own).
  std::vector<std::vector<std::string>> view_tables_one(int table) {
    std::vector<std::vector<std::string>> rows;
    std::function<void(int, int)> walk = [&](int id, int d) {
      if (d > Document::kMaxDepth + 2 || rows.size() >= 500) return;
      const Node& n = doc.node(id);
      if (id != table && n.tag == "table") return;
      if (n.tag == "tr") {
        std::vector<std::string> cells;
        for (int c : n.children) {
          const Node& cell = doc.node(c);
          if (cell.tag == "td" || cell.tag == "th") cells.push_back(collapse(doc.text_of(c, 2000), 1000));
        }
        if (!cells.empty()) rows.push_back(std::move(cells));
        return;
      }
      for (int c : n.children) walk(c, d + 1);
    };
    walk(table, 0);
    return rows;
  }
};

}  // namespace

// ---------------------------------------------------------------- Node and Document

const std::string* Node::attr(std::string_view name) const {
  for (const auto& [k, v] : attrs) {
    if (k == name) return &v;
  }
  return nullptr;
}

bool Node::has_class(std::string_view cls) const {
  const std::string* c = attr("class");
  if (!c || cls.empty()) return false;
  std::string_view s = *c;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = i;
    while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
    if (s.substr(i, j - i) == cls) return true;
    i = j;
  }
  return false;
}

std::string decode_entities(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (c != '&') {
      out += c;
      ++i;
      continue;
    }
    size_t j = i + 1;
    if (j < s.size() && s[j] == '#') {
      ++j;
      bool hex = j < s.size() && (s[j] == 'x' || s[j] == 'X');
      if (hex) ++j;
      size_t start = j;
      uint32_t cp = 0;
      while (j < s.size() && j - start < 8 && (hex ? std::isxdigit(static_cast<unsigned char>(s[j])) : std::isdigit(static_cast<unsigned char>(s[j])))) {
        cp = cp * (hex ? 16 : 10) + uint32_t(std::isdigit(static_cast<unsigned char>(s[j])) ? s[j] - '0' : (std::tolower(static_cast<unsigned char>(s[j])) - 'a' + 10));
        ++j;
      }
      if (j == start) {
        out += '&';
        ++i;
        continue;
      }
      if (j < s.size() && s[j] == ';') ++j;
      append_utf8(out, cp);
      i = j;
      continue;
    }
    size_t start = j;
    while (j < s.size() && j - start < 32 && std::isalnum(static_cast<unsigned char>(s[j]))) ++j;
    std::string_view name = s.substr(start, j - start);
    bool semi = j < s.size() && s[j] == ';';
    const Named* found = nullptr;
    for (const auto& e : kNamed) {
      if (name == e.name) found = &e;
    }
    // Without a semicolon, only the few that browsers accept that way.
    if (found && (semi || in(name, {"amp", "lt", "gt", "quot", "nbsp", "copy", "reg"}))) {
      append_utf8(out, found->cp);
      i = j + (semi ? 1 : 0);
      continue;
    }
    out += '&';
    ++i;
  }
  return out;
}

Document Document::parse(std::string_view s) {
  Document doc;
  Builder b(doc.nodes_);
  size_t i = 0;
  // A UTF-8 byte order mark isn't text.
  if (s.starts_with("\xEF\xBB\xBF")) i = 3;
  while (i < s.size() && !b.full()) {
    if (s[i] != '<') {
      size_t next = s.find('<', i);
      if (next == std::string_view::npos) next = s.size();
      b.text(decode_entities(s.substr(i, next - i)));
      i = next;
      continue;
    }
    if (s.substr(i, 4) == "<!--") {
      size_t close = s.find("-->", i + 4);
      i = close == std::string_view::npos ? s.size() : close + 3;
      continue;
    }
    if (i + 1 < s.size() && (s[i + 1] == '!' || s[i + 1] == '?')) {
      size_t close = s.find('>', i);
      i = close == std::string_view::npos ? s.size() : close + 1;
      continue;
    }
    if (i + 1 < s.size() && s[i + 1] == '/') {
      size_t j = i + 2;
      while (j < s.size() && name_char(s[j])) ++j;
      std::string name = lower(s.substr(i + 2, j - i - 2));
      size_t close = s.find('>', j);
      i = close == std::string_view::npos ? s.size() : close + 1;
      if (!name.empty()) b.end(name);
      continue;
    }
    if (i + 1 >= s.size() || !std::isalpha(static_cast<unsigned char>(s[i + 1]))) {
      b.text("<");
      ++i;
      continue;
    }
    // A start tag: its name, then attributes until >.
    size_t j = i + 1;
    while (j < s.size() && name_char(s[j])) ++j;
    std::string tag = lower(s.substr(i + 1, j - i - 1));
    std::vector<std::pair<std::string, std::string>> attrs;
    bool self_closing = false;
    while (j < s.size() && s[j] != '>') {
      char c = s[j];
      if (std::isspace(static_cast<unsigned char>(c))) {
        ++j;
        continue;
      }
      if (c == '/') {
        self_closing = j + 1 < s.size() && s[j + 1] == '>';
        ++j;
        continue;
      }
      size_t k = j;
      while (k < s.size() && !std::isspace(static_cast<unsigned char>(s[k])) && s[k] != '=' && s[k] != '>' &&
             !(s[k] == '/' && k + 1 < s.size() && s[k + 1] == '>')) {
        ++k;
      }
      if (k == j) {  // a stray '=' with no name
        ++j;
        continue;
      }
      std::string name = lower(s.substr(j, k - j));
      j = k;
      while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
      std::string value;
      if (j < s.size() && s[j] == '=') {
        ++j;
        while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        if (j < s.size() && (s[j] == '"' || s[j] == '\'')) {
          char q = s[j];
          size_t end = s.find(q, j + 1);
          if (end == std::string_view::npos) end = s.size();
          value = decode_entities(s.substr(j + 1, std::min<size_t>(end - j - 1, 8192)));
          j = end + 1;
        } else {
          size_t end = j;
          while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end])) && s[end] != '>') ++end;
          value = decode_entities(s.substr(j, std::min<size_t>(end - j, 8192)));
          j = end;
        }
      }
      bool dup = false;
      for (const auto& a : attrs) dup |= a.first == name;
      if (!dup && attrs.size() < 64) attrs.emplace_back(std::move(name), std::move(value));
    }
    i = j < s.size() ? j + 1 : s.size();
    int id = b.start(tag, std::move(attrs), self_closing);
    if (id < 0) break;
    if ((is_raw(tag) || is_rcdata(tag)) && !self_closing) {
      // Everything up to the matching end tag is this element's text.
      size_t end = find_ci(s, "</" + tag, i);
      if (end == std::string_view::npos) end = s.size();
      std::string_view body = s.substr(i, end - i);
      b.text(is_raw(tag) ? std::string(body) : decode_entities(body));
      size_t close = end < s.size() ? s.find('>', end) : std::string_view::npos;
      i = close == std::string_view::npos ? s.size() : close + 1;
      b.pop_if(id);
    }
  }
  return doc;
}

Url Document::base_for(const Url& page) const {
  for (int id : elements("base")) {
    if (const std::string* href = node(id).attr("href")) {
      if (auto u = resolve(page, *href)) return *u;
    }
  }
  return page;
}

std::vector<int> Document::elements(std::string_view tag) const {
  std::vector<int> out;
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i].kind == Node::Kind::kElement && nodes_[i].tag == tag) out.push_back(int(i));
  }
  return out;
}

std::string Document::text_of(int id, size_t max_bytes) const {
  Writer w(max_bytes);
  const Node& n = node(id);
  // Asked for directly, a hidden element still gives its text (a <title>, say), but what's
  // hidden inside it doesn't.
  if (n.kind == Node::Kind::kElement && is_hidden(n.tag)) {
    for (int c : n.children) write_text(*this, c, w, false, 1);
  } else {
    write_text(*this, id, w, false, 0);
  }
  return w.done();
}

std::string Document::outer_html(int id, size_t max_bytes) const {
  std::string out;
  std::function<void(int, int, bool)> walk = [&](int i, int depth, bool raw) {
    if (out.size() >= max_bytes || depth > kMaxDepth + 2) return;
    const Node& n = node(i);
    if (n.kind == Node::Kind::kText) {
      out += raw ? n.text : base::html_escape(n.text);
      return;
    }
    if (n.kind == Node::Kind::kElement) {
      out += "<" + n.tag;
      for (const auto& [k, v] : n.attrs) out += " " + k + "=\"" + base::html_escape(v) + "\"";
      out += ">";
    }
    for (int c : n.children) walk(c, depth + 1, is_raw(n.tag));
    if (n.kind == Node::Kind::kElement && !is_void(n.tag)) out += "</" + n.tag + ">";
  };
  walk(id, 0, false);
  return clip(std::move(out), max_bytes);
}

// ---------------------------------------------------------------- selectors

std::optional<Selector> Selector::parse(std::string_view text, std::string* error) {
  if (text.size() > 1024) return *error = "a selector is at most 1024 bytes", std::nullopt;
  Selector sel;
  size_t i = 0;
  auto ident = [&]() {
    size_t start = i;
    while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '-' || text[i] == '_' ||
                               static_cast<unsigned char>(text[i]) >= 0x80)) {
      ++i;
    }
    return std::string(text.substr(start, i - start));
  };
  auto skip_ws = [&] {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  };
  std::vector<Compound> chain;
  char pending = 0;
  skip_ws();
  while (true) {
    Compound c;
    c.combinator = chain.empty() ? 0 : (pending ? pending : ' ');
    bool any = false;
    if (i < text.size() && text[i] == '*') {
      ++i;
      any = true;
    } else if (i < text.size() && (std::isalpha(static_cast<unsigned char>(text[i])))) {
      c.tag = lower(ident());
      any = true;
    }
    while (i < text.size()) {
      char ch = text[i];
      if (ch == '#' || ch == '.') {
        ++i;
        std::string name = ident();
        if (name.empty()) return *error = std::string("a name must follow ") + ch, std::nullopt;
        (ch == '#' ? c.ids : c.classes).push_back(name);
        any = true;
      } else if (ch == '[') {
        ++i;
        skip_ws();
        Attr a;
        a.name = lower(ident());
        if (a.name.empty()) return *error = "an attribute selector needs a name", std::nullopt;
        skip_ws();
        if (i < text.size() && text[i] != ']') {
          if (text[i] == '=') {
            a.op = "=";
            ++i;
          } else if (i + 1 < text.size() && text[i + 1] == '=' && std::strchr("~|^$*", text[i])) {
            a.op = std::string(1, text[i]) + "=";
            i += 2;
          } else {
            return *error = "unknown attribute operator", std::nullopt;
          }
          skip_ws();
          if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
            char q = text[i++];
            size_t end = text.find(q, i);
            if (end == std::string_view::npos) return *error = "unclosed quote", std::nullopt;
            a.value = std::string(text.substr(i, end - i));
            i = end + 1;
          } else {
            a.value = ident();
          }
          skip_ws();
        }
        if (i >= text.size() || text[i] != ']') return *error = "expected ]", std::nullopt;
        ++i;
        c.attrs.push_back(std::move(a));
        any = true;
      } else if (ch == ':') {
        ++i;
        std::string p = lower(ident());
        if (p == "first-child") {
          c.nth = 1;
        } else if (p == "last-child") {
          c.last = true;
        } else if (p == "nth-child" && i < text.size() && text[i] == '(') {
          size_t close = text.find(')', i);
          if (close == std::string_view::npos) return *error = "unclosed (", std::nullopt;
          std::string n(text.substr(i + 1, close - i - 1));
          c.nth = std::atoi(n.c_str());
          if (c.nth < 1 || c.nth > 100000) return *error = ":nth-child takes a plain number, like 2", std::nullopt;
          i = close + 1;
        } else {
          return *error = ":" + p + " isn't supported; use :first-child, :last-child, or :nth-child(n)", std::nullopt;
        }
        any = true;
      } else {
        break;
      }
    }
    if (!any) return *error = "expected a selector at byte " + std::to_string(i), std::nullopt;
    chain.push_back(std::move(c));
    if (chain.size() > 32) return *error = "a selector chain is at most 32 parts", std::nullopt;
    size_t before = i;
    skip_ws();
    pending = 0;
    if (i >= text.size()) break;
    if (text[i] == ',') {
      ++i;
      skip_ws();
      sel.groups_.push_back(std::move(chain));
      chain.clear();
      if (sel.groups_.size() >= 63) return *error = "a selector has at most 64 comma-separated parts", std::nullopt;
      continue;
    }
    if (text[i] == '>') {
      pending = '>';
      ++i;
      skip_ws();
      continue;
    }
    if (text[i] == '+' || text[i] == '~') return *error = "the + and ~ combinators aren't supported", std::nullopt;
    if (i == before) return *error = "unexpected '" + std::string(1, text[i]) + "'", std::nullopt;
    pending = ' ';
  }
  if (chain.empty()) return *error = "an empty selector", std::nullopt;
  sel.groups_.push_back(std::move(chain));
  return sel;
}

bool Selector::match_compound(const Document& doc, int id, const Compound& c) const {
  const Node& n = doc.node(id);
  if (n.kind != Node::Kind::kElement) return false;
  if (!c.tag.empty() && c.tag != n.tag) return false;
  for (const auto& want : c.ids) {
    const std::string* v = n.attr("id");
    if (!v || *v != want) return false;
  }
  for (const auto& cls : c.classes) {
    if (!n.has_class(cls)) return false;
  }
  for (const auto& a : c.attrs) {
    const std::string* v = n.attr(a.name);
    if (!v) return false;
    std::string_view s = *v;
    const std::string& w = a.value;
    if (a.op == "=" && s != w) return false;
    if (a.op == "^=" && (w.empty() || !s.starts_with(w))) return false;
    if (a.op == "$=" && (w.empty() || !s.ends_with(w))) return false;
    if (a.op == "*=" && (w.empty() || s.find(w) == std::string_view::npos)) return false;
    if (a.op == "|=" && !(s == w || s.starts_with(w + "-"))) return false;
    if (a.op == "~=") {
      Node probe;
      probe.attrs = {{"class", std::string(s)}};
      if (!probe.has_class(w)) return false;
    }
  }
  if (c.nth || c.last) {
    const auto& siblings = doc.node(n.parent).children;
    int index = 0, count = 0, mine = 0;
    for (int sib : siblings) {
      if (doc.node(sib).kind != Node::Kind::kElement) continue;
      ++count;
      if (sib == id) mine = ++index;
      else if (!mine) ++index;
    }
    if (c.nth && mine != c.nth) return false;
    if (c.last && mine != count) return false;
  }
  return true;
}

bool Selector::match_from(const Document& doc, int id, const std::vector<Compound>& chain, size_t at,
                          std::unordered_map<int64_t, bool>& memo) const {
  // Right to left: this node matches chain[at], and its ancestors match what's before it.
  // Memoized per (node, position), so nested descendant chains stay polynomial.
  int64_t key = (int64_t(id) * 64 + int64_t(at)) * 64 + int64_t(&chain - groups_.data());
  if (auto it = memo.find(key); it != memo.end()) return it->second;
  bool result = false;
  if (match_compound(doc, id, chain[at])) {
    if (at == 0) {
      result = true;
    } else if (chain[at].combinator == '>') {
      int p = doc.node(id).parent;
      result = p > 0 && match_from(doc, p, chain, at - 1, memo);
    } else {
      for (int p = doc.node(id).parent; p > 0 && !result; p = doc.node(p).parent) {
        result = match_from(doc, p, chain, at - 1, memo);
      }
    }
  }
  memo[key] = result;
  return result;
}

bool Selector::matches(const Document& doc, int id) const {
  std::unordered_map<int64_t, bool> memo;
  for (const auto& chain : groups_) {
    if (match_from(doc, id, chain, chain.size() - 1, memo)) return true;
  }
  return false;
}

std::vector<int> Selector::select(const Document& doc, int scope, size_t limit) const {
  std::vector<int> out;
  std::vector<int> stack(doc.node(scope).children.rbegin(), doc.node(scope).children.rend());
  // One memo for the whole walk: the same ancestors are asked about again and again.
  std::unordered_map<int64_t, bool> memo;
  while (!stack.empty() && out.size() < limit) {
    int id = stack.back();
    stack.pop_back();
    const Node& n = doc.node(id);
    if (n.kind != Node::Kind::kElement) continue;
    for (const auto& chain : groups_) {
      if (match_from(doc, id, chain, chain.size() - 1, memo)) {
        out.push_back(id);
        break;
      }
    }
    for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
  }
  return out;
}

// ---------------------------------------------------------------- views

std::string view_text(const Document& doc) { return doc.text_of(doc.root(), 1 << 20); }

std::string view_markdown(const Document& doc, const Url& base) {
  Md md{doc, base, Writer(1 << 20), {}};
  md.node(doc.root(), 0);
  return md.w.done();
}

bj::array view_links(const Document& doc, const Url& base) {
  bj::array out;
  std::set<std::string> seen;
  for (int id : doc.elements("a")) {
    if (out.size() >= 2000) break;
    const Node& n = doc.node(id);
    const std::string* href = n.attr("href");
    if (!href) continue;
    std::string url;
    std::string_view h = *href;
    while (!h.empty() && std::isspace(static_cast<unsigned char>(h.front()))) h.remove_prefix(1);
    if (h.starts_with("mailto:") || h.starts_with("tel:")) {
      url = collapse(h, 512);
    } else if (auto u = resolve(base, h)) {
      url = u->str();
    } else {
      continue;
    }
    if (!seen.insert(url).second) continue;
    bj::object link{{"url", url}, {"text", collapse(doc.text_of(id, 1000), 200)}};
    if (const std::string* rel = n.attr("rel")) link["rel"] = *rel;
    out.push_back(std::move(link));
  }
  return out;
}

bj::object view_meta(const Document& doc, const Url& base) {
  bj::object out;
  auto first_text = [&](std::string_view tag) -> std::string {
    auto els = doc.elements(tag);
    return els.empty() ? "" : collapse(doc.text_of(els[0], 4096), 1000);
  };
  out["title"] = first_text("title");
  for (int id : doc.elements("html")) {
    if (const std::string* lang = doc.node(id).attr("lang")) out["lang"] = *lang;
    break;
  }
  bj::object og, twitter;
  std::string platform;
  auto note_platform = [&](std::string_view s) {
    if (!platform.empty()) return;
    std::string l = lower(s.substr(0, 2048));
    struct P {
      const char* needle;
      const char* name;
    };
    static constexpr P kPlatforms[] = {
        {"cdn.shopify.com", "shopify"},     {"shopify", "shopify"},           {"wix.com", "wix"},
        {"squarespace", "squarespace"},     {"wp-content", "wordpress"},       {"wordpress", "wordpress"},
        {"localline", "localline"},         {"barn2door", "barn2door"},        {"square.site", "square"},
        {"squareup.com", "square"},         {"bigcommerce", "bigcommerce"},    {"woocommerce", "woocommerce"},
        {"godaddy", "godaddy"},             {"weebly", "weebly"},              {"webflow", "webflow"},
        {"grazecart", "grazecart"},         {"farmmatch", "farmmatch"},        {"harvie", "harvie"},
    };
    for (const auto& p : kPlatforms) {
      if (l.find(p.needle) != std::string::npos) {
        platform = p.name;
        return;
      }
    }
  };
  for (int id : doc.elements("meta")) {
    const Node& n = doc.node(id);
    const std::string* content = n.attr("content");
    if (!content) continue;
    std::string name = n.attr("name") ? lower(*n.attr("name")) : "";
    std::string property = n.attr("property") ? lower(*n.attr("property")) : "";
    std::string value = collapse(*content, 2000);
    if (name == "description") out["description"] = value;
    if (name == "keywords") out["keywords"] = value;
    if (name == "robots") out["robots"] = value;
    if (name == "generator") {
      out["generator"] = value;
      note_platform(value);
    }
    if (property.starts_with("og:") && og.size() < 50) og[property.substr(3)] = value;
    if (name.starts_with("twitter:") && twitter.size() < 50) twitter[name.substr(8)] = value;
    if (property.starts_with("product:") && og.size() < 50) og[property] = value;
  }
  if (!og.empty()) out["og"] = std::move(og);
  if (!twitter.empty()) out["twitter"] = std::move(twitter);
  bj::array feeds;
  for (int id : doc.elements("link")) {
    const Node& n = doc.node(id);
    const std::string* href = n.attr("href");
    if (!href) continue;
    note_platform(*href);
    std::string rel = n.attr("rel") ? lower(*n.attr("rel")) : "";
    auto u = resolve(base, *href);
    if (!u) continue;
    if (rel == "canonical") out["canonical"] = u->str();
    if (rel.find("icon") != std::string::npos && !out.contains("icon")) out["icon"] = u->str();
    std::string type = n.attr("type") ? lower(*n.attr("type")) : "";
    if (rel == "alternate" && (type.find("rss") != std::string::npos || type.find("atom") != std::string::npos) &&
        feeds.size() < 20) {
      feeds.push_back(bj::object{{"url", u->str()}, {"type", type}});
    }
  }
  if (!feeds.empty()) out["feeds"] = std::move(feeds);
  for (int id : doc.elements("script")) {
    if (const std::string* src = doc.node(id).attr("src")) note_platform(*src);
  }
  if (!platform.empty()) out["platform"] = platform;
  return out;
}

bj::array view_jsonld(const Document& doc) {
  bj::array out;
  size_t bytes = 0;
  auto add = [&](const bj::value& v) {
    if (out.size() >= 200 || bytes > (1u << 20)) return;
    if (v.is_object()) {
      bytes += json::dump(v).size();
      out.push_back(v);
    }
  };
  for (int id : doc.elements("script")) {
    const Node& n = doc.node(id);
    const std::string* type = n.attr("type");
    if (!type || lower(*type).find("ld+json") == std::string::npos) continue;
    std::string text;
    for (int c : n.children) text += doc.node(c).text;
    // Some sites wrap it in comments or CDATA, or leave a trailing semicolon.
    std::string_view t = text;
    while (!t.empty() && (std::isspace(static_cast<unsigned char>(t.front())) || t.front() == ';')) t.remove_prefix(1);
    while (!t.empty() && (std::isspace(static_cast<unsigned char>(t.back())) || t.back() == ';')) t.remove_suffix(1);
    if (t.starts_with("<!--")) t.remove_prefix(4);
    if (t.ends_with("-->")) t.remove_suffix(3);
    std::string err;
    auto v = json::parse(t, &err);
    if (!v) continue;
    auto flatten = [&](const bj::value& item) {
      if (item.is_object() && item.as_object().contains("@graph") && item.as_object().at("@graph").is_array()) {
        for (const auto& g : item.as_object().at("@graph").as_array()) add(g);
      } else {
        add(item);
      }
    };
    if (v->is_array()) {
      for (const auto& item : v->as_array()) flatten(item);
    } else {
      flatten(*v);
    }
  }
  return out;
}

bj::array view_tables(const Document& doc) {
  bj::array out;
  Url none;
  Md md{doc, none, Writer(16), {}};
  for (int id : doc.elements("table")) {
    if (out.size() >= 50) break;
    auto rows = md.view_tables_one(id);
    if (rows.empty()) continue;
    bj::object t;
    for (int c : doc.node(id).children) {
      if (doc.node(c).tag == "caption") t["caption"] = collapse(doc.text_of(c, 1000), 500);
    }
    // A first row of header cells is the headers.
    bool header = false;
    std::function<int(int, int)> first_tr = [&](int n, int d) -> int {
      if (d > Document::kMaxDepth) return -1;
      for (int c : doc.node(n).children) {
        if (doc.node(c).tag == "table") continue;
        if (doc.node(c).tag == "tr") return c;
        if (int r = first_tr(c, d + 1); r >= 0) return r;
      }
      return -1;
    };
    if (int tr = first_tr(id, 0); tr >= 0) {
      bool all_th = true, any = false;
      for (int c : doc.node(tr).children) {
        if (doc.node(c).tag == "td") all_th = false;
        if (doc.node(c).tag == "th") any = true;
      }
      header = any && all_th;
      if (doc.node(doc.node(tr).parent).tag == "thead") header = true;
    }
    bj::array rows_json;
    size_t start = 0;
    if (header) {
      bj::array h;
      for (const auto& c : rows[0]) h.emplace_back(c);
      t["headers"] = std::move(h);
      start = 1;
    }
    for (size_t r = start; r < rows.size(); ++r) {
      bj::array row;
      for (const auto& c : rows[r]) row.emplace_back(c);
      rows_json.push_back(std::move(row));
    }
    t["rows"] = std::move(rows_json);
    out.push_back(std::move(t));
  }
  return out;
}

std::optional<bj::array> view_select(const Document& doc, std::string_view selector, std::string* error) {
  auto sel = Selector::parse(selector, error);
  if (!sel) return std::nullopt;
  bj::array out;
  size_t bytes = 0;
  for (int id : sel->select(doc, doc.root(), 500)) {
    if (bytes > (2u << 20)) break;
    const Node& n = doc.node(id);
    bj::object attrs;
    for (const auto& [k, v] : n.attrs) attrs[k] = clip(v, 2000);
    bj::object o{{"tag", n.tag}, {"text", clip(doc.text_of(id, 4000), 2000)}, {"attrs", std::move(attrs)},
                 {"html", doc.outer_html(id, 8192)}};
    bytes += json::dump(o).size();
    out.push_back(std::move(o));
  }
  return out;
}

std::optional<bj::array> view_extract(const Document& doc, const Url& base, const bj::value& spec, std::string* error) {
  if (!spec.is_object()) return *error = "extract is {\"items\": selector, \"fields\": {name: \"sel\" or \"sel@attr\"}}", std::nullopt;
  const auto& o = spec.as_object();
  const auto* items = o.if_contains("items");
  const auto* fields = o.if_contains("fields");
  if (!items || !items->is_string()) return *error = "extract.items must be a selector", std::nullopt;
  if (!fields || !fields->is_object() || fields->as_object().empty() || fields->as_object().size() > 50) {
    return *error = "extract.fields must name 1 to 50 fields", std::nullopt;
  }
  auto item_sel = Selector::parse(items->as_string(), error);
  if (!item_sel) return *error = "extract.items: " + *error, std::nullopt;
  struct Field {
    std::string name, attr;
    std::optional<Selector> sel;
  };
  std::vector<Field> fs;
  for (const auto& f : fields->as_object()) {
    if (!f.value().is_string()) return *error = "extract.fields." + std::string(f.key()) + " must be a string", std::nullopt;
    std::string s(f.value().as_string());
    Field field{std::string(f.key()), "", std::nullopt};
    if (auto at = s.rfind('@'); at != std::string::npos) {
      field.attr = lower(s.substr(at + 1));
      s = s.substr(0, at);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (!s.empty()) {
      field.sel = Selector::parse(s, error);
      if (!field.sel) return *error = "extract.fields." + field.name + ": " + *error, std::nullopt;
    }
    fs.push_back(std::move(field));
  }
  bj::array out;
  for (int item : item_sel->select(doc, doc.root(), 1000)) {
    bj::object rec;
    for (const auto& f : fs) {
      int target = item;
      if (f.sel) {
        auto hits = f.sel->select(doc, item, 1);
        target = hits.empty() ? -1 : hits[0];
      }
      if (target < 0) {
        rec[f.name] = nullptr;
        continue;
      }
      if (f.attr.empty()) {
        rec[f.name] = collapse(doc.text_of(target, 8000), 4000);
        continue;
      }
      const std::string* v = doc.node(target).attr(f.attr);
      if (!v) {
        rec[f.name] = nullptr;
      } else if (f.attr == "href" || f.attr == "src") {
        auto u = resolve(base, *v);
        rec[f.name] = u ? bj::value(u->str()) : bj::value(*v);
      } else {
        rec[f.name] = clip(*v, 4000);
      }
    }
    out.push_back(std::move(rec));
  }
  return out;
}

}  // namespace goober::web
