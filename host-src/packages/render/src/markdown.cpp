#include "goober/render/markdown.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <list>
#include <map>
#include <vector>

#include "goober/base/encoding.h"

namespace goober::render {
namespace {

using Lines = std::vector<std::string>;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool is_punct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }
bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

std::string esc(std::string_view s) { return base::html_escape(s); }

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

size_t indent_of(std::string_view l) {
  size_t n = 0;
  while (n < l.size() && l[n] == ' ') ++n;
  return n;
}

bool blank(std::string_view l) { return trim(l).empty(); }

// Splits text into lines, expanding tabs to the next multiple of four columns.
Lines split_lines(std::string_view text) {
  Lines out;
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      out.push_back(std::move(cur));
      cur.clear();
    } else if (c == '\r') {
    } else if (c == '\t') {
      cur.append(4 - cur.size() % 4, ' ');
    } else if (c == '\0') {
      cur += "\xEF\xBF\xBD";
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// --- Block shapes. Each takes a whole line and allows up to three spaces of indent. ---

bool atx_heading(std::string_view l, int* level, std::string* text) {
  size_t i = indent_of(l);
  if (i > 3) return false;
  size_t n = 0;
  while (i + n < l.size() && l[i + n] == '#') ++n;
  if (n < 1 || n > 6) return false;
  if (i + n < l.size() && l[i + n] != ' ') return false;
  std::string_view rest = trim(l.substr(i + n));
  // A closing run of #s is dropped when it's preceded by a space, or is everything.
  size_t end = rest.size();
  while (end > 0 && rest[end - 1] == '#') --end;
  if (end == 0) {
    rest = {};
  } else if (end < rest.size() && rest[end - 1] == ' ') {
    rest = trim(rest.substr(0, end));
  }
  *level = int(n);
  *text = std::string(rest);
  return true;
}

bool thematic_break(std::string_view l) {
  if (indent_of(l) > 3) return false;
  char ch = 0;
  int count = 0;
  for (char c : l) {
    if (c == ' ') continue;
    if (c != '-' && c != '*' && c != '_') return false;
    if (ch && c != ch) return false;
    ch = c;
    ++count;
  }
  return count >= 3;
}

struct Fence {
  char ch = 0;
  size_t len = 0;
  size_t indent = 0;
  std::string info;
};

bool fence_open(std::string_view l, Fence* f) {
  size_t i = indent_of(l);
  if (i > 3 || i >= l.size() || (l[i] != '`' && l[i] != '~')) return false;
  size_t n = 0;
  while (i + n < l.size() && l[i + n] == l[i]) ++n;
  if (n < 3) return false;
  std::string_view info = trim(l.substr(i + n));
  if (l[i] == '`' && info.find('`') != std::string_view::npos) return false;
  f->ch = l[i];
  f->len = n;
  f->indent = i;
  f->info = std::string(info.substr(0, info.find(' ')));
  return true;
}

bool fence_close(std::string_view l, const Fence& f) {
  size_t i = indent_of(l);
  if (i > 3) return false;
  size_t n = 0;
  while (i + n < l.size() && l[i + n] == f.ch) ++n;
  return n >= f.len && blank(l.substr(i + n));
}

bool quote_line(std::string_view l) {
  size_t i = indent_of(l);
  return i <= 3 && i < l.size() && l[i] == '>';
}

struct Marker {
  bool ordered = false;
  char ch = 0;      // the bullet, or the ordered delimiter "." or ")"
  long start = 1;
  size_t offset = 0;  // the column where the item's content starts
  bool empty = false;  // nothing after the marker
};

bool list_marker(std::string_view l, Marker* m) {
  size_t i = indent_of(l);
  if (i > 3 || i >= l.size()) return false;
  size_t end = i;
  if (l[i] == '-' || l[i] == '+' || l[i] == '*') {
    m->ordered = false;
    m->ch = l[i];
    end = i + 1;
  } else {
    size_t d = i;
    while (d < l.size() && d - i < 9 && std::isdigit(static_cast<unsigned char>(l[d]))) ++d;
    if (d == i || d >= l.size() || (l[d] != '.' && l[d] != ')')) return false;
    m->ordered = true;
    m->ch = l[d];
    m->start = std::stol(std::string(l.substr(i, d - i)));
    end = d + 1;
  }
  if (end < l.size() && l[end] != ' ') return false;
  size_t spaces = 0;
  while (end + spaces < l.size() && l[end + spaces] == ' ') ++spaces;
  m->empty = end + spaces >= l.size();
  m->offset = (m->empty || spaces > 4) ? end + 1 : end + spaces;
  return true;
}

bool html_block_start(std::string_view l) {
  size_t i = indent_of(l);
  if (i > 3 || i + 1 >= l.size() || l[i] != '<') return false;
  char c = l[i + 1];
  return std::isalpha(static_cast<unsigned char>(c)) || c == '/' || c == '!' || c == '?';
}

std::vector<std::string> table_cells(std::string_view l) {
  std::string_view s = trim(l);
  if (!s.empty() && s.front() == '|') s.remove_prefix(1);
  if (!s.empty() && s.back() == '|' && (s.size() < 2 || s[s.size() - 2] != '\\')) s.remove_suffix(1);
  std::vector<std::string> cells;
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') {
      cur += '|';
      ++i;
    } else if (s[i] == '|') {
      cells.emplace_back(trim(cur));
      cur.clear();
    } else {
      cur += s[i];
    }
  }
  cells.emplace_back(trim(cur));
  return cells;
}

