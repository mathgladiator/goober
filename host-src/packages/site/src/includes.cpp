#include "goober/site/includes.h"

#include "goober/site/pwa.h"

#include "goober/render/mustache.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace goober::site {

namespace {

constexpr std::string_view kMarker = "<!--#include";

bool name_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

struct Marker {
  std::string name;
  size_t end = 0;
  boost::json::object params;  // key="value" pairs after the name (027 § 7)
};

bool key_char(char c, bool first) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (!first && c >= '0' && c <= '9');
}

constexpr size_t kMaxParams = 32;
constexpr size_t kMaxValue = 2048;

// One marker at `at`: its name, its parameters, and where it ends, or nullopt when it isn't one.
std::optional<Marker> marker_at(std::string_view html, size_t at) {
  size_t i = at + kMarker.size();
  if (i >= html.size() || (html[i] != ' ' && html[i] != '\t')) return std::nullopt;
  while (i < html.size() && (html[i] == ' ' || html[i] == '\t')) ++i;
  size_t start = i;
  // A name may hold "-", so it stops where the comment closes.
  while (i < html.size() && name_char(html[i]) && html.substr(i, 3) != "-->") ++i;
  Marker m;
  m.name = std::string(html.substr(start, i - start));
  if (!is_partial_name(m.name)) return std::nullopt;
  for (;;) {
    size_t gap = i;
    while (i < html.size() && (html[i] == ' ' || html[i] == '\t' || html[i] == '\n')) ++i;
    if (html.substr(i, 3) == "-->") break;
    // A parameter: key="value", after at least one space. The value is taken as written, up to
    // the next quote; it may not hold "--", so the comment can't close inside it.
    if (i == gap || i >= html.size() || !key_char(html[i], true)) return std::nullopt;
    size_t ks = i;
    while (i < html.size() && key_char(html[i], false)) ++i;
    std::string key(html.substr(ks, i - ks));
    if (html.substr(i, 2) != "=\"") return std::nullopt;
    i += 2;
    size_t close = html.find('"', i);
    if (close == std::string_view::npos || close - i > kMaxValue) return std::nullopt;
    std::string_view value = html.substr(i, close - i);
    if (value.find("--") != std::string_view::npos || m.params.contains(key) || m.params.size() >= kMaxParams) return std::nullopt;
    m.params[key] = boost::json::string(value);
    i = close + 1;
  }
  m.end = i + 3;
  return m;
}

struct Attr {
  std::string name, value;
  size_t value_begin = 0, value_end = 0;  // in the tag; for an unquoted or empty value, begin == end at its spot
  char quote = 0;
};

// The end of the tag that starts at `at` (one past its '>'), quotes respected, or npos.
size_t tag_end(std::string_view s, size_t at) {
  char quote = 0;
  for (size_t i = at + 1; i < s.size(); ++i) {
    char c = s[i];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i + 1;
    }
  }
  return std::string_view::npos;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// The attributes of one tag's text ("<a href=... class=...>").
std::vector<Attr> attributes(std::string_view tag) {
  std::vector<Attr> out;
  size_t i = 1;
  while (i < tag.size() && !std::isspace(static_cast<unsigned char>(tag[i])) && tag[i] != '>' && tag[i] != '/') ++i;
  while (i < tag.size()) {
    while (i < tag.size() && (std::isspace(static_cast<unsigned char>(tag[i])) || tag[i] == '/')) ++i;
    if (i >= tag.size() || tag[i] == '>') break;
    size_t ns = i;
    while (i < tag.size() && !std::isspace(static_cast<unsigned char>(tag[i])) && tag[i] != '=' && tag[i] != '>' &&
           tag[i] != '/') {
      ++i;
    }
    Attr a;
    a.name = lower(tag.substr(ns, i - ns));
    a.value_begin = a.value_end = i;
    size_t j = i;
    while (j < tag.size() && std::isspace(static_cast<unsigned char>(tag[j]))) ++j;
    if (j < tag.size() && tag[j] == '=') {
      ++j;
      while (j < tag.size() && std::isspace(static_cast<unsigned char>(tag[j]))) ++j;
      if (j < tag.size() && (tag[j] == '"' || tag[j] == '\'')) {
        a.quote = tag[j];
        size_t close = tag.find(a.quote, j + 1);
        if (close == std::string_view::npos) close = tag.size() - 1;
        a.value_begin = j + 1;
        a.value_end = close;
        a.value = std::string(tag.substr(j + 1, close - j - 1));
        i = close + 1;
      } else {
        size_t vs = j;
        while (j < tag.size() && !std::isspace(static_cast<unsigned char>(tag[j])) && tag[j] != '>') ++j;
        a.value_begin = vs;
        a.value_end = j;
        a.value = std::string(tag.substr(vs, j - vs));
        i = j;
      }
    }
    if (!a.name.empty()) out.push_back(std::move(a));
  }
  return out;
}

const Attr* find(const std::vector<Attr>& attrs, std::string_view name) {
  for (const auto& a : attrs) {
    if (a.name == name) return &a;
  }
  return nullptr;
}

// A path as compared: no query or fragment, no trailing slash but on "/".
std::string normal(std::string_view p) {
  std::string s(p.substr(0, std::min(p.find('?'), p.find('#'))));
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  if (s.empty()) s = "/";
  return s;
}

// Whether `tag` (the "<a" or "<meta" at the start) begins with that tag name.
bool is_tag(std::string_view s, size_t at, std::string_view name) {
  if (s.size() < at + 1 + name.size() + 1) return false;
  if (lower(s.substr(at + 1, name.size())) != name) return false;
  char c = s[at + 1 + name.size()];
  return std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '/';
}

