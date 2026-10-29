#include "goober/site/site.h"

#include <algorithm>
#include <charconv>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"
#include "goober/render/frontmatter.h"
#include "goober/render/markdown.h"
#include "goober/render/mustache.h"

namespace goober::site {

const std::map<std::string, std::string>& builtin_templates();

namespace {

constexpr int kMaxLayouts = 4;
constexpr int64_t kDefaultPerPage = 20;
constexpr int64_t kMaxPerPage = 200;
constexpr size_t kFeedEntries = 20;

// The CSP for templated pages (007 §10): scripts and styles from the site itself,
// images from anywhere on https, forms only to the site, framed only by the site.
constexpr const char* kTemplatedCsp =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' https: data:; "
    "media-src 'self' https:; font-src 'self' https: data:; connect-src 'self'; form-action 'self'; "
    "frame-ancestors 'self'; base-uri 'self'; object-src 'none'";

}  // namespace

const char* templated_csp() { return kTemplatedCsp; }

namespace {

// Keys the host sets in template data, which front matter can't shadow.
bool reserved_key(std::string_view k) {
  return k == "body" || k == "page" || k == "site" || k == "data" || k == "user" || k == "pages" ||
         k == "pagination" || k == "request" || k == "now" || k == "tag" || k == "title_in_body" || k == "document";
}

std::string etag_of(std::string_view body) { return "\"" + base::sha256_hex(body).substr(0, 16) + "\""; }

std::string str(const bj::object& o, std::string_view key) {
  auto* v = o.if_contains(key);
  if (!v) return "";
  if (v->is_string()) return std::string(v->get_string());
  if (v->is_int64()) return std::to_string(v->get_int64());
  return "";
}

bool flag(const bj::object& o, std::string_view key) {
  auto* v = o.if_contains(key);
  return v && ((v->is_bool() && v->get_bool()) || (v->is_string() && v->get_string() == "true"));
}

// "trip-to-maine" becomes "Trip to maine".
std::string humanize(std::string_view name) {
  std::string out;
  for (char c : name) out += (c == '-' || c == '_') ? ' ' : c;
  if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = char(out[0] - 'a' + 'A');
  return out;
}

std::string last_segment(std::string_view path) {
  auto slash = path.rfind('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string strip_ext(std::string_view path) {
  auto dot = path.rfind('.');
  auto slash = path.rfind('/');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return std::string(path);
  return std::string(path.substr(0, dot));
}

std::string between(std::string_view html, std::string_view open, std::string_view close) {
  std::string lower = base::to_lower(html.substr(0, std::min<size_t>(html.size(), 64 * 1024)));
  auto a = lower.find(open);
  if (a == std::string::npos) return "";
  auto start = lower.find('>', a);
  if (start == std::string::npos) return "";
  auto b = lower.find(close, start);
  if (b == std::string::npos) return "";
  return render::html_to_text(html.substr(start + 1, b - start - 1));
}

std::string title_of(const Page& p) {
  if (auto t = str(p.fields, "title"); !t.empty()) return t;
  if (p.kind == Kind::kMarkdown) {
    if (auto t = render::markdown_title(p.body); !t.empty()) return t;
  } else {
    if (auto t = between(p.body, "<title", "</title>"); !t.empty()) return t;
    if (auto t = between(p.body, "<h1", "</h1>"); !t.empty()) return t;
  }
  std::string name = last_segment(strip_ext(p.path));
  if (name == "index") {
    std::string dir = p.path.find('/') == std::string::npos ? "" : last_segment(p.path.substr(0, p.path.rfind('/')));
    return dir.empty() ? "Home" : humanize(dir);
  }
  return humanize(name);
}

std::string summary_of(const Page& p) {
  if (auto s = str(p.fields, "summary"); !s.empty()) return s;
  if (auto s = str(p.fields, "description"); !s.empty()) return s;
  if (p.kind == Kind::kMarkdown) return render::markdown_summary(p.body);
  std::string text = render::html_to_text(p.kind == Kind::kDocument ? between(p.body, "<body", "</body>") : p.body);
  if (text.size() <= 240) return text;
  size_t cut = text.rfind(' ', 240);
  if (cut == std::string::npos) cut = 240;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  return text.substr(0, cut) + "\xE2\x80\xA6";
}

std::optional<int64_t> to_int(std::string_view s) {
  int64_t n = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc() || end != s.data() + s.size()) return std::nullopt;
  return n;
}

// An RFC 3339 time for Atom, from "YYYY-MM-DD" or a full time.
std::string atom_time(const std::string& date, const std::string& fallback) {
  if (date.size() == 10) return date + "T00:00:00Z";
  if (date.size() > 10 && date[4] == '-' && date[10] == 'T') return date;
  return fallback;
}

std::string xml_escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string query_string(const std::map<std::string, std::string>& q) {
  std::string out;
  for (const auto& [k, v] : q) {
    out += (out.empty() ? "?" : "&") + base::percent_encode(k) + "=" + base::percent_encode(v);
  }
  return out;
}

Result fail(int status, std::string error = "") {
  Result r;
  r.status = status;
  r.error = std::move(error);
  return r;
}

}  // namespace

const char* kind_name(Kind kind) {
  switch (kind) {
    case Kind::kStatic: return "static";
    case Kind::kMarkdown: return "markdown";
    case Kind::kFragment: return "fragment";
    case Kind::kDocument: return "document";
    case Kind::kListing: return "listing";
    case Kind::kDirectory: return "directory";
    case Kind::kFeed: return "feed";
    case Kind::kRedirect: return "redirect";
    case Kind::kSource: return "source";
  }
  return "unknown";
}

std::optional<std::string> page_url(std::string_view path) {
  if (!path.ends_with(".md") && !path.ends_with(".html")) return std::nullopt;
  std::string bare = strip_ext(path);
  if (bare == "index") return std::string("/");
  if (bare.ends_with("/index")) bare.resize(bare.size() - 6);
  return "/" + bare;
}

bool is_private_path(std::string_view path) {
  // Static site generators put their assets in these; nobody hides anything in them.
  static constexpr std::string_view kGeneratorDirs[] = {"_astro/", "_next/", "_nuxt/", "_app/"};
  size_t start = 0;
  while (start <= path.size()) {
    bool generator = false;
    for (auto dir : kGeneratorDirs) generator |= path.substr(start).starts_with(dir);
    if (start < path.size() && path[start] == '_' && !generator) return true;
    size_t slash = path.find('/', start);
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return false;
}

Kind kind_of(std::string_view path, std::string_view data) {
  if (path.ends_with(".md")) return Kind::kMarkdown;
  if (path.ends_with(".html")) {
    if (data.starts_with("\xEF\xBB\xBF")) data.remove_prefix(3);
    // The same rule split_front_matter uses: the first line, trimmed, is "---".
    auto end = data.find('\n');
    std::string_view first = data.substr(0, end);
    while (!first.empty() && (first.back() == ' ' || first.back() == '\t' || first.back() == '\r')) first.remove_suffix(1);
    while (!first.empty() && (first.front() == ' ' || first.front() == '\t')) first.remove_prefix(1);
    return first == "---" ? Kind::kFragment : Kind::kDocument;
  }
  return Kind::kStatic;
}

std::vector<std::string> Site::builtin_names() {
  std::vector<std::string> out;
  for (const auto& [name, text] : builtin_templates()) out.push_back(name);
  return out;
}

std::optional<Page> Site::load(std::string_view tenant, std::string_view path) const {
  auto url = page_url(path);
  if (!url) return std::nullopt;
  auto file = store_.get(tenant, path);
  if (!file) return std::nullopt;
  Page p;
  p.path = std::string(path);
  p.url = *url;
  p.kind = kind_of(path, file->data);
  p.raw = std::move(file->data);
  if (p.kind == Kind::kDocument) {
    p.body = p.raw;
  } else {
    std::string err;
    auto doc = render::split_front_matter(p.raw, &err);
    p.fields = std::move(doc.fields);
    p.body = std::move(doc.body);
    p.broken = err;
    if (auto* d = p.fields.if_contains("draft"); d && !d->is_bool()) {
      p.broken = "draft must be true or false";
    }
    // A whole HTML document with front matter (document: true): served as written, without
    // the front matter, but its draft, requirements, title, and summary still apply.
    if (auto* d = p.fields.if_contains("document"); d && d->is_bool() && d->as_bool()) p.kind = Kind::kDocument;
  }
  return p;
}

std::vector<Page> Site::pages(std::string_view tenant, std::string_view prefix) const {
  std::vector<Page> out;
  for (const auto& f : store_.list(tenant, prefix)) {
    if (is_private_path(f) || !page_url(f)) continue;
    if (auto p = load(tenant, f)) out.push_back(std::move(*p));
  }
  return out;
}

std::optional<std::string> Site::find_template(std::string_view tenant, std::string_view name,
                                               std::string* source) const {
  std::string path = "_templates/" + std::string(name) + ".html";
  if (auto f = store_.get(tenant, path)) {
    if (source) *source = "tenant";
    return std::move(f->data);
  }
  if (auto f = store_.get(content::kHostTree, path)) {
    if (source) *source = "host";
    return std::move(f->data);
  }
  auto& builtin = builtin_templates();
  if (auto it = builtin.find(std::string(name)); it != builtin.end()) {
    if (source) *source = "built-in";
    return it->second;
  }
  return std::nullopt;
}

bj::object Site::base_data(std::string_view tenant) const {
  bj::object data;
  bj::object files;
  for (const auto& f : store_.list(tenant, "_data/")) {
    if (!f.ends_with(".json") || f.find('/', 6) != std::string::npos) continue;
    auto file = store_.get(tenant, f);
    std::string err;
    auto v = file ? json::parse(file->data, &err) : std::nullopt;
    if (v) files[strip_ext(f.substr(6))] = std::move(*v);
  }
  auto* site = files.if_contains("site");
  data["site"] = site && site->is_object() ? *site : bj::value(bj::object());
  data["data"] = std::move(files);
  std::string now = base::format_utc_ms(base::now_unix_ms());
  data["now"] = bj::object{{"year", now.substr(0, 4)}, {"date", now.substr(0, 10)}, {"time", now}};
  return data;
}

std::optional<std::string> Site::render_template(std::string_view tenant, std::string_view name, bj::object data,
                                                 const std::map<std::string, std::string>* inline_templates,
                                                 std::string* error) const {
  render::TemplateOptions options;
  options.trusted_html = {"body"};
  options.partial = [&](std::string_view partial) -> std::optional<std::string> {
    if (inline_templates) {
      if (auto it = inline_templates->find(std::string(partial)); it != inline_templates->end()) return it->second;
    }
    auto text = find_template(tenant, partial);
    if (!text) return std::nullopt;
    return render::split_front_matter(*text).body;
  };
  std::string current(name);
  for (int depth = 0; depth <= kMaxLayouts; ++depth) {
    std::optional<std::string> text;
    if (inline_templates) {
      if (auto it = inline_templates->find(current); it != inline_templates->end()) text = it->second;
    }
    if (!text) text = find_template(tenant, current);
    if (!text) {
      if (error) *error = "no template named " + current;
      return std::nullopt;
    }
    auto doc = render::split_front_matter(*text);
    std::string err;
    auto out = render::render_mustache(doc.body, bj::value(data), options, &err);
    if (!out) {
      if (error) *error = "template " + current + ": " + err;
      return std::nullopt;
    }
    std::string layout = str(doc.fields, "layout");
    if (layout.empty()) return out;
    data["body"] = std::move(*out);
    current = layout;
  }
  if (error) *error = "layouts nest deeper than " + std::to_string(kMaxLayouts) + " from template " + std::string(name);
  return std::nullopt;
}

std::optional<std::string> Site::render_named(std::string_view tenant, std::string_view name, bj::object data,
                                              const std::map<std::string, std::string>* inline_templates,
                                              std::string* error) const {
  bj::object base = base_data(tenant);
  for (auto& [k, v] : base) {
    if (!data.contains(k)) data[k] = std::move(v);
  }
  // A box's JSON is data, never markup: only the host's own rendering may be "body".
  data.erase("body");
  return render_template(tenant, name, std::move(data), inline_templates, error);
}

std::vector<bj::object> Site::entries(const Visit& v, const std::string& dir) const {
  std::string prefix = dir.empty() ? "" : dir + "/";
  std::vector<bj::object> out;
  for (const auto& f : store_.list(v.tenant, prefix)) {
    if (is_private_path(f) || !page_url(f)) continue;
    std::string rest = f.substr(prefix.size());
    // Direct children: "x.md", or "x/index.md" as the page /dir/x.
    auto slash = rest.find('/');
    bool child = slash == std::string::npos ? strip_ext(rest) != "index"
                                            : rest.find('/', slash + 1) == std::string::npos &&
                                                  strip_ext(rest.substr(slash + 1)) == "index";
    if (!child) continue;
    auto p = load(v.tenant, f);
    if (!p || !p->broken.empty()) continue;
    if (flag(p->fields, "draft") && !v.drafts) continue;
    if (p->fields.contains("redirect")) continue;
    if (auto* req = p->fields.if_contains("requirements")) {
      if (!v.authorize || v.authorize(*req) != 200) continue;
    }
    bj::object e;
    for (const auto& [k, val] : p->fields) {
      if (!reserved_key(k)) e[k] = val;
    }
    e["url"] = p->url;
    e["path"] = p->path;
    e["title"] = title_of(*p);
    e["summary"] = summary_of(*p);
    e["date"] = str(p->fields, "date");
    e["image"] = str(p->fields, "image");
    if (!e.contains("tags") || !e["tags"].is_array()) e["tags"] = bj::array();
    if (!e.contains("order")) e["order"] = 0;
    out.push_back(std::move(e));
  }
  return out;
}

Result Site::render_page(const Visit& v, const Page& page, bool feed) const {
  Result r;
  r.file = page.path;
  r.kind = page.kind;
  if (!page.broken.empty()) {
    Result broken = fail(500);
    broken.error = page.path + ": " + page.broken;
    return broken;
  }
  if (flag(page.fields, "draft") && !v.drafts) return fail(404);
  if (auto* req = page.fields.if_contains("requirements")) {
    int status = v.authorize ? v.authorize(*req) : 403;
    if (status != 200) return fail(status);
    r.restricted = true;
  }
  if (auto target = str(page.fields, "redirect"); !target.empty() && !feed) {
    auto code = to_int(str(page.fields, "redirect-status")).value_or(301);
    if (code != 301 && code != 302 && code != 307 && code != 308 && code != 410) code = 301;
    r.kind = Kind::kRedirect;
    r.status = int(code);
    if (code != 410) r.location = target;
    return r;
  }
  bool wants_source = v.accept.find("text/markdown") != std::string::npos &&
                      v.accept.find("text/html") == std::string::npos;
  if (wants_source && !feed) {
    r.kind = Kind::kSource;
    r.status = 200;
    // A fragment's source is shown as text, never as a page: it would run with no CSP.
    r.content_type = page.kind == Kind::kMarkdown ? "text/markdown; charset=utf-8" : "text/plain; charset=utf-8";
    r.body = page.raw;
    r.etag = etag_of(r.body);
    return r;
  }
  if (page.kind == Kind::kDocument) {
    r.status = 200;
    r.content_type = "text/html; charset=utf-8";
    r.body = page.body;
    r.etag = etag_of(r.body);
    r.headers.emplace_back("content-security-policy", "frame-ancestors 'self'");
    return r;
  }

  std::string body_html;
  if (page.kind == Kind::kMarkdown) {
    body_html = render::markdown_to_html(page.body);
  } else {
    body_html = page.body;
  }
  bj::object data = base_data(v.tenant);
  // Without _data/site.json's title, the site is named by its domain, not left blank.
  if (auto& site = data["site"].as_object(); !site.contains("title") && !v.origin.empty()) {
    std::string host = v.origin.substr(v.origin.find("://") == std::string::npos ? 0 : v.origin.find("://") + 3);
    site["title"] = host.substr(0, host.find_first_of(":/"));
  }
  for (const auto& [k, val] : page.fields) {
    if (!reserved_key(k)) data[k] = val;
  }
  std::string title = title_of(page);
  data["title"] = title;
  data["summary"] = summary_of(page);
  if (!data.contains("date")) data["date"] = "";
  if (!data.contains("image")) data["image"] = "";
  bj::object page_obj = page.fields;
  page_obj["url"] = page.url;
  page_obj["path"] = page.path;
  page_obj["kind"] = kind_name(page.kind);
  page_obj["title"] = title;
  bool is_listing = page.fields.contains("list");
  if (is_listing && flag(page.fields, "feed")) page_obj["feed_url"] = (page.url == "/" ? "" : page.url) + "/feed.xml";
  data["user"] = v.user;
  bj::object query;
  for (const auto& [k, val] : v.query) query[k] = val;
  data["request"] = bj::object{{"path", v.path}, {"query", std::move(query)}};
  size_t lead = body_html.find_first_not_of(" \n");
  data["title_in_body"] = lead != std::string::npos && body_html.compare(lead, 3, "<h1") == 0;

  std::string template_name = str(page.fields, "template");
  if (is_listing) {
    std::string dir = str(page.fields, "list");
    while (!dir.empty() && dir.front() == '/') dir.erase(dir.begin());
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    if (!dir.empty() && (!base::is_safe_relative_path(dir) || is_private_path(dir))) {
      return fail(500, "list: " + dir + " is not a public directory of the content tree");
    }
    auto all = entries(v, dir);
    std::string tag;
    if (auto it = v.query.find("tag"); it != v.query.end()) {
      tag = it->second;
      std::erase_if(all, [&](const bj::object& e) {
        for (const auto& t : e.at("tags").as_array()) {
          if (t.is_string() && t.get_string() == tag) return false;
        }
        return true;
      });
      data["tag"] = tag;
    }
    std::string sort = str(page.fields, "sort");
    if (sort.empty()) sort = "-date";
    bool desc = sort.front() == '-';
    std::string key = desc ? sort.substr(1) : sort;
    if (key != "date" && key != "title" && key != "order" && key != "path") {
      return fail(500, "sort must be date, title, order, or path, with - for descending; not " + sort);
    }
    std::stable_sort(all.begin(), all.end(), [&](const bj::object& a, const bj::object& b) {
      auto cmp = [&](const bj::object& x, const bj::object& y) -> int {
        if (key == "order") {
          auto xi = x.at("order").is_int64() ? x.at("order").get_int64() : 0;
          auto yi = y.at("order").is_int64() ? y.at("order").get_int64() : 0;
          return xi < yi ? -1 : xi > yi ? 1 : 0;
        }
        std::string xs = str(x, key), ys = str(y, key);
        if (key == "date" && xs.empty() != ys.empty()) return xs.empty() ? (desc ? -1 : 1) : (desc ? 1 : -1);
        return xs.compare(ys);
      };
      int c = cmp(a, b);
      if (c == 0) return str(a, "title") < str(b, "title");
      return desc ? c > 0 : c < 0;
    });
    if (feed) {
      std::string base = v.origin;
      std::string now = base::format_utc_ms(base::now_unix_ms());
      std::string updated = all.empty() ? now : atom_time(str(all.front(), "date"), now);
      std::string self = base + (page.url == "/" ? "" : page.url) + "/feed.xml";
      std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<feed xmlns=\"http://www.w3.org/2005/Atom\">\n";
      out += "<title>" + xml_escape(title) + "</title>\n<link href=\"" + xml_escape(base + page.url) + "\"/>\n";
      out += "<link rel=\"self\" href=\"" + xml_escape(self) + "\"/>\n<id>" + xml_escape(base + page.url) + "</id>\n";
      out += "<updated>" + xml_escape(updated) + "</updated>\n";
      for (size_t i = 0; i < all.size() && i < kFeedEntries; ++i) {
        const auto& e = all[i];
        std::string link = base + str(e, "url");
        out += "<entry>\n<title>" + xml_escape(str(e, "title")) + "</title>\n<link href=\"" + xml_escape(link) +
               "\"/>\n<id>" + xml_escape(link) + "</id>\n<updated>" + xml_escape(atom_time(str(e, "date"), updated)) +
               "</updated>\n";
        if (auto s = str(e, "summary"); !s.empty()) out += "<summary>" + xml_escape(s) + "</summary>\n";
        out += "</entry>\n";
      }
      out += "</feed>\n";
      r.kind = Kind::kFeed;
      r.status = 200;
      r.content_type = "application/atom+xml; charset=utf-8";
      r.body = std::move(out);
      r.etag = etag_of(r.body);
      return r;
    }
    int64_t per_page = std::clamp<int64_t>(to_int(str(page.fields, "per-page")).value_or(kDefaultPerPage), 1, kMaxPerPage);
    int64_t total = int64_t(all.size());
    int64_t pages = std::max<int64_t>(1, (total + per_page - 1) / per_page);
    int64_t n = 1;
    if (auto it = v.query.find("page"); it != v.query.end()) {
      auto parsed = to_int(it->second);
      if (!parsed || *parsed < 1 || *parsed > pages) return fail(404);
      n = *parsed;
    }
    bj::array list;
    for (int64_t i = (n - 1) * per_page; i < std::min(total, n * per_page); ++i) list.push_back(std::move(all[size_t(i)]));
    data["pages"] = std::move(list);
    if (pages > 1) {
      auto url_for = [&](int64_t k) {
        auto q = v.query;
        q.erase("page");
        if (k > 1) q["page"] = std::to_string(k);
        return page.url + query_string(q);
      };
      bj::array numbers;
      for (int64_t k = 1; k <= pages; ++k) {
        numbers.push_back(bj::object{{"n", k}, {"url", url_for(k)}, {"current", k == n}});
      }
      data["pagination"] = bj::object{{"page", n},          {"pages", pages},
                                      {"total", total},     {"has_prev", n > 1},
                                      {"has_next", n < pages}, {"prev_url", n > 1 ? url_for(n - 1) : ""},
                                      {"next_url", n < pages ? url_for(n + 1) : ""}, {"numbers", std::move(numbers)}};
    }
    if (template_name.empty()) template_name = "list";
    r.kind = Kind::kListing;
  } else if (feed) {
    return fail(404);
  }
  if (template_name.empty()) template_name = "page";
  data["page"] = std::move(page_obj);
  data["body"] = std::move(body_html);
  std::string err;
  auto html = render_template(v.tenant, template_name, std::move(data), nullptr, &err);
  if (!html) return fail(500, page.path + ": " + err);
  r.status = 200;
  r.content_type = "text/html; charset=utf-8";
  r.body = std::move(*html);
  r.etag = etag_of(r.body);
  r.headers.emplace_back("content-security-policy", kTemplatedCsp);
  return r;
}

Result Site::directory(const Visit& v, const std::string& dir) const {
  std::string source;
  if (!find_template(v.tenant, "directory", &source) || source == "built-in") return fail(404);
  Page synthetic;
  synthetic.path = dir.empty() ? "" : dir + "/";
  synthetic.url = dir.empty() ? "/" : "/" + dir;
  synthetic.kind = Kind::kFragment;
  synthetic.fields["list"] = dir.empty() ? "/" : dir;
  synthetic.fields["template"] = "directory";
  synthetic.fields["title"] = dir.empty() ? "Home" : humanize(last_segment(dir));
  synthetic.fields["directory"] = true;
  auto r = render_page(v, synthetic, false);
  if (r.status == 200) r.kind = Kind::kDirectory;
  r.file = synthetic.path;
  return r;
}

Result Site::visit(const Visit& v) const {
  std::string rel = v.path;
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
  if (!rel.empty() && is_private_path(rel)) return fail(404);

  // Feeds: /blog/feed.xml is the feed of the listing at /blog.
  if (rel == "feed.xml" || rel.ends_with("/feed.xml")) {
    std::string base = rel.size() > 9 ? rel.substr(0, rel.size() - 9) : "";
    for (const char* ext : {".md", ".html"}) {
      for (const std::string& candidate : {base.empty() ? std::string("index") : base, base.empty() ? std::string("index") : base + "/index"}) {
        if (auto p = load(v.tenant, candidate + ext); p && p->fields.contains("list") && flag(p->fields, "feed")) {
          return render_page(v, *p, true);
        }
      }
    }
  }

  if (!rel.empty()) {
    if (rel.ends_with(".md") && store_.get(v.tenant, rel)) {
      Result r;
      r.kind = Kind::kRedirect;
      r.status = 301;
      r.location = *page_url(rel);
      return r;
    }
    if (rel.ends_with(".html")) {
      if (auto p = load(v.tenant, rel)) return render_page(v, *p, false);
    } else if (auto f = store_.get(v.tenant, rel)) {
      Result r;
      r.status = 200;
      r.file = rel;
      r.content_type = f->content_type;
      r.etag = f->etag;
      r.body = std::move(f->data);
      return r;
    }
  }
  std::vector<std::string> candidates;
  if (rel.empty()) {
    candidates = {"index.md", "index.html"};
  } else {
    candidates = {rel + ".md", rel + ".html", rel + "/index.md", rel + "/index.html"};
  }
  for (const auto& c : candidates) {
    if (auto p = load(v.tenant, c)) return render_page(v, *p, false);
  }
  if (rel.empty() || !store_.list(v.tenant, rel + "/").empty()) return directory(v, rel);
  return fail(404);
}

}  // namespace goober::site