bool table_delimiter(std::string_view l, std::vector<std::string>* aligns) {
  if (l.find('-') == std::string_view::npos || indent_of(l) > 3) return false;
  aligns->clear();
  for (const auto& c : table_cells(l)) {
    if (c.empty()) return false;
    bool left = c.front() == ':';
    bool right = c.back() == ':';
    std::string_view dashes(c);
    if (left) dashes.remove_prefix(1);
    if (right && !dashes.empty()) dashes.remove_suffix(1);
    if (dashes.empty()) return false;
    for (char d : dashes) {
      if (d != '-') return false;
    }
    aligns->push_back(left && right ? "center" : right ? "right" : left ? "left" : "");
  }
  return true;
}

// A link label, normalized for matching: whitespace collapsed, lowercased.
std::string normalize_label(std::string_view s) {
  std::string out;
  bool space = false;
  for (char c : trim(s)) {
    if (is_space(c)) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += char(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// A URL that markdown may link to: http, https, mailto, tel, or a scheme-less
// reference; images may also be data:image/ of a raster type. Anything else is "#".
std::string safe_url(std::string_view url, bool image) {
  url = trim(url);
  size_t colon = url.find(':');
  size_t stop = url.find_first_of("/?#");
  if (colon != std::string_view::npos && (stop == std::string_view::npos || colon < stop)) {
    std::string scheme = base::to_lower(url.substr(0, colon));
    bool ok = scheme == "http" || scheme == "https" || scheme == "mailto" || scheme == "tel";
    if (image && scheme == "data") {
      std::string head = base::to_lower(url.substr(0, 20));
      ok = head.starts_with("data:image/png") || head.starts_with("data:image/gif") ||
           head.starts_with("data:image/jpeg") || head.starts_with("data:image/webp");
    }
    if (!ok) return "#";
  }
  std::string out;
  for (char c : url) {
    if (c == ' ') {
      out += "%20";
    } else {
      out += c;
    }
  }
  return esc(out);
}

std::string decode_entities(std::string_view s) {
  static const std::pair<const char*, const char*> kEntities[] = {
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&#x27;", "'"},
      {"&nbsp;", " "}, {"&apos;", "'"}};
  std::string out;
  for (size_t i = 0; i < s.size();) {
    bool hit = false;
    if (s[i] == '&') {
      for (const auto& [e, d] : kEntities) {
        if (s.substr(i).starts_with(e)) {
          out += d;
          i += std::string_view(e).size();
          hit = true;
          break;
        }
      }
    }
    if (!hit) out += s[i++];
  }
  return out;
}

struct Ref {
  std::string url;
  std::string title;
};

// Bounds on one document, so no input can exhaust the stack or the clock: blocks nest at
// most kMaxNesting deep (deeper markers are text), and the inline scans that can go
// quadratic (emphasis, brackets, link destinations) share one budget of steps, past which
// what remains is left as literal text.
constexpr int kMaxNesting = 32;
constexpr int64_t kWorkBudget = 20'000'000;

struct Ctx {
  const MarkdownOptions& o;
  std::map<std::string, Ref> refs;
  std::map<std::string, int> ids;
  int depth = 0;
  int64_t work = 0;
  bool spent() { return ++work > kWorkBudget; }
};

// --- Inlines ---

struct Inl {
  enum Kind { kText, kHtml, kDelim, kOpen };
  Kind k = kText;
  std::string s;
  char ch = 0;
  int count = 0;
  int orig = 0;
  bool can_open = false;
  bool can_close = false;
  bool image = false;
  bool active = true;
  size_t src = 0;  // for kOpen: where the label text starts in the source
};

using InlList = std::list<Inl>;

class Inline {
 public:
  Inline(Ctx& ctx, std::string_view t) : ctx_(ctx), t_(t) {}

  std::string render() {
    size_t n = t_.size();
    size_t i = 0;
    while (i < n) {
      char c = t_[i];
      if (c == '\\' && i + 1 < n && is_punct(t_[i + 1])) {
        buf_ += esc(std::string_view(&t_[i + 1], 1));
        i += 2;
      } else if (c == '\\' && i + 1 < n && t_[i + 1] == '\n') {
        html("<br />\n");
        i += 2;
      } else if (c == '`') {
        i = code_span(i);
      } else if (c == '*' || c == '_' || c == '~') {
        i = delimiter(i);
      } else if (c == '!' && i + 1 < n && t_[i + 1] == '[') {
        open(i + 2, true);
        i += 2;
      } else if (c == '[') {
        open(i + 1, false);
        i += 1;
      } else if (c == ']') {
        i = close_bracket(i);
      } else if (c == '<') {
        i = angle(i);
      } else if (c == '&') {
        i = entity(i);
      } else if (c == '\n') {
        size_t sp = 0;
        while (sp < buf_.size() && buf_[buf_.size() - 1 - sp] == ' ') ++sp;
        buf_.resize(buf_.size() - sp);
        if (sp >= 2) {
          html("<br />\n");
        } else {
          buf_ += '\n';
        }
        ++i;
        while (i < n && t_[i] == ' ') ++i;
      } else if ((c == 'h' || c == 'H') && (i == 0 || !is_alnum(t_[i - 1])) && !in_link() && bare_url(i, &i)) {
      } else {
        buf_ += esc(std::string_view(&t_[i], 1));
        ++i;
      }
    }
    flush();
    emphasis(nodes_.begin());
    std::string out;
    for (auto& node : nodes_) out += node.s;
    return out;
  }

 private:
  void flush() {
    if (buf_.empty()) return;
    Inl t;
    t.s = std::move(buf_);
    buf_.clear();
    nodes_.push_back(std::move(t));
  }
  void html(std::string s) {
    flush();
    Inl h;
    h.k = Inl::kHtml;
    h.s = std::move(s);
    nodes_.push_back(std::move(h));
  }
  bool in_link() const {
    for (const auto& node : nodes_) {
      if (node.k == Inl::kOpen && node.active && !node.image) return true;
    }
    return false;
  }

  size_t code_span(size_t i) {
    size_t k = 0;
    while (i + k < t_.size() && t_[i + k] == '`') ++k;
    size_t j = i + k;
    while (j < t_.size()) {
      if (t_[j] != '`') {
        ++j;
        continue;
      }
      size_t r = 0;
      while (j + r < t_.size() && t_[j + r] == '`') ++r;
      if (r == k) {
        std::string code(t_.substr(i + k, j - i - k));
        for (auto& ch : code) {
          if (ch == '\n') ch = ' ';
        }
        if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' &&
            code.find_first_not_of(' ') != std::string::npos) {
          code = code.substr(1, code.size() - 2);
        }
        html("<code>" + esc(code) + "</code>");
        return j + r;
      }
      j += r;
    }
    buf_ += std::string(k, '`');
    return i + k;
  }

  size_t delimiter(size_t i) {
    char c = t_[i];
    size_t k = 0;
    while (i + k < t_.size() && t_[i + k] == c) ++k;
    char before = i == 0 ? '\n' : t_[i - 1];
    char after = i + k >= t_.size() ? '\n' : t_[i + k];
    bool left = !is_space(after) && (!is_punct(after) || is_space(before) || is_punct(before));
    bool right = !is_space(before) && (!is_punct(before) || is_space(after) || is_punct(after));
    flush();
    Inl d;
    d.k = Inl::kDelim;
    d.ch = c;
    d.count = d.orig = int(k);
    d.s = std::string(k, c);
    if (c == '_') {
      d.can_open = left && (!right || is_punct(before));
      d.can_close = right && (!left || is_punct(after));
    } else if (c == '~') {
      d.can_open = k <= 2 && left;
      d.can_close = k <= 2 && right;
    } else {
      d.can_open = left;
      d.can_close = right;
    }
    nodes_.push_back(std::move(d));
    return i + k;
  }

  void open(size_t src, bool image) {
    flush();
    Inl o;
    o.k = Inl::kOpen;
    o.image = image;
    o.s = image ? "![" : "[";
    o.src = src;
    nodes_.push_back(std::move(o));
  }

  // Parses "(dest "title")" at i; returns the index after ")" or npos.
  size_t inline_link(size_t i, std::string* url, std::string* title) {
    size_t n = t_.size();
    if (i >= n || t_[i] != '(' || ctx_.work > kWorkBudget) return std::string::npos;
    size_t j = i + 1;
    while (j < n && is_space(t_[j])) ++j;
    if (j < n && t_[j] == '<') {
      size_t e = t_.find('>', j);
      if (e == std::string_view::npos || t_.substr(j, e - j).find('\n') != std::string_view::npos) return std::string::npos;
      *url = std::string(t_.substr(j + 1, e - j - 1));
      j = e + 1;
    } else {
      int depth = 0;
      size_t s = j;
      while (j < n && !is_space(t_[j])) {
        if (ctx_.spent()) return std::string::npos;
        if (t_[j] == '\\' && j + 1 < n) {
          j += 2;
          continue;
        }
        if (t_[j] == '(') ++depth;
        if (t_[j] == ')') {
          if (depth == 0) break;
          --depth;
        }
        ++j;
      }
      *url = unescape(t_.substr(s, j - s));
    }
    while (j < n && is_space(t_[j])) ++j;
    if (j < n && (t_[j] == '"' || t_[j] == '\'' || t_[j] == '(')) {
      char closer = t_[j] == '(' ? ')' : t_[j];
      size_t e = j + 1;
      while (e < n && t_[e] != closer) {
        if (ctx_.spent()) return std::string::npos;
        e += (t_[e] == '\\' && e + 1 < n) ? 2 : 1;
      }
      if (e >= n) return std::string::npos;
      *title = unescape(t_.substr(j + 1, e - j - 1));
      j = e + 1;
      while (j < n && is_space(t_[j])) ++j;
    }
    if (j >= n || t_[j] != ')') return std::string::npos;
    return j + 1;
  }

  static std::string unescape(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size() && is_punct(s[i + 1])) ++i;
      out += s[i];
    }
    return out;
  }

  size_t close_bracket(size_t i) {
    flush();
    auto opener = nodes_.end();
    for (auto it = nodes_.end(); it != nodes_.begin();) {
      --it;
      if (ctx_.spent()) break;
      if (it->k == Inl::kOpen) {
        opener = it;
        break;
      }
    }
    if (opener == nodes_.end()) {
      buf_ += "]";
      return i + 1;
    }
    if (!opener->active) {
      opener->k = Inl::kText;
      buf_ += "]";
      return i + 1;
    }
    std::string url, title;
    size_t next = inline_link(i + 1, &url, &title);
    bool found = next != std::string::npos;
    if (!found) {
      std::string label(t_.substr(opener->src, i - opener->src));
      next = i + 1;
      if (i + 1 < t_.size() && t_[i + 1] == '[') {
        size_t e = t_.find(']', i + 2);
        if (e != std::string_view::npos) {
          std::string inner(t_.substr(i + 2, e - i - 2));
          if (!inner.empty()) label = inner;
          next = e + 1;
        }
      }
      auto ref = ctx_.refs.find(normalize_label(label));
      if (ref != ctx_.refs.end()) {
        found = true;
        url = ref->second.url;
        title = ref->second.title;
      } else {
        next = i + 1;
      }
    }
    if (!found) {
      opener->k = Inl::kText;
      buf_ += "]";
      return i + 1;
    }
    emphasis(std::next(opener));
    std::string title_attr = title.empty() ? "" : " title=\"" + esc(title) + "\"";
    if (opener->image) {
      std::string alt;
      for (auto it = std::next(opener); it != nodes_.end(); ++it) alt += it->s;
      nodes_.erase(std::next(opener), nodes_.end());
      opener->k = Inl::kHtml;
      opener->s = "<img src=\"" + safe_url(url, true) + "\" alt=\"" + esc(html_to_text(alt)) + "\"" + title_attr + " />";
    } else {
      opener->k = Inl::kHtml;
      opener->s = "<a href=\"" + safe_url(url, false) + "\"" + title_attr + ">";
      html("</a>");
      for (auto it = nodes_.begin(); it != opener; ++it) {
        if (it->k == Inl::kOpen && !it->image) it->active = false;
      }
    }
    return next;
  }

  size_t angle(size_t i) {
    size_t e = t_.find('>', i + 1);
    if (e != std::string_view::npos) {
      std::string_view inner = t_.substr(i + 1, e - i - 1);
      bool spaces = inner.find_first_of(" \n<") != std::string_view::npos;
      size_t colon = inner.find(':');
      if (!spaces && colon != std::string_view::npos && colon >= 2 && colon <= 32 &&
          std::isalpha(static_cast<unsigned char>(inner[0]))) {
        html("<a href=\"" + safe_url(inner, false) + "\">" + esc(inner) + "</a>");
        return e + 1;
      }
      size_t at = inner.find('@');
      if (!spaces && at != std::string_view::npos && at > 0 && inner.find('.', at) != std::string_view::npos) {
        html("<a href=\"mailto:" + esc(inner) + "\">" + esc(inner) + "</a>");
        return e + 1;
      }
      if (ctx_.o.allow_html && i + 1 < t_.size() &&
          (std::isalpha(static_cast<unsigned char>(t_[i + 1])) || t_[i + 1] == '/' || t_[i + 1] == '!')) {
        html(std::string(t_.substr(i, e - i + 1)));
        return e + 1;
      }
    }
    buf_ += "&lt;";
    return i + 1;
  }

  size_t entity(size_t i) {
    size_t j = i + 1;
    bool num = j < t_.size() && t_[j] == '#';
    if (num) ++j;
    bool hex = num && j < t_.size() && (t_[j] == 'x' || t_[j] == 'X');
    if (hex) ++j;
    size_t s = j;
    while (j < t_.size() && j - s < 32 && (hex ? std::isxdigit(static_cast<unsigned char>(t_[j])) != 0
                                            : num ? std::isdigit(static_cast<unsigned char>(t_[j])) != 0
                                                  : is_alnum(t_[j]))) {
      ++j;
    }
    if (j > s && j < t_.size() && t_[j] == ';' && (num || std::isalpha(static_cast<unsigned char>(t_[s])))) {
      buf_ += std::string(t_.substr(i, j - i + 1));
      return j + 1;
    }
    buf_ += "&amp;";
    return i + 1;
  }

  bool bare_url(size_t i, size_t* next) {
    std::string head = base::to_lower(t_.substr(i, 8));
    if (!head.starts_with("http://") && !head.starts_with("https://")) return false;
    size_t j = i;
    while (j < t_.size() && !is_space(t_[j]) && t_[j] != '<') ++j;
    while (j > i && std::string_view(".,:;!?\"'*_~").find(t_[j - 1]) != std::string_view::npos) --j;
    std::string_view url = t_.substr(i, j - i);
    while (!url.empty() && url.back() == ')' &&
           std::count(url.begin(), url.end(), ')') > std::count(url.begin(), url.end(), '(')) {
      url.remove_suffix(1);
    }
    if (url.size() <= head.find("//") + 2) return false;
    html("<a href=\"" + safe_url(url, false) + "\">" + esc(url) + "</a>");
    *next = i + url.size();
    return true;
  }

  // CommonMark's "process emphasis", from `bottom` to the end.
  void emphasis(InlList::iterator bottom) {
    for (auto closer = bottom; closer != nodes_.end();) {
      if (closer->k != Inl::kDelim || !closer->can_close || closer->count == 0) {
        ++closer;
        continue;
      }
      auto opener = nodes_.end();
      for (auto it = closer; it != bottom;) {
        --it;
        if (ctx_.spent()) return;  // what's left stays literal text
        if (it->k != Inl::kDelim || it->ch != closer->ch || !it->can_open || it->count == 0) continue;
        if (closer->ch == '~' && it->count != closer->count) continue;
        bool both = it->can_close || closer->can_open;
        if (both && (it->orig + closer->orig) % 3 == 0 && !(it->orig % 3 == 0 && closer->orig % 3 == 0)) continue;
        opener = it;
        break;
      }
      if (opener == nodes_.end()) {
        if (!closer->can_open) closer->k = Inl::kText;
        ++closer;
        continue;
      }
      int use = closer->ch == '~' ? closer->count : (closer->count >= 2 && opener->count >= 2) ? 2 : 1;
      std::string tag = closer->ch == '~' ? "del" : use == 2 ? "strong" : "em";
      opener->count -= use;
      closer->count -= use;
      opener->s = std::string(size_t(opener->count), opener->ch);
      closer->s = std::string(size_t(closer->count), closer->ch);
      for (auto it = std::next(opener); it != closer; ++it) {
        if (it->k == Inl::kDelim) it->k = Inl::kText;
      }
      Inl o, c;
      o.k = c.k = Inl::kHtml;
      o.s = "<" + tag + ">";
      c.s = "</" + tag + ">";
      nodes_.insert(std::next(opener), std::move(o));
      nodes_.insert(closer, std::move(c));
      if (closer->count == 0) ++closer;
    }
  }

  Ctx& ctx_;
  std::string_view t_;
  InlList nodes_;
  std::string buf_;
};