// The page's own choice of link: <meta name="nav-key" content="...">.
std::string nav_key_of(std::string_view html) {
  for (size_t at = html.find('<'); at != std::string_view::npos; at = html.find('<', at + 1)) {
    if (!is_tag(html, at, "meta")) continue;
    size_t end = tag_end(html, at);
    if (end == std::string_view::npos) break;
    auto attrs = attributes(html.substr(at, end - at));
    const Attr* name = find(attrs, "name");
    const Attr* content = find(attrs, "content");
    if (name && content && lower(name->value) == "nav-key") return content->value;
  }
  return "";
}

}  // namespace

bool is_partial_name(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  return std::all_of(name.begin(), name.end(), name_char);
}

std::vector<std::string> include_names(std::string_view html) {
  std::vector<std::string> out;
  for (size_t at = html.find(kMarker); at != std::string_view::npos; at = html.find(kMarker, at + 1)) {
    auto m = marker_at(html, at);
    if (m && std::find(out.begin(), out.end(), m->name) == out.end()) out.push_back(m->name);
  }
  return out;
}

std::string mark_active(std::string_view partial, std::string_view path, std::string_view nav_key) {
  struct Link {
    size_t at, end;
    std::vector<Attr> attrs;
    size_t score;
  };
  std::vector<Link> links;
  std::string p = normal(path);
  bool keyed = false;
  for (size_t at = partial.find('<'); at != std::string_view::npos; at = partial.find('<', at + 1)) {
    if (!is_tag(partial, at, "a")) continue;
    size_t end = tag_end(partial, at);
    if (end == std::string_view::npos) break;
    Link l{at, end, attributes(partial.substr(at, end - at)), 0};
    if (const Attr* k = find(l.attrs, "data-key"); k && !nav_key.empty() && k->value == nav_key) {
      l.score = std::string_view::npos;  // the page named it
      keyed = true;
    }
    if (const Attr* e = find(l.attrs, "data-match-exact"); e && normal(e->value) == p) {
      l.score = std::max(l.score, 2 * normal(e->value).size() + 1);
    }
    if (const Attr* m = find(l.attrs, "data-match")) {
      std::string v = normal(m->value);
      if (p == v || v == "/" || p.starts_with(v + "/")) l.score = std::max(l.score, 2 * v.size());
    }
    links.push_back(std::move(l));
  }
  size_t best = 0;
  for (const auto& l : links) best = std::max(best, l.score);
  if (best == 0) return std::string(partial);
  if (keyed) best = std::string_view::npos;
  std::string out(partial);
  // From the last link back, so earlier offsets stay true.
  for (auto it = links.rbegin(); it != links.rend(); ++it) {
    if (it->score != best) continue;
    std::string tag(partial.substr(it->at, it->end - it->at));
    std::string extra;
    if (!find(it->attrs, "aria-current")) extra += " aria-current=\"page\"";
    if (const Attr* c = find(it->attrs, "class")) {
      if (c->quote) {
        tag.insert(c->value_end, c->value.empty() ? "active" : " active");
      } else {
        tag.replace(c->value_begin, c->value_end - c->value_begin, "\"" + c->value + (c->value.empty() ? "" : " ") + "active\"");
      }
    } else {
      extra += " class=\"active\"";
    }
    size_t close = tag.size() - 1;  // the '>'
    if (close > 0 && tag[close - 1] == '/') --close;
    tag.insert(close, extra);
    out.replace(it->at, it->end - it->at, tag);
  }
  return out;
}

Expanded expand_includes(const content::ContentStore& store, std::string_view tenant, std::string_view html,
                         std::string_view path, bool signed_in) {
  Expanded out;
  if (html.find(kMarker) == std::string_view::npos) {
    out.html = std::string(html);
    return out;
  }
  std::string key = nav_key_of(html);
  size_t from = 0;
  for (size_t at = html.find(kMarker); at != std::string_view::npos; at = html.find(kMarker, at + 1)) {
    auto m = marker_at(html, at);
    if (!m) continue;
    out.html.append(html.substr(from, at - from));
    from = m->end;
    const std::string& name = m->name;
    std::string file = "_partials/" + name + ".html";
    auto auth = store.get(tenant, "_partials/" + name + "@auth.html");
    if (auth) out.by_auth = true;
    std::optional<content::File> f = signed_in && auth ? auth : store.get(tenant, file);
    if (signed_in && auth) file = "_partials/" + name + "@auth.html";
    if (!f && name == "pwa") {
      // The built-in pwa partial: the head tags of the app whose scope fits (782.PWA.md § 3),
      // or nothing, which is no miss. A tenant's own _partials/pwa.html wins.
      if (auto app = app_for(apps(store, tenant), path)) {
        out.html += head_tags(*app);
        out.used.push_back("_apps/" + app->name + ".json");
      }
      continue;
    }
    if (!f) {
      if (std::find(out.missing.begin(), out.missing.end(), name) == out.missing.end()) out.missing.push_back(name);
      continue;
    }
    if (std::find(out.used.begin(), out.used.end(), file) == out.used.end()) out.used.push_back(file);
    std::string text = f->data;
    if (!m->params.empty()) {
      // With parameters, the partial is a Mustache template over them: {{title}} is escaped,
      // {{#featured}}…{{/featured}} a section. Without, it's spliced as written, as before.
      render::TemplateOptions opts;
      opts.max_output_bytes = 1 << 20;
      std::string err;
      auto rendered = render::render_mustache(text, boost::json::value(m->params), opts, &err);
      if (rendered) {
        text = std::move(*rendered);
      } else {
        out.errors.push_back(name + ": " + err);
      }
    }
    out.html += mark_active(text, path, key);
  }
  out.html.append(html.substr(from));
  return out;
}

}  // namespace goober::site
