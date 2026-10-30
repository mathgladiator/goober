#include <cctype>
#include "goober/site/site.h"
#include "goober/site/includes.h"

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
         k == "pagination" || k == "request" || k == "now" || k == "tag" || k == "title_in_body" || k == "document" ||
         k == "head" || k == "json";  // the host fills these, and they print unescaped
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

// A listing has feeds unless its front matter says feed: false (031 § 2).
bool feeds_on(const bj::object& fields) {
  auto* v = fields.if_contains("feed");
  if (!v) return true;
  return !((v->is_bool() && !v->get_bool()) || (v->is_string() && v->get_string() == "false"));
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

std::string xml_escape(std::string_view raw) {
  // XML 1.0 refuses C0 controls (but tab, newline, return) and broken UTF-8: one such byte in a
  // title would make every reader reject the whole feed or sitemap.
  std::string s = base::valid_utf8(raw);
  std::string out;
  for (char c : s) {
    if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
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

// Structured data for search engines: JSON (an object or array, or a string holding one) as
// the whole script tag, with < > & U+2028 U+2029 escaped as \u sequences, so no value can end
// the script or reach the page as markup (070 § Templates). nullopt for anything else.
std::optional<std::string> json_ld_script(const bj::value& ld) {
  std::optional<bj::value> v;
  if (ld.is_object() || ld.is_array()) {
    v = ld;
  } else if (ld.is_string()) {
    std::string err;
    v = json::parse(ld.get_string(), &err);
    if (v && !v->is_object() && !v->is_array()) v.reset();
  }
  if (!v) return std::nullopt;
  std::string text;
  for (char c : json::dump(*v)) {
    if (c == '<') text += "\\u003c";
    else if (c == '>') text += "\\u003e";
    else if (c == '&') text += "\\u0026";
    else text += c;
  }
  for (std::string_view sep : {std::string_view("\xe2\x80\xa8"), std::string_view("\xe2\x80\xa9")}) {
    for (size_t i = 0; (i = text.find(sep, i)) != std::string::npos;) {
      text.replace(i, sep.size(), sep.back() == '\xa8' ? "\\u2028" : "\\u2029");
    }
  }
  return "<script type=\"application/ld+json\">" + text + "</script>";
}

// A template written before the host built the tag wraps {{{json_ld}}} in a script of its own:
// that outer tag is dropped, so the page has one (Pit Boss, 2026-10-29). Only an ld+json
// script holding exactly the host's tag, with whitespace around it, is unwrapped.
static void unwrap_json_ld(std::string& html, const std::string& tag) {
  if (tag.empty()) return;
  size_t at = 0;
  while ((at = html.find(tag, at)) != std::string::npos) {
    size_t before = at;
    while (before > 0 && std::isspace(static_cast<unsigned char>(html[before - 1]))) --before;
    size_t after = at + tag.size();
    while (after < html.size() && std::isspace(static_cast<unsigned char>(html[after]))) ++after;
    size_t open = before > 0 && html[before - 1] == '>' ? html.rfind("<script", before - 1) : std::string::npos;
    bool wrapped = open != std::string::npos && html.find('>', open) == before - 1 &&
                   html.substr(open, before - open).find("application/ld+json") != std::string::npos &&
                   html.compare(after, 9, "</script>") == 0;
    if (!wrapped) {
      at += tag.size();
      continue;
    }
    html.replace(open, after + 9 - open, tag);
    at = open + tag.size();
  }
}

std::optional<std::string> Site::render_template(std::string_view tenant, std::string_view name, bj::object data,
                                                 const std::map<std::string, std::string>* inline_templates,
                                                 std::string* error, bool json_ld) const {
  render::TemplateOptions options;
  options.trusted_html = {"body", "head"};
  if (json_ld) options.trusted_html.insert("json_ld");
  // {{{json.title}}}: a value as JSON, safe inside a <script> (031 § 2).
  if (auto* j = data.if_contains("json"); j && j->is_object()) {
    for (const auto& kv : j->as_object()) options.trusted_html.insert("json." + std::string(kv.key()));
  }
  options.partial = [&](std::string_view partial) -> std::optional<std::string> {
    if (inline_templates) {
      if (auto it = inline_templates->find(std::string(partial)); it != inline_templates->end()) return it->second;
    }
    auto text = find_template(tenant, partial);
    if (!text) return std::nullopt;
    return render::split_front_matter(*text).body;
  };
  std::string current(name);
  // Only templates fill {{{head}}}: whatever the data said is dropped.
  std::string head;
  data["head"] = "";
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
    // A child template's <!--head-->...<!--/head--> blocks go to the layout's {{{head}}}, so a
    // template can add to <head> without replacing base (031 § 2).
    for (size_t at = out->find("<!--head-->"); at != std::string::npos; at = out->find("<!--head-->", at)) {
      size_t end = out->find("<!--/head-->", at);
      if (end == std::string::npos) break;
      head += out->substr(at + 11, end - at - 11);
      out->erase(at, end + 12 - at);
    }
    data["head"] = head;
    data["body"] = std::move(*out);
    current = layout;
  }
  if (error) *error = "layouts nest deeper than " + std::to_string(kMaxLayouts) + " from template " + std::string(name);
  return std::nullopt;
}

std::optional<std::string> Site::render_named(std::string_view tenant, std::string_view name, bj::object data,
                                              const std::map<std::string, std::string>* inline_templates,
                                              std::string* error) const {
  // A box's data never names the host's trusted slots ({{{head}}}, {{{json.*}}}).
  data.erase("head");
  data.erase("json");
  bj::object base = base_data(tenant);
  for (auto& [k, v] : base) {
    if (!data.contains(k)) data[k] = std::move(v);
  }
  // A box's JSON is data, never markup: only the host's own rendering may be "body".
  data.erase("body");
  // Structured data for search engines: the box gives JSON (an object or array, or a string
  // holding one), and the host writes the whole script tag, with < > & escaped as \u003c and
  // so on, so no value can end the script or reach the page as markup (070 § Templates).
  if (auto* ld = data.if_contains("json_ld")) {
    if (auto tag = json_ld_script(*ld)) {
      *ld = std::move(*tag);
    } else {
      data.erase("json_ld");
    }
  }
  bool has_json_ld = data.contains("json_ld");
  std::string tag = has_json_ld ? std::string(data.at("json_ld").as_string()) : "";
  auto out = render_template(tenant, name, std::move(data), inline_templates, error, has_json_ld);
  if (out) unwrap_json_ld(*out, tag);
  return out;
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
  if (is_listing && feeds_on(page.fields)) {
    page_obj["feed"] = true;
    page_obj["feed_url"] = (page.url == "/" ? "" : page.url) + "/feed.xml";
    page_obj["rss_url"] = (page.url == "/" ? "" : page.url) + "/rss.xml";
  }
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
    // "list-template": "post" keeps only the pages written with that template (031 § 2).
    if (auto only = str(page.fields, "list-template"); !only.empty()) {
      std::erase_if(all, [&](const bj::object& e) { return str(e, "template") != only; });
    }
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
    if (feed && v.path.ends_with("rss.xml")) {
      // RSS 2.0, from the same entries as the Atom feed.
      std::string base = v.origin;
      std::string site_title, site_author;
      if (auto* st = data.if_contains("site"); st && st->is_object()) {
        site_title = str(st->as_object(), "title");
        site_author = str(st->as_object(), "author");
      }
      auto rfc822 = [](const std::string& date) -> std::string {
        // YYYY-MM-DD... as RFC 822 at midnight UTC; nothing for anything else.
        if (date.size() < 10 || date[4] != '-' || date[7] != '-') return "";
        static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int y = std::atoi(date.substr(0, 4).c_str()), m = std::atoi(date.substr(5, 2).c_str()), d = std::atoi(date.substr(8, 2).c_str());
        if (m < 1 || m > 12 || d < 1 || d > 31) return "";
        // Sakamoto's day of the week.
        static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
        int yy = m < 3 ? y - 1 : y;
        int w = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s, %02d %s %04d 00:00:00 +0000", kDays[w], d, kMonths[m - 1], y);
        return buf;
      };
      std::string link = base + page.url;
      std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<rss version=\"2.0\" xmlns:atom=\"http://www.w3.org/2005/Atom\" "
                        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:media=\"http://search.yahoo.com/mrss/\">\n<channel>\n";
      out += "<title>" + xml_escape(title) + "</title>\n<link>" + xml_escape(link) + "</link>\n";
      out += "<description>" + xml_escape(summary_of(page).empty() ? title : summary_of(page)) + "</description>\n";
      out += "<atom:link href=\"" + xml_escape(base + (page.url == "/" ? "" : page.url) + "/rss.xml") +
             "\" rel=\"self\" type=\"application/rss+xml\"/>\n";
      if (!all.empty()) {
        if (auto when = rfc822(str(all.front(), "date")); !when.empty()) out += "<lastBuildDate>" + when + "</lastBuildDate>\n";
      }
      for (size_t i = 0; i < all.size() && i < kFeedEntries; ++i) {
        const auto& e = all[i];
        std::string item = base + str(e, "url");
        out += "<item>\n<title>" + xml_escape(str(e, "title")) + "</title>\n<link>" + xml_escape(item) +
               "</link>\n<guid isPermaLink=\"true\">" + xml_escape(item) + "</guid>\n";
        if (auto when = rfc822(str(e, "date")); !when.empty()) out += "<pubDate>" + when + "</pubDate>\n";
        if (auto s = str(e, "summary"); !s.empty()) out += "<description>" + xml_escape(s) + "</description>\n";
        std::string a = str(e, "author");
        if (a.empty()) a = site_author;
        if (!a.empty()) out += "<dc:creator>" + xml_escape(a) + "</dc:creator>\n";
        if (auto img = str(e, "image"); !img.empty()) {
          out += "<media:content url=\"" + xml_escape(img.starts_with("http") ? img : base + img) + "\" medium=\"image\"/>\n";
        }
        for (const auto& t : e.at("tags").as_array()) {
          if (t.is_string()) out += "<category>" + xml_escape(std::string(t.get_string())) + "</category>\n";
        }
        out += "</item>\n";
      }
      out += "</channel>\n</rss>\n";
      (void)site_title;
      r.kind = Kind::kFeed;
      r.status = 200;
      r.content_type = "application/rss+xml; charset=utf-8";
      r.body = std::move(out);
      r.etag = etag_of(r.body);
      return r;
    }
    if (feed) {
      std::string base = v.origin;
      std::string now = base::format_utc_ms(base::now_unix_ms());
      std::string updated = all.empty() ? now : atom_time(str(all.front(), "date"), now);
      std::string self = base + (page.url == "/" ? "" : page.url) + "/feed.xml";
      std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<feed xmlns=\"http://www.w3.org/2005/Atom\">\n";
      out += "<title>" + xml_escape(title) + "</title>\n<link href=\"" + xml_escape(base + page.url) + "\"/>\n";
      out += "<link rel=\"self\" href=\"" + xml_escape(self) + "\"/>\n<id>" + xml_escape(base + page.url) + "</id>\n";
      out += "<updated>" + xml_escape(updated) + "</updated>\n";
      // Atom needs an author: the site, by its title (or its domain).
      std::string site_title;
      if (auto* st = data.if_contains("site"); st && st->is_object()) site_title = str(st->as_object(), "title");
      out += "<author><name>" + xml_escape(site_title.empty() ? title : site_title) + "</name></author>\n";
      for (size_t i = 0; i < all.size() && i < kFeedEntries; ++i) {
        const auto& e = all[i];
        std::string link = base + str(e, "url");
        out += "<entry>\n<title>" + xml_escape(str(e, "title")) + "</title>\n<link href=\"" + xml_escape(link) +
               "\"/>\n<id>" + xml_escape(link) + "</id>\n<updated>" + xml_escape(atom_time(str(e, "date"), updated)) +
               "</updated>\n";
        if (auto s = str(e, "summary"); !s.empty()) out += "<summary>" + xml_escape(s) + "</summary>\n";
        std::string a = str(e, "author");
        if (a.empty()) {
          if (auto* st = data.if_contains("site"); st && st->is_object()) a = str(st->as_object(), "author");
        }
        if (!a.empty()) out += "<author><name>" + xml_escape(a) + "</name></author>\n";
        if (auto img = str(e, "image"); !img.empty()) {
          out += "<link rel=\"enclosure\" href=\"" + xml_escape(img.starts_with("http") ? img : base + img) + "\"/>\n";
        }
        for (const auto& t : e.at("tags").as_array()) {
          if (t.is_string()) out += "<category term=\"" + xml_escape(std::string(t.get_string())) + "\"/>\n";
        }
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
  // What a search engine and a link preview read (031 § 2): the canonical URL (a listing's later
  // pages keep their ?page=), the image as an absolute URL, the author, and the kind of page.
  {
    std::string canonical = str(page.fields, "canonical");
    if (!(canonical.starts_with("https://") || canonical.starts_with("http://"))) {
      canonical = v.origin.empty() ? "" : v.origin + page.url;
      if (!canonical.empty() && is_listing) {
        if (auto it = v.query.find("page"); it != v.query.end() && it->second != "1") canonical += "?page=" + it->second;
      }
    }
    data["canonical"] = canonical;
    std::string image = str(page.fields, "image");
    data["image_url"] = image.starts_with("http") ? image : (!image.empty() && image.front() == '/' && !v.origin.empty()) ? v.origin + image : "";
    if (!data.contains("author") || !data["author"].is_string()) {
      std::string site_author;
      if (auto* st = data.if_contains("site"); st && st->is_object()) site_author = str(st->as_object(), "author");
      data["author"] = site_author;
    }
    bool article = template_name == "post";
    data["og_type"] = article ? "article" : "website";
    auto* tags = page.fields.if_contains("tags");
    data["has_tags"] = tags && tags->is_array() && !tags->as_array().empty();
    // A post without a json_ld field gets a BlogPosting built from its front matter.
    if (article && !data.contains("json_ld")) {
      bj::object ld{{"@context", "https://schema.org"}, {"@type", "BlogPosting"}, {"headline", title}};
      if (!canonical.empty()) ld["mainEntityOfPage"] = canonical, ld["url"] = canonical;
      if (auto d = str(page.fields, "date"); !d.empty()) ld["datePublished"] = d;
      std::string updated = str(page.fields, "updated");
      if (updated.empty()) updated = str(page.fields, "date");
      if (!updated.empty()) ld["dateModified"] = updated;
      if (auto s = summary_of(page); !s.empty()) ld["description"] = s;
      if (auto a = data["author"].as_string(); !a.empty()) ld["author"] = bj::object{{"@type", "Person"}, {"name", a}};
      if (auto img = std::string(data["image_url"].as_string()); !img.empty()) ld["image"] = img;
      if (data["has_tags"].as_bool()) ld["keywords"] = *tags;
      std::string site_title;
      if (auto* st = data.if_contains("site"); st && st->is_object()) site_title = str(st->as_object(), "title");
      if (!site_title.empty()) ld["publisher"] = bj::object{{"@type", "Organization"}, {"name", site_title}};
      data["json_ld"] = std::move(ld);
    }
  }
  {
    // Every plain value again as JSON text escaped for a <script> ({{{json.title}}}).
    bj::object js;
    auto script_safe = [](std::string s) {
      std::string out;
      for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch == '<') out += "\\u003c";
        else if (ch == '>') out += "\\u003e";
        else if (ch == '&') out += "\\u0026";
        else if (ch == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                 (static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9)) {
          out += static_cast<unsigned char>(s[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
          i += 2;
        } else {
          out += s[i];
        }
      }
      return out;
    };
    for (const auto& [k, v] : data) {
      if (k == "body" || k == "json_ld" || k == "page" || k == "site" || k == "data" || k == "user" || k == "pages" ||
          k == "request" || k == "json") {
        continue;
      }
      js[k] = script_safe(json::dump(v));
    }
    data["json"] = std::move(js);
  }
  data["page"] = std::move(page_obj);
  data["body"] = std::move(body_html);
  // A page's json_ld field becomes the host-built tag, as a box's does.
  bool has_json_ld = false;
  std::string ld_tag;
  if (auto* ld = data.if_contains("json_ld")) {
    if (auto tag = json_ld_script(*ld)) {
      ld_tag = *tag;
      *ld = std::move(*tag);
      has_json_ld = true;
    } else {
      data.erase("json_ld");
    }
  }
  std::string err;
  auto html = render_template(v.tenant, template_name, std::move(data), nullptr, &err, has_json_ld);
  if (html) unwrap_json_ld(*html, ld_tag);
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

// Every page's HTML gets its partials (design/machine/027.Partials.md), and its ETag is taken
// after, so one partial edit changes every page that includes it.
Result Site::visit(const Visit& v) const {
  Result r = visit_unexpanded(v);
  if (r.status != 200 || r.kind == Kind::kStatic || !r.content_type.starts_with("text/html")) return r;
  auto e = expand_includes(store_, v.tenant, r.body, v.path, !v.user.is_null());
  if (e.used.empty() && e.missing.empty() && !e.by_auth) return r;
  if (!e.errors.empty()) {
    std::string why;
    for (const auto& n : e.errors) why += (why.empty() ? "" : "; ") + n;
    r.headers.emplace_back("x-goober-partial-error", why.substr(0, 400));
  }
  r.body = std::move(e.html);
  r.etag = etag_of(r.body);
  // A signed-in variant: the answer depends on the cookie, so no shared cache may keep it.
  if (e.by_auth) r.restricted = true;
  if (!e.missing.empty()) {
    std::string names;
    for (const auto& n : e.missing) names += (names.empty() ? "" : ", ") + n;
    r.headers.emplace_back("x-goober-partial-missing", names);
  }
  return r;
}

Result Site::visit_unexpanded(const Visit& v) const {
  std::string rel = v.path;
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
  if (!rel.empty() && is_private_path(rel)) return fail(404);

  // Feeds: /blog/feed.xml is the feed of the listing at /blog.
  // /blog/rss.xml is the same entries as RSS 2.0.
  if (rel == "feed.xml" || rel.ends_with("/feed.xml") || rel == "rss.xml" || rel.ends_with("/rss.xml")) {
    size_t name = rel.ends_with("rss.xml") ? 8 : 9;
    std::string base = rel.size() > name ? rel.substr(0, rel.size() - name) : "";
    for (const char* ext : {".md", ".html"}) {
      for (const std::string& candidate : {base.empty() ? std::string("index") : base, base.empty() ? std::string("index") : base + "/index"}) {
        if (auto p = load(v.tenant, candidate + ext); p && p->fields.contains("list") && feeds_on(p->fields)) {
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
  // The platform's own sitemap and llms.txt, when no file of the tenant's answers (031 § 1).
  if (rel == "sitemap.xml" || rel == "sitemap-pages.xml" || rel == "llms.txt") {
    Result r;
    r.status = 200;
    r.kind = Kind::kStatic;
    // With "sitemaps" in _data/site.json (a box's own, like /places/sitemap.xml), /sitemap.xml is
    // an index of those and /sitemap-pages.xml, which lists the pages.
    bj::array others;
    {
      auto data = base_data(v.tenant);
      if (auto* s = data["site"].as_object().if_contains("sitemaps"); s && s->is_array()) others = s->as_array();
    }
    if (rel == "llms.txt") {
      r.body = llms_txt(v.tenant, v.origin);
    } else if (rel == "sitemap.xml" && !others.empty()) {
      r.body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<sitemapindex xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n"
               "  <sitemap><loc>" + xml_escape(v.origin + "/sitemap-pages.xml") + "</loc></sitemap>\n";
      for (const auto& o : others) {
        if (!o.is_string()) continue;
        std::string u(o.as_string());
        if (!u.starts_with("http")) u = v.origin + u;
        r.body += "  <sitemap><loc>" + xml_escape(u) + "</loc></sitemap>\n";
      }
      r.body += "</sitemapindex>\n";
    } else if (rel == "sitemap-pages.xml" && others.empty()) {
      return fail(404);
    } else {
      r.body = sitemap_xml(v.tenant, v.origin);
    }
    r.content_type = rel == "llms.txt" ? "text/plain; charset=utf-8" : "application/xml; charset=utf-8";
    r.etag = etag_of(r.body);
    return r;
  }
  return fail(404);
}

std::vector<Page> Site::listed_pages(std::string_view tenant) const {
  std::vector<Page> out;
  for (auto& p : pages(tenant, "")) {
    if (!p.broken.empty() || is_private_path(p.path) || flag(p.fields, "draft") || flag(p.fields, "noindex")) continue;
    if (p.fields.contains("requirements") || p.fields.contains("redirect")) continue;
    if (auto* s = p.fields.if_contains("sitemap"); s && ((s->is_bool() && !s->get_bool()) || (s->is_string() && s->get_string() == "false"))) continue;
    out.push_back(std::move(p));
  }
  std::sort(out.begin(), out.end(), [](const Page& a, const Page& b) { return a.url < b.url; });
  return out;
}

namespace {

std::string field_text(const bj::object& o, const char* k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->get_string()) : std::string();
}

// The page's canonical URL: its own canonical field when absolute, else the origin and URL.
std::string canonical_of(const Page& p, const std::string& origin) {
  std::string c = field_text(p.fields, "canonical");
  if (c.starts_with("https://") || c.starts_with("http://")) return c;
  return origin + p.url;
}

std::string page_title(const Page& p) {
  std::string t = field_text(p.fields, "title");
  return t.empty() ? (p.url == "/" ? std::string("Home") : p.url) : t;
}

}  // namespace

std::string Site::sitemap_xml(std::string_view tenant, const std::string& origin) const {
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n";
  for (const auto& p : listed_pages(tenant)) {
    out += "  <url><loc>" + xml_escape(canonical_of(p, origin)) + "</loc>";
    std::string when = field_text(p.fields, "updated");
    if (when.empty()) when = field_text(p.fields, "date");
    if (when.size() >= 10 && when[4] == '-' && when[7] == '-') out += "<lastmod>" + xml_escape(when.substr(0, 10)) + "</lastmod>";
    out += "</url>\n";
  }
  return out + "</urlset>\n";
}

std::string Site::llms_txt(std::string_view tenant, const std::string& origin) const {
  auto data = base_data(tenant);
  const auto& site = data["site"].as_object();
  std::string title = field_text(site, "title");
  if (title.empty()) title = origin.substr(origin.find("//") == std::string::npos ? 0 : origin.find("//") + 2);
  std::string description = field_text(site, "description");
  auto one_line = [](std::string s) {
    for (auto& ch : s) {
      if (ch == '\n' || ch == '\r') ch = ' ';
    }
    return s;
  };
  std::string out = "# " + one_line(title) + "\n\n";
  if (!description.empty()) out += "> " + one_line(description) + "\n\n";
  out += "## Pages\n\n";
  for (const auto& p : listed_pages(tenant)) {
    std::string about = field_text(p.fields, "description");
    if (about.empty()) about = field_text(p.fields, "summary");
    out += "- [" + one_line(page_title(p)) + "](" + canonical_of(p, origin) + ")" + (about.empty() ? "" : ": " + one_line(about)) + "\n";
  }
  return out;
}

}  // namespace goober::site