std::string inlines(Ctx& ctx, std::string_view text) {
  if (ctx.work > kWorkBudget) return esc(trim(text));
  return Inline(ctx, trim(text)).render();
}

std::string slug(std::string_view text) {
  std::string out;
  bool dash = false;
  for (char c : text) {
    if (is_alnum(c) || (static_cast<unsigned char>(c) >= 0x80)) {
      if (dash && !out.empty()) out += '-';
      dash = false;
      out += char(std::tolower(static_cast<unsigned char>(c)));
    } else if (c == ' ' || c == '-' || c == '_') {
      dash = true;
    }
  }
  return out.empty() ? "section" : out;
}

// --- Blocks ---

bool starts_block(Ctx& ctx, std::string_view l) {
  int level;
  std::string text;
  Fence f;
  Marker m;
  if (atx_heading(l, &level, &text) || thematic_break(l) || fence_open(l, &f) || quote_line(l)) return true;
  if (ctx.o.allow_html && html_block_start(l)) return true;
  return list_marker(l, &m) && !m.empty && (!m.ordered || m.start == 1);
}

std::string blocks(Ctx& ctx, const Lines& lines, bool tight, bool* starts_with_paragraph = nullptr);

std::string heading(Ctx& ctx, int level, std::string_view text) {
  std::string inner = inlines(ctx, text);
  std::string tag = "h" + std::to_string(level);
  std::string id;
  if (ctx.o.heading_ids) {
    std::string base_id = slug(html_to_text(inner));
    int& seen = ctx.ids[base_id];
    id = " id=\"" + esc(seen == 0 ? base_id : base_id + "-" + std::to_string(seen)) + "\"";
    ++seen;
  }
  return "<" + tag + id + ">" + inner + "</" + tag + ">\n";
}

std::string table(Ctx& ctx, const Lines& lines, size_t& i, const std::vector<std::string>& aligns) {
  auto header = table_cells(lines[i]);
  std::string out = "<table>\n<thead>\n<tr>\n";
  auto cell = [&](const std::string& tag, size_t col, const std::string& text) {
    std::string align = col < aligns.size() && !aligns[col].empty() ? " align=\"" + aligns[col] + "\"" : "";
    return "<" + tag + align + ">" + inlines(ctx, text) + "</" + tag + ">\n";
  };
  for (size_t c = 0; c < aligns.size(); ++c) out += cell("th", c, c < header.size() ? header[c] : "");
  out += "</tr>\n</thead>\n";
  i += 2;
  bool body = false;
  while (i < lines.size() && !blank(lines[i]) && lines[i].find('|') != std::string::npos && !starts_block(ctx, lines[i])) {
    if (!body) out += "<tbody>\n";
    body = true;
    auto row = table_cells(lines[i]);
    out += "<tr>\n";
    for (size_t c = 0; c < aligns.size(); ++c) out += cell("td", c, c < row.size() ? row[c] : "");
    out += "</tr>\n";
    ++i;
  }
  if (body) out += "</tbody>\n";
  return out + "</table>\n";
}

std::string list(Ctx& ctx, const Lines& lines, size_t& i) {
  Marker first;
  list_marker(lines[i], &first);
  std::vector<Lines> items;
  bool loose = false;
  while (i < lines.size()) {
    Marker m;
    if (thematic_break(lines[i]) || !list_marker(lines[i], &m) || m.ordered != first.ordered || m.ch != first.ch) break;
    Lines item;
    const std::string& l = lines[i];
    item.push_back(m.offset < l.size() ? l.substr(m.offset) : "");
    size_t width = m.offset;
    ++i;
    bool prev_blank = m.empty;
    bool inner_blank = false;
    while (i < lines.size()) {
      const std::string& next = lines[i];
      if (blank(next)) {
        // An empty item ends at its first blank line.
        if (item.size() == 1 && item[0].empty()) break;
        item.emplace_back();
        prev_blank = true;
        ++i;
        continue;
      }
      if (indent_of(next) >= width) {
        if (prev_blank) inner_blank = true;
        item.push_back(next.substr(width));
        prev_blank = false;
        ++i;
        continue;
      }
      if (Marker other; list_marker(next, &other)) break;
      if (!prev_blank && !starts_block(ctx, next) && !blank(item.back())) {
        item.push_back(std::string(trim(next)));
        ++i;
        continue;
      }
      break;
    }
    size_t trailing = 0;
    while (!item.empty() && blank(item.back())) {
      item.pop_back();
      ++trailing;
    }
    if (inner_blank) {
      // A blank line between two blocks of one item loosens the list; one inside a
      // fenced code block doesn't, but telling them apart isn't worth it here.
      loose = true;
    }
    items.push_back(std::move(item));
    Marker again;
    if (trailing > 0 && i < lines.size() && list_marker(lines[i], &again) && again.ordered == first.ordered &&
        again.ch == first.ch) {
      loose = true;
    }
  }
  std::string tag = first.ordered ? "ol" : "ul";
  std::string out = "<" + tag + (first.ordered && first.start != 1 ? " start=\"" + std::to_string(first.start) + "\"" : "") + ">\n";
  for (auto& item : items) {
    std::string check;
    if (!item.empty()) {
      std::string& head = item[0];
      if (head.starts_with("[ ] ") || head == "[ ]") {
        check = "<input type=\"checkbox\" disabled /> ";
        head = head.substr(std::min<size_t>(4, head.size()));
      } else if (head.starts_with("[x] ") || head.starts_with("[X] ") || head == "[x]" || head == "[X]") {
        check = "<input type=\"checkbox\" checked disabled /> ";
        head = head.substr(std::min<size_t>(4, head.size()));
      }
    }
    bool para = false;
    std::string inner = blocks(ctx, item, !loose, &para);
    if (!loose) {
      while (!inner.empty() && inner.back() == '\n') inner.pop_back();
      // A tight item ends on a newline only when its last block isn't a paragraph.
      bool ends_in_block = false;
      for (const char* close : {"</ul>", "</ol>", "</pre>", "</blockquote>", "</table>", "<hr />", "</h1>", "</h2>",
                                "</h3>", "</h4>", "</h5>", "</h6>"}) {
        ends_in_block = ends_in_block || inner.ends_with(close);
      }
      out += "<li>" + check + (para || inner.empty() ? "" : "\n") + inner + (ends_in_block ? "\n" : "") + "</li>\n";
    } else {
      out += "<li>" + check + "\n" + inner + "</li>\n";
    }
  }
  return out + "</" + tag + ">\n";
}

std::string blocks(Ctx& ctx, const Lines& lines, bool tight, bool* starts_with_paragraph) {
  // Past kMaxNesting, the rest is one paragraph of plain text: no more recursion.
  if (ctx.depth >= kMaxNesting) {
    std::string text;
    for (const auto& l : lines) text += (text.empty() ? "" : "\n") + l;
    return "<p>" + esc(text) + "</p>\n";
  }
  struct Deeper {
    Ctx& c;
    explicit Deeper(Ctx& ctx) : c(ctx) { ++c.depth; }
    ~Deeper() { --c.depth; }
  } deeper(ctx);
  std::string out;
  if (starts_with_paragraph) *starts_with_paragraph = false;
  size_t i = 0;
  while (i < lines.size()) {
    const std::string& l = lines[i];
    if (blank(l)) {
      ++i;
      continue;
    }
    int level = 0;
    std::string text;
    Fence fence;
    std::vector<std::string> aligns;
    if (indent_of(l) >= 4) {
      Lines code;
      while (i < lines.size() && (blank(lines[i]) || indent_of(lines[i]) >= 4)) {
        code.push_back(lines[i].size() > 4 ? lines[i].substr(4) : "");
        ++i;
      }
      while (!code.empty() && blank(code.back())) code.pop_back();
      out += "<pre><code>";
      for (auto& c : code) out += esc(c) + "\n";
      out += "</code></pre>\n";
    } else if (fence_open(l, &fence)) {
      ++i;
      std::string code;
      while (i < lines.size() && !fence_close(lines[i], fence)) {
        const std::string& c = lines[i];
        size_t drop = std::min(fence.indent, indent_of(c));
        code += esc(c.substr(drop)) + "\n";
        ++i;
      }
      if (i < lines.size()) ++i;
      std::string cls = fence.info.empty() ? "" : " class=\"language-" + esc(fence.info) + "\"";
      out += "<pre><code" + cls + ">" + code + "</code></pre>\n";
    } else if (atx_heading(l, &level, &text)) {
      out += heading(ctx, level, text);
      ++i;
    } else if (thematic_break(l)) {
      out += "<hr />\n";
      ++i;
    } else if (quote_line(l)) {
      Lines inner;
      while (i < lines.size()) {
        const std::string& q = lines[i];
        if (quote_line(q)) {
          size_t at = q.find('>') + 1;
          if (at < q.size() && q[at] == ' ') ++at;
          inner.push_back(q.substr(at));
        } else if (!blank(q) && !inner.empty() && !blank(inner.back()) && !starts_block(ctx, q)) {
          inner.push_back(q);
        } else {
          break;
        }
        ++i;
      }
      out += "<blockquote>\n" + blocks(ctx, inner, false) + "</blockquote>\n";
    } else if (Marker m; list_marker(l, &m)) {
      out += list(ctx, lines, i);
    } else if (ctx.o.allow_html && html_block_start(l)) {
      while (i < lines.size() && !blank(lines[i])) out += lines[i++] + "\n";
    } else if (l.find('|') != std::string::npos && i + 1 < lines.size() && table_delimiter(lines[i + 1], &aligns) &&
               table_cells(l).size() == aligns.size()) {
      out += table(ctx, lines, i, aligns);
    } else {
      std::string para;
      int setext = 0;
      while (i < lines.size() && !blank(lines[i])) {
        const std::string& p = lines[i];
        if (!para.empty() && indent_of(p) <= 3) {
          std::string_view t = trim(p);
          if (!t.empty() && t.find_first_not_of('=') == std::string_view::npos) setext = 1;
          if (!t.empty() && t.find_first_not_of('-') == std::string_view::npos) setext = 2;
          if (setext) {
            ++i;
            break;
          }
        }
        if (!para.empty() && starts_block(ctx, p)) break;
        if (!para.empty()) para += '\n';
        para += p.substr(indent_of(p));
        ++i;
      }
      if (out.empty() && starts_with_paragraph && !setext) *starts_with_paragraph = true;
      if (setext) {
        out += heading(ctx, setext, para);
      } else if (tight) {
        out += inlines(ctx, para) + "\n";
      } else {
        out += "<p>" + inlines(ctx, para) + "</p>\n";
      }
    }
  }
  return out;
}

// Pulls reference definitions ("[label]: url "title"") out of the lines, outside code.
Lines take_refs(Ctx& ctx, const Lines& lines) {
  Lines out;
  Fence f;
  bool in_fence = false;
  bool para = false;
  for (const auto& l : lines) {
    if (in_fence) {
      if (fence_close(l, f)) in_fence = false;
      out.push_back(l);
      continue;
    }
    if (fence_open(l, &f)) {
      in_fence = true;
      out.push_back(l);
      continue;
    }
    std::string_view t = trim(l);
    if (!para && indent_of(l) <= 3 && t.starts_with("[") && !t.starts_with("[^")) {
      size_t close = t.find("]:");
      if (close != std::string_view::npos && close > 1) {
        std::string label = normalize_label(t.substr(1, close - 1));
        std::string_view rest = trim(t.substr(close + 2));
        std::string_view url = rest.substr(0, rest.find(' '));
        std::string_view title = trim(rest.substr(url.size()));
        if (url.size() >= 2 && url.front() == '<' && url.back() == '>') url = url.substr(1, url.size() - 2);
        if (!url.empty() && (title.empty() || (title.size() >= 2 && (title.front() == '"' || title.front() == '\'' ||
                                                                       title.front() == '(')))) {
          if (!title.empty()) title = title.substr(1, title.size() - 2);
          ctx.refs.emplace(label, Ref{std::string(url), std::string(title)});
          continue;
        }
      }
    }
    para = !blank(l) && !starts_block(ctx, l) && indent_of(l) < 4;
    out.push_back(l);
  }
  return out;
}

}  // namespace

std::string markdown_to_html(std::string_view markdown, const MarkdownOptions& options) {
  Ctx ctx{options, {}, {}};
  Lines lines = take_refs(ctx, split_lines(markdown));
  return blocks(ctx, lines, false);
}

std::string html_to_text(std::string_view html) {
  std::string text;
  bool tag = false;
  for (char c : html) {
    if (c == '<') {
      tag = true;
    } else if (c == '>' && tag) {
      tag = false;
      text += ' ';
    } else if (!tag) {
      text += c;
    }
  }
  std::string decoded = decode_entities(text);
  std::string out;
  bool space = false;
  for (char c : decoded) {
    if (is_space(c)) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

std::string markdown_title(std::string_view markdown) {
  MarkdownOptions o;
  o.heading_ids = false;
  Ctx ctx{o, {}, {}};
  Lines lines = take_refs(ctx, split_lines(markdown));
  Fence f;
  for (size_t i = 0; i < lines.size(); ++i) {
    int level;
    std::string text;
    if (fence_open(lines[i], &f)) {
      while (++i < lines.size() && !fence_close(lines[i], f)) {
      }
      continue;
    }
    if (atx_heading(lines[i], &level, &text) && level == 1) return html_to_text(inlines(ctx, text));
    if (i + 1 < lines.size() && !blank(lines[i]) && !starts_block(ctx, lines[i])) {
      std::string_view u = trim(lines[i + 1]);
      if (!u.empty() && u.find_first_not_of('=') == std::string_view::npos) return html_to_text(inlines(ctx, lines[i]));
    }
  }
  return "";
}

std::string markdown_summary(std::string_view markdown, size_t max_chars) {
  MarkdownOptions o;
  o.heading_ids = false;
  Ctx ctx{o, {}, {}};
  Lines lines = take_refs(ctx, split_lines(markdown));
  Fence f;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    if (blank(l)) continue;
    if (fence_open(l, &f)) {
      while (++i < lines.size() && !fence_close(lines[i], f)) {
      }
      continue;
    }
    Marker m;
    int level;
    std::string text;
    if (indent_of(l) >= 4 || atx_heading(l, &level, &text) || thematic_break(l) || list_marker(l, &m) ||
        html_block_start(l) || l.find('|') != std::string::npos) {
      continue;
    }
    std::string para;
    while (i < lines.size() && !blank(lines[i])) {
      std::string_view u = trim(lines[i]);
      if (!para.empty() && !u.empty() && (u.find_first_not_of('=') == std::string_view::npos ||
                                          u.find_first_not_of('-') == std::string_view::npos)) {
        para.clear();  // a setext heading, not a paragraph
        break;
      }
      std::string_view line = lines[i];
      if (quote_line(line)) line = trim(line.substr(line.find('>') + 1));
      para += std::string(line) + "\n";
      ++i;
    }
    if (para.empty()) continue;
    std::string s = html_to_text(inlines(ctx, para));
    if (s.size() <= max_chars) return s;
    size_t cut = s.rfind(' ', max_chars);
    if (cut == std::string::npos || cut < max_chars / 2) cut = max_chars;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return std::string(trim(s.substr(0, cut))) + "\xE2\x80\xA6";
  }
  return "";
}

}  // namespace goober::render
