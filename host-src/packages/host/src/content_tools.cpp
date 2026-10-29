// The operator tools for building sites (design/machine/007.Content.md §9): the guide,
// pages, templates, and attachments.

#include <fcntl.h>

#include <algorithm>
#include <cctype>
#include <set>

#include "backups.h"
#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/process.h"
#include "goober/base/names.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/render/frontmatter.h"
#include "goober/render/mustache.h"
#include "goober/router/path.h"
#include "goober/site/includes.h"


namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

constexpr const char* kGuide = R"guide(# Building a site on goober

Each tenant has a content tree. Everything below is written with the tools named in brackets,
persisted to disk through the host WAL, and live at once.

## Pages [page_put, page_get, page_list, page_render]
A page's URL is its path without the extension; an index page is its directory's URL.
  blog/hello.md -> /blog/hello        blog/index.md -> /blog        index.md -> /
Three formats:
  - markdown: CommonMark plus tables, task lists, ~~strike~~, and bare URLs. Raw HTML is escaped.
  - fragment: HTML that the page's template wraps (written as .html with front matter).
  - document: a whole HTML page, served exactly as written, with no template.
page_put takes path (the URL), format, body, and optional title, template, and fields. Fields
become front matter. Useful fields:
  title, date (YYYY-MM-DD), summary, image (/attachment/1.png), tags (["news"]), order (number),
  draft (true hides the page), requirements ([["owner"]] to restrict it; public otherwise),
  redirect (/new) with redirect-status (301, 302, 307, 308, or 410).
Preview anything, drafts included, with page_render before telling a person it is done.

## Templates [template_put, template_list]
Mustache, stored as _templates/$name.html. A page picks one with the template field; the
default is "page", or "list" for a listing. Built-in base, page, and list templates exist, so
a site looks clean with no templates at all; write your own to restyle.
  {{title}} {{date}} {{summary}} {{any_front_matter_field}}  escaped values
  {{{body}}}                     the page's rendered HTML (the only unescaped value)
  {{site.title}} {{#site.nav}}<a href="{{url}}">{{label}}</a>{{/site.nav}}   from _data/site.json
  {{data.NAME.x}}                from _data/NAME.json
  {{#user}}Hi {{email}}{{/user}} the signed-in person, or nothing
  {{page.url}} {{request.path}} {{now.year}}
  {{#pages}}...{{/pages}}        listing entries: url, title, date, summary, image, tags, and fields
  {{#pagination}}{{#has_prev}}<a href="{{prev_url}}">Newer</a>{{/has_prev}} {{page}}/{{pages}}{{/pagination}}
  {{> nav}}                      includes _templates/nav.html
A template may begin with front matter "layout: base"; its output becomes {{{body}}} in base.
So a typical site has base (the <html>, header, nav, footer), page, and list.

## Listings
A page with the field list: blog lists the pages directly inside blog/ (blog/x.md, and
blog/y/index.md as /blog/y). Drafts and pages the visitor can't open are left out.
  sort: -date (default), date, title, -title, order, path      per-page: 20 (at most 200)
  ?page=2 pages through; ?tag=news filters; feed: true also serves /blog/feed.xml (Atom).
A directory without an index page is listed only if _templates/directory.html exists.

## Site data [content_put]
_data/site.json holds the site's title, navigation, and footer:
  {"title": "Bob's Plumbing", "nav": [{"label": "Blog", "url": "/blog"}], "footer": "Since 1987",
   "stylesheet": "/css/site.css"}
Static files (CSS, JS, fonts, favicons) go anywhere with content_put. Paths with a segment that
starts with "_" are never served.

## Images and files [attachment_put, attachment_list, attachment_get, attachment_delete]
attachment_put takes base64 bytes, a file already on the site (file: "/hero.jpg"), or an https
url to fetch, plus name, alt, and visibility (public, or members). A URL on this host's own
domain is read from its files. It answers with /attachment/$id.$ext; use that URL in pages:
  ![A red truck](/attachment/3.jpg)      or   image: /attachment/3.jpg
PNG, JPEG, GIF, WebP, AVIF, ICO, SVG, and PDF, detected from the bytes. People can also upload
at /attachment/ in a browser, as an owner of the tenant. base64 suits small files: for anything
over about 100 KB, give url or file, or call attachment_upload_url and PUT the bytes to the URL
it returns (curl -T photo.jpg '<url>'), once, within 15 minutes. Without code, send the base64
in pieces: attachment_upload_part {ticket: the upload_url, base64: the next 100000 characters},
then done: true with the last piece. If a call fails, nothing is stored:
fix the call; never paste its arguments into a page.

## Boxes with templates
A box route may name a template; browsers then get the box's JSON rendered through it, while
API callers get the JSON.
)guide";

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

bool bool_arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  return it != args.end() && it->value().is_bool() && it->value().get_bool();
}

bool valid_template_name(std::string_view n) {
  if (n.empty() || n.size() > 63 || !std::islower(static_cast<unsigned char>(n[0]))) return false;
  for (char c : n) {
    if (!std::islower(static_cast<unsigned char>(c)) && !std::isdigit(static_cast<unsigned char>(c)) && c != '_' &&
        c != '-') {
      return false;
    }
  }
  return true;
}

// A front matter line for one field, readable by render::split_front_matter.
std::string front_matter_line(const std::string& key, const bj::value& v) {
  if (v.is_string()) {
    std::string_view s = v.get_string();
    bool plain = !s.empty() && s.find('\n') == std::string_view::npos && s.front() != ' ' && s.back() != ' ' &&
                 s.front() != '"' && s.front() != '\'' && s.front() != '[' && s.front() != '{' && s != "true" &&
                 s != "false" && s.find_first_not_of("-0123456789") != std::string_view::npos && s.front() != '#';
    return key + ": " + (plain ? std::string(s) : json::dump(v)) + "\n";
  }
  return key + ": " + json::dump(v) + "\n";
}

// The page file for a URL: an existing page if there is one, else a new path.
std::string file_for(const site::Site& site, const std::string& tenant, std::string url, const std::string& ext) {
  bool index = url.ends_with("/");
  while (!url.empty() && url.front() == '/') url.erase(url.begin());
  while (!url.empty() && url.back() == '/') url.pop_back();
  if (url.empty()) return "index" + ext;
  if (index) return url + "/index" + ext;  // asked for a directory's index: there, whatever else exists
  for (const char* stem : {"", "/index"}) {
    for (const char* e : {".md", ".html"}) {
      if (site.load(tenant, url + stem + e)) return url + stem + ext;
    }
  }
  return url + (index ? "/index" : "") + ext;
}

// Fetches an https URL with the system curl, for attachment_put (007 §7).
std::optional<std::string> fetch_https(const std::string& url, size_t max_bytes, std::string* error) {
  if (!url.starts_with("https://")) return *error = "only https URLs can be fetched", std::nullopt;
  for (char c : url) {
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return *error = "the URL holds a space or control character", std::nullopt;
  }
  std::string limit = std::to_string(max_bytes);
  // -q first: no ~/.curlrc. -g: no URL globbing, so one URL is one request. --noproxy:
  // the environment's proxy settings don't apply.
  auto r = base::run_process({"curl", "-q", "-g", "--noproxy", "*", "-sS", "-f", "-L", "--max-redirs", "3", "--proto", "=https", "--proto-redir",
                              "=https", "--max-time", "20", "--max-filesize", limit, "-A", "goober-host", "--", url},
                             "", max_bytes, 25000);
  if (!r.started) return *error = "could not run curl", std::nullopt;
  if (r.truncated) return *error = "the file is larger than " + limit + " bytes", std::nullopt;
  if (r.timed_out) return *error = "fetching " + url + " took too long", std::nullopt;
  if (r.exit_code != 0) {
    std::string msg = r.err;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return *error = "fetching " + url + " failed: " + (msg.empty() ? "curl exited with an error" : msg), std::nullopt;
  }
  return r.out;
}

bj::object attachment_json(const attachments::Attachment& a, const std::vector<std::string>* used_by) {
  bj::array used;
  if (used_by) {
    for (const auto& p : *used_by) used.push_back(bj::value(p));
  }
  return bj::object{{"id", a.id},
                    {"url", a.url()},
                    {"markdown", "![" + (a.alt.empty() ? a.name : a.alt) + "](" + a.url() + ")"},
                    {"content_type", a.content_type},
                    {"size", a.size},
                    {"name", a.name},
                    {"alt", a.alt},
                    {"visibility", a.visibility},
                    {"created", base::format_utc_ms(a.created_ms)},
                    {"used_by", std::move(used)}};
}

}  // namespace

void Host::register_content_tools() {
  using mcp::boolean_property;
  using mcp::enum_property;
  using mcp::object_property;
  using mcp::object_schema;
  using mcp::string_property;

  auto tenant_prop = string_property("A tenant name from the host config, like playground.");
  auto known_tenant = [this](const std::string& t) {
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == t) return true;
    }
    return false;
  };

  mcp_->add_tool({"site_guide",
                  "Read this first when building a site: how pages, templates, listings, site data, and images work "
                  "on goober, and which tool sets each.",
                  object_schema({}, {}), [](const bj::object&) { return ToolResult::ok(bj::value(kGuide)); }});

  mcp_->add_tool(
      {"page_put",
       "Writes a page at a URL. format is markdown, fragment (HTML wrapped in a template), or document (a whole HTML "
       "page, served as written). title, template, and fields become front matter (fields like date, summary, image, "
       "tags, draft, requirements, list, sort, per-page, feed); a document takes title, summary, date, tags, draft, "
       "and requirements, and is still served as written. A path ending in / writes that directory's index "
       "(/about/ is about/index.*). Replaces every file that answers the URL, whatever its format. The body is stored "
       "byte for byte: entities like &amp; are kept as written.",
       object_schema({{"tenant", tenant_prop},
                      {"path", string_property("The page's URL, like /about, /blog/hello, or / for the home page.")},
                      {"format", enum_property("How body is written.", {"markdown", "fragment", "document"})},
                      {"body", string_property("The page: markdown, an HTML fragment, or a whole HTML document.")},
                      {"title", string_property("The title; defaults to the first heading.")},
                      {"template", string_property("The template to wrap the page in; defaults to page, or list.")},
                      {"fields", object_property("More front matter, like {\"date\": \"2026-10-26\", \"tags\": "
                                                 "[\"news\"]} or {\"list\": \"blog\"} for a listing.")}},
                     {"tenant", "path", "format", "body"}),
       [this, known_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto path = arg(args, "path");
         auto format = arg(args, "format");
         auto body = arg(args, "body");
         if (!tenant || !path || !format || !body) return ToolResult::fail("tenant, path, format, and body are required");
         if (!known_tenant(*tenant)) return ToolResult::fail("no domain in the host config serves tenant " + *tenant);
         if (*format != "markdown" && *format != "fragment" && *format != "document") {
           return ToolResult::fail("format must be markdown, fragment, or document");
         }
         bj::object fields;
         if (auto it = args.find("fields"); it != args.end()) {
           if (!it->value().is_object()) return ToolResult::fail("fields must be an object");
           fields = it->value().get_object();
         }
         if (auto t = arg(args, "title")) fields["title"] = *t;
         if (auto t = arg(args, "template")) fields["template"] = *t;
         if (*format == "document") {
           // A document is served as written; front matter (document: true) carries only what
           // decides who sees it and how it's listed.
           for (const auto& [k, v] : fields) {
             if (k == "template" || k == "list" || k == "sort" || k == "per-page" || k == "feed") {
               return ToolResult::fail("a document is served as written, so it takes no " + std::string(k) +
                                       "; use fragment to wrap HTML in a template");
             }
           }
           if (body->starts_with("---")) {
             return ToolResult::fail("a document can't start with ---, which would read as front matter");
           }
           if (!fields.empty()) fields["document"] = true;
         }
         std::string ext = *format == "markdown" ? ".md" : ".html";
         std::string file = file_for(*site_, *tenant, *path, ext);
         if (site::is_private_path(file)) return ToolResult::fail("a page's path can't have a segment starting with _");
         std::string text;
         if (*format != "document" || !fields.empty()) {
           text = "---\n";
           for (const auto& [k, v] : fields) {
             std::string key(k);
             bool ok = !key.empty();
             for (char c : key) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
             if (!ok) return ToolResult::fail("field names are letters, digits, _ and -: " + key);
             text += front_matter_line(key, v);
           }
           text += "---\n";
         }
         text += *body;
         std::string err;
         if (!content_->put(*tenant, file, text, &err)) return ToolResult::fail(err);
         // One page per URL: drop every other file that answers it (the other format, and
         // name.html beside name/index.html), so what was just written is what's served.
         bj::array removed;
         if (auto url = site::page_url(file)) {
           std::string stem = url->substr(1);
           std::vector<std::string> others = stem.empty() ? std::vector<std::string>{"index.md", "index.html"}
                                                          : std::vector<std::string>{stem + ".md", stem + ".html",
                                                                                     stem + "/index.md", stem + "/index.html"};
           for (const auto& other : others) {
             if (other != file && content_->get(*tenant, other) && content_->remove(*tenant, other, &err)) {
               removed.push_back(bj::value(other));
             }
           }
         }
         bj::array warnings;
         std::string tmpl = fields.contains("template") && fields["template"].is_string()
                                ? std::string(fields["template"].get_string())
                                : (fields.contains("list") ? "list" : "page");
         if (*format != "document" && !site_->find_template(*tenant, tmpl)) {
           warnings.push_back(bj::value("no template named " + tmpl + ": write it with template_put"));
         }
         auto url = site::page_url(file);
         return ToolResult::ok(bj::object{{"tenant", *tenant}, {"file", file}, {"url", url ? *url : ""},
                                          {"replaced", std::move(removed)}, {"warnings", std::move(warnings)}});
       }});

  mcp_->add_tool(
      {"page_get", "Reads the page at a URL (or a file path like blog/hello.md): its file, format, front matter, and body.",
       object_schema({{"tenant", tenant_prop}, {"path", string_property("A URL like /blog/hello, or a file path.")}},
                     {"tenant", "path"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto path = arg(args, "path");
         if (!tenant || !path) return ToolResult::fail("tenant and path are required");
         std::optional<site::Page> p;
         std::string rel = *path;
         while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
         if (rel.ends_with(".md") || rel.ends_with(".html")) p = site_->load(*tenant, rel);
         // A URL, resolved the way a visit resolves it: name.md, name.html, name/index.md, name/index.html.
         while (!rel.empty() && rel.back() == '/') rel.pop_back();
         std::vector<std::string> candidates = rel.empty() ? std::vector<std::string>{"index.md", "index.html"}
                                                           : std::vector<std::string>{rel + ".md", rel + ".html",
                                                                                      rel + "/index.md", rel + "/index.html"};
         for (const auto& c : candidates) {
           if (!p) p = site_->load(*tenant, c);
         }
         if (!p) return ToolResult::fail("no page at " + *path);
         return ToolResult::ok(bj::object{{"file", p->path}, {"url", p->url}, {"kind", site::kind_name(p->kind)},
                                          {"fields", p->fields}, {"body", p->body}});
       }});

  // Shared partials (design/machine/027.Partials.md): _partials/$name.html, included by
  // <!--#include name--> in pages and box HTML; the last ten versions kept for rollback.
  auto partial_file = [](const std::string& name) { return "_partials/" + name + ".html"; };
  auto versions_of = [this](const std::string& tenant, const std::string& name) {
    auto files = content_->list(tenant, "_partials/_versions/" + name + "/");
    std::sort(files.begin(), files.end());
    return files;
  };
  auto partial_name_prop = string_property(
      "The partial's name, 1 to 64 of [a-z0-9_-], like nav or footer. A signed-in visitor gets name@auth "
      "when it exists (write it as \"nav@auth\").");
  auto check_name = [](const std::string& name) {
    std::string base = name.ends_with("@auth") ? name.substr(0, name.size() - 5) : name;
    return site::is_partial_name(base);
  };
  mcp_->add_tool(
      {"partial_put",
       "Writes a shared partial (a nav, a footer): every page and box page with <!--#include name--> gets it at "
       "once, with no page re-put, and their ETags change. Links may carry data-match=\"/places\" (that path and "
       "below), data-match-exact=\"/\", or data-key=\"x\" (a page's <meta name=\"nav-key\" content=\"x\">): the "
       "best fit for each request gets aria-current=\"page\" and class active. The previous version is kept "
       "(ten in all).",
       object_schema({{"tenant", tenant_prop}, {"name", partial_name_prop}, {"html", string_property("The HTML, as served.")}},
                     {"tenant", "name", "html"}),
       [this, partial_file, versions_of, check_name](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto name = arg(args, "name");
         auto html = arg(args, "html");
         if (!tenant || !name || !html) return ToolResult::fail("tenant, name, and html are required");
         if (!check_name(*name)) return ToolResult::fail("a partial's name is 1 to 64 of [a-z0-9_-], optionally ending @auth");
         if (html->size() > 256 * 1024) return ToolResult::fail("a partial is at most 256 KiB");
         std::string err;
         auto old = content_->get(*tenant, partial_file(*name));
         if (old) {
           char stamp[32];
           std::snprintf(stamp, sizeof stamp, "%015lld", static_cast<long long>(base::now_unix_ms()));
           if (!content_->put(*tenant, "_partials/_versions/" + *name + "/" + stamp + ".html", old->data, &err)) {
             return ToolResult::fail(err);
           }
           auto kept = versions_of(*tenant, *name);
           for (size_t i = 0; i + 10 < kept.size(); ++i) content_->remove(*tenant, kept[i], &err);
         }
         if (!content_->put(*tenant, partial_file(*name), *html, &err)) return ToolResult::fail(err);
         auto now = content_->get(*tenant, partial_file(*name));
         return ToolResult::ok(bj::object{{"file", partial_file(*name)},
                                          {"etag", now ? now->etag : ""},
                                          {"previous_etag", old ? bj::value(old->etag) : bj::value(nullptr)}});
       }});
  mcp_->add_tool(
      {"partial_get", "Reads a shared partial, or one of its kept versions (from partial_list).",
       object_schema({{"tenant", tenant_prop},
                      {"name", partial_name_prop},
                      {"version", string_property("A kept version's id, from partial_list; omit for the current one.")}},
                     {"tenant", "name"}),
       [this, partial_file, check_name](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto name = arg(args, "name");
         if (!tenant || !name || !check_name(*name)) return ToolResult::fail("tenant and a partial's name are required");
         auto version = arg(args, "version");
         if (version && (version->empty() || version->size() > 20 ||
                         !std::all_of(version->begin(), version->end(), [](char c) { return c >= '0' && c <= '9'; }))) {
           return ToolResult::fail("version is an id from partial_list");
         }
         std::string file = version ? "_partials/_versions/" + *name + "/" + *version + ".html" : partial_file(*name);
         auto f = content_->get(*tenant, file);
         if (!f) return ToolResult::fail("no partial " + *name + (version ? " version " + *version : ""));
         return ToolResult::ok(bj::object{{"name", *name}, {"etag", f->etag}, {"html", f->data}});
       }});
  mcp_->add_tool(
      {"partial_list",
       "A tenant's shared partials: name, etag, bytes, the kept versions to roll back to, and the pages whose bodies "
       "include each (box pages aren't listed: route_check says whether a route takes includes).",
       object_schema({{"tenant", tenant_prop}}, {"tenant"}),
       [this, versions_of](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!tenant) return ToolResult::fail("tenant is required");
         std::map<std::string, bj::array> used_by;
         for (const auto& p : site_->pages(*tenant, "")) {
           for (const auto& n : site::include_names(p.body)) used_by[n].push_back(bj::value(p.url));
         }
         bj::array out;
         for (const auto& f : content_->list(*tenant, "_partials/")) {
           if (f.starts_with("_partials/_versions/") || !f.ends_with(".html")) continue;
           std::string name = f.substr(10, f.size() - 15);
           auto file = content_->get(*tenant, f);
           bj::array versions;
           for (const auto& v : versions_of(*tenant, name)) {
             std::string id = v.substr(v.rfind('/') + 1);
             id.resize(id.size() - 5);
             versions.push_back(bj::value(id));
           }
           std::string base = name.ends_with("@auth") ? name.substr(0, name.size() - 5) : name;
           out.push_back(bj::object{{"name", name},
                                    {"etag", file ? file->etag : ""},
                                    {"bytes", file ? int64_t(file->data.size()) : 0},
                                    {"versions", std::move(versions)},
                                    {"included_by", used_by.count(base) ? used_by[base] : bj::array{}}});
         }
         // Markers with no partial at all, which serve empty.
         bj::array missing;
         for (const auto& [n, pages] : used_by) {
           if (!content_->get(*tenant, "_partials/" + n + ".html")) missing.push_back(bj::object{{"name", n}, {"included_by", pages}});
         }
         return ToolResult::ok(bj::object{{"partials", std::move(out)}, {"missing", std::move(missing)}});
       }});
  mcp_->add_tool(
      {"partial_delete", "Removes a shared partial; its markers then render empty. Its kept versions stay, so partial_get "
                         "and partial_put can bring it back.",
       object_schema({{"tenant", tenant_prop}, {"name", partial_name_prop}}, {"tenant", "name"}),
       [this, partial_file, check_name](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto name = arg(args, "name");
         if (!tenant || !name || !check_name(*name)) return ToolResult::fail("tenant and a partial's name are required");
         std::string err;
         auto old = content_->get(*tenant, partial_file(*name));
         if (!old) return ToolResult::fail("no partial " + *name);
         char stamp[32];
         std::snprintf(stamp, sizeof stamp, "%015lld", static_cast<long long>(base::now_unix_ms()));
         if (!content_->put(*tenant, "_partials/_versions/" + *name + "/" + stamp + ".html", old->data, &err) ||
             !content_->remove(*tenant, partial_file(*name), &err)) {
           return ToolResult::fail(err);
         }
         return ToolResult::ok(bj::object{{"deleted", partial_file(*name)}, {"kept_as", stamp}});
       }});

  mcp_->add_tool({"page_list", "Lists the pages under a directory (all of them by default), with URL, title, and status.",
                  object_schema({{"tenant", tenant_prop}, {"dir", string_property("A directory like blog, or empty.")}},
                                {"tenant"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    if (!tenant) return ToolResult::fail("tenant is required");
                    std::string dir = arg(args, "dir").value_or("");
                    while (!dir.empty() && dir.front() == '/') dir.erase(dir.begin());
                    if (!dir.empty() && !dir.ends_with("/")) dir += "/";
                    bj::array out;
                    for (const auto& p : site_->pages(*tenant, dir)) {
                      bj::object e{{"url", p.url}, {"file", p.path}, {"kind", site::kind_name(p.kind)}};
                      for (const char* k : {"title", "date", "template", "draft", "list", "requirements", "redirect"}) {
                        if (auto* v = p.fields.if_contains(k)) e[k] = *v;
                      }
                      out.push_back(std::move(e));
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool(
      {"page_render",
       "Renders a URL exactly as a visitor gets it (status, kind, and HTML), as a host operator, so pages with "
       "requirements render too. With drafts, drafts render; use it to preview before telling someone it's done.",
       object_schema({{"tenant", tenant_prop},
                      {"path", string_property("The URL, like /blog or /blog/hello.")},
                      {"query", object_property("Query parameters, like {\"page\": \"2\"}.")},
                      {"drafts", boolean_property("Render drafts too.")}},
                     {"tenant", "path"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto path = arg(args, "path");
         if (!tenant || !path) return ToolResult::fail("tenant and path are required");
         auto normal = router::normalize_path(*path);
         if (!normal) return ToolResult::fail("not a valid URL path");
         site::Visit v;
         v.tenant = *tenant;
         v.path = normal->path;
         if (auto it = args.find("query"); it != args.end() && it->value().is_object()) {
           for (const auto& [k, val] : it->value().get_object()) {
             if (val.is_string()) v.query[std::string(k)] = std::string(val.get_string());
           }
         }
         v.drafts = bool_arg(args, "drafts");
         v.origin = config_.get()->base_url();
         // As the tenant's own domain would render it (its name stands in for a missing site title).
         for (const auto& [d, e] : config_.get()->domains) {
           if (e.tenant == *tenant && d.find('.') != std::string::npos) v.origin = "https://" + d;
         }
         v.accept = "text/html";
         v.authorize = [](const bj::value&) { return 200; };
         auto r = site_->visit(v);
         bj::object out{{"status", r.status}, {"kind", site::kind_name(r.kind)}, {"file", r.file},
                        {"content_type", r.content_type}};
         if (!r.location.empty()) out["location"] = r.location;
         if (!r.error.empty()) out["error"] = r.error;
         constexpr size_t kMax = 200 * 1024;
         if (r.kind == site::Kind::kStatic && r.content_type.find("text") == std::string::npos) {
           out["bytes"] = r.body.size();
         } else {
           out["body"] = r.body.size() > kMax ? r.body.substr(0, kMax) + "\n[truncated]" : r.body;
         }
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool(
      {"template_put",
       "Writes a Mustache template, _templates/$name.html, after checking it parses. Start it with front matter "
       "\"---\\nlayout: base\\n---\\n\" to wrap it in another template. Use tenant _host to restyle every tenant.",
       object_schema({{"tenant", string_property("A tenant name, or _host for every tenant.")},
                      {"name", string_property("The template's name, like base, page, list, post, or directory.")},
                      {"template", string_property("The Mustache text.")}},
                     {"tenant", "name", "template"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto name = arg(args, "name");
         auto text = arg(args, "template");
         if (!tenant || !name || !text) return ToolResult::fail("tenant, name, and template are required");
         if (!valid_template_name(*name)) return ToolResult::fail("a template name is lowercase letters, digits, _ and -");
         std::string err;
         auto doc = render::split_front_matter(*text, &err);
         if (!err.empty()) return ToolResult::fail("front matter: " + err);
         if (!render::check_mustache(doc.body, &err)) return ToolResult::fail(err);
         std::string path = "_templates/" + *name + ".html";
         if (!content_->put(*tenant, path, *text, &err)) return ToolResult::fail(err);
         return ToolResult::ok(bj::object{{"tenant", *tenant}, {"file", path}});
       }});

  mcp_->add_tool({"template_list",
                  "Lists a tenant's templates, and where base, page, list, and directory come from: the tenant, "
                  "_host, or the built-ins (directory has none, so directory listings are off until it exists).",
                  object_schema({{"tenant", tenant_prop}}, {"tenant"}), [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    if (!tenant) return ToolResult::fail("tenant is required");
                    bj::array own;
                    for (const auto& f : content_->list(*tenant, "_templates/")) {
                      if (f.ends_with(".html")) own.push_back(bj::value(f.substr(11, f.size() - 16)));
                    }
                    bj::object sources;
                    for (const char* n : {"base", "page", "list", "directory"}) {
                      std::string source;
                      sources[n] = site_->find_template(*tenant, n, &source) ? bj::value(source) : bj::value("none");
                    }
                    return ToolResult::ok(bj::object{{"templates", std::move(own)}, {"defaults", std::move(sources)}});
                  }});

  mcp_->add_tool(
      {"attachment_put",
       "Stores an image or PDF and returns its URL, /attachment/$id.$ext, to use in pages. Give exactly one of: "
       "base64 bytes; file, a file already on the tenant's site (like /hero-small.jpg, from content_list); or an "
       "https url for the host to fetch (a URL on one of this host's own domains is read from its files). The type "
       "comes from the bytes: PNG, JPEG, GIF, WebP, AVIF, ICO, SVG, or PDF. base64 suits small files (under about "
       "100 KB): larger ones can be cut off on their way into a tool call, so give url or file, or upload with "
       "attachment_upload_url. A file that arrives cut off is refused, saying how many bytes arrived. On failure, "
       "nothing is stored: don't put this call's arguments in a page.",
       object_schema({{"tenant", tenant_prop},
                      {"base64", string_property("The file's bytes as base64.")},
                      {"url", string_property("An https URL to fetch the file from.")},
                      {"file", string_property("A file already on the tenant's site (not your machine), like /hero-small.jpg.")},
                      {"name", string_property("A name for people, like truck.jpg.")},
                      {"alt", string_property("What the image shows, for screen readers and the markdown snippet.")},
                      {"visibility", enum_property("Who can fetch it.", {"public", "members"})}},
                     {"tenant"}),
       [this, known_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!tenant) return ToolResult::fail("tenant is required");
         if (!known_tenant(*tenant)) return ToolResult::fail("no domain in the host config serves tenant " + *tenant);
         auto b64 = arg(args, "base64");
         auto url = arg(args, "url");
         auto file = arg(args, "file");
         if (int(bool(b64)) + int(bool(url)) + int(bool(file)) != 1) {
           return ToolResult::fail("give exactly one of base64, file, or url");
         }
         // A URL on this host (one of its domains, or loopback) names one of its own files: read
         // it, rather than fetch through the network what is already here.
         if (url) {
           std::string rest = url->starts_with("https://") ? url->substr(8) : url->starts_with("http://") ? url->substr(7) : "";
           std::string authority = rest.substr(0, rest.find('/'));
           std::string host = base::to_lower(authority.substr(0, authority.rfind(':') == std::string::npos || authority.ends_with("]")
                                                                      ? authority.size()
                                                                      : authority.rfind(':')));
           auto cfg = config_.get();
           auto own = cfg->domains.find(host);
           bool loopback = host == "localhost" || host == "127.0.0.1" || host == "[::1]";
           if (!rest.empty() && (loopback || own != cfg->domains.end())) {
             if (own != cfg->domains.end() && own->second.tenant != *tenant) {
               return ToolResult::fail(host + " is tenant " + own->second.tenant + "'s, not " + *tenant + "'s");
             }
             file = rest.find('/') == std::string::npos ? std::string("/") : rest.substr(rest.find('/'));
             url.reset();
           } else if (!url->starts_with("https://")) {
             return ToolResult::fail("only https URLs can be fetched; for a file already on the site, give file, like "
                                     "\"/hero-small.jpg\"");
           }
         }
         std::string bytes, err;
         std::string name = arg(args, "name").value_or("");
         if (file) {
           std::string path = file->substr(0, file->find_first_of("?#"));
           while (path.starts_with("/")) path.erase(0, 1);
           auto f = content_->get(*tenant, path);
           if (!f) {
             return ToolResult::fail("no file /" + path + " on tenant " + *tenant + "'s site (content_list shows them). "
                                     "file is a path on the site, not on your machine: for a file you hold, use "
                                     "attachment_upload_url (PUT its bytes) or attachment_upload_part.");
           }
           bytes = std::move(f->data);
           if (name.empty()) name = path.substr(path.rfind('/') + 1);
         } else if (b64) {
           // 32 MiB of file is at most this much base64; past it, say so rather than try.
           constexpr size_t kMaxBase64 = (kOperatorAttachmentMaxBytes + 2) / 3 * 4;
           if (b64->size() > kMaxBase64) {
             return ToolResult::fail("base64 exceeds " + std::to_string(kMaxBase64) + " characters (32 MiB of file): " +
                                     std::to_string(b64->size()) + " arrived");
           }
           auto decoded = base::base64_decode(*b64);
           if (!decoded) {
             return ToolResult::fail("base64 is not valid (" + std::to_string(b64->size()) +
                                     " characters arrived; an argument cut off on its way looks like this). For files "
                                     "over about 100 KB, give url or file, or upload with attachment_upload_url.");
           }
           bytes = std::move(*decoded);
         } else {
           auto fetched = fetch_https(*url, size_t(kOperatorAttachmentMaxBytes), &err);
           if (!fetched) return ToolResult::fail(err);
           bytes = std::move(*fetched);
           if (name.empty()) {
             std::string u = url->substr(0, url->find_first_of("?#"));
             name = u.substr(u.rfind('/') + 1);
           }
         }
         // Operators may attach more than a visitor may upload (attachment_routes.cpp).
         if (bytes.size() > kOperatorAttachmentMaxBytes) {
           return ToolResult::fail("larger than " + std::to_string(kOperatorAttachmentMaxBytes >> 20) + " MiB");
         }
         auto a = attachments_->put(*tenant, bytes, name, arg(args, "alt").value_or(""),
                                    arg(args, "visibility").value_or("public"), base::now_unix_ms(), &err);
         if (!a) return ToolResult::fail(err);
         backups_->nudge();  // queued for the bucket at the next backup tick
         auto out = attachment_json(*a, nullptr);
         // What arrived, so an agent can tell its own side cut a long argument short.
         if (b64) out["base64_chars_received"] = int64_t(b64->size());
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool(
      {"attachment_upload_part",
       "Sends a file in pieces, so no one tool call carries it all: get a ticket from attachment_upload_url, then "
       "call this with each piece of the file's base64, in order, and done: true on the last. Each piece but the "
       "last must be a multiple of 4 characters long (split the base64 every 100000 characters, say). The last call "
       "stores the attachment and answers what attachment_put answers. Up to 32 MiB, within the ticket's 15 minutes.",
       object_schema({{"ticket", string_property("The upload_url from attachment_upload_url, or just its ticket.")},
                      {"base64", string_property("The next piece of the file's base64.")},
                      {"done", boolean_property("true on the last piece: store the file.")}},
                     {"ticket", "base64"}),
       [this](const bj::object& args) {
         std::string ticket = arg(args, "ticket").value_or("");
         if (auto at = ticket.find("ticket="); at != std::string::npos) ticket = ticket.substr(at + 7);
         ticket = ticket.substr(0, ticket.find_first_of("&# "));
         std::string piece = arg(args, "base64").value_or("");
         bool done = false;
         if (const auto* d = args.if_contains("done"); d && d->is_bool()) done = d->as_bool();
         if (!done && piece.size() % 4 != 0) {
           return ToolResult::fail("a piece's base64 must be a multiple of 4 characters long, but the last; this one is " +
                                   std::to_string(piece.size()));
         }
         auto bytes = base::base64_decode(piece);
         if (!bytes) {
           return ToolResult::fail("this piece's base64 is not valid (" + std::to_string(piece.size()) +
                                   " characters arrived); send it again");
         }
         UploadTicket t;
         {
           std::lock_guard lock(upload_tickets_mu_);
           auto it = upload_tickets_.find(base::sha256_hex(ticket));
           if (it == upload_tickets_.end() || it->second.expires_ms <= base::now_unix_ms()) {
             return ToolResult::fail("this ticket is unknown, used, or expired; ask attachment_upload_url for a new one");
           }
           if (it->second.parts.size() + bytes->size() > kOperatorAttachmentMaxBytes) {
             upload_part_bytes_ -= it->second.parts.size();
             upload_tickets_.erase(it);
             return ToolResult::fail("the file passed 32 MiB; the ticket is gone");
           }
           if (upload_part_bytes_ + bytes->size() > (64ull << 20)) {
             return ToolResult::fail("the host is holding 64 MiB of pieces already; finish or abandon other uploads first");
           }
           it->second.parts += *bytes;
           it->second.part_count += 1;
           upload_part_bytes_ += bytes->size();
           if (!done) {
             return ToolResult::ok(bj::object{{"received_bytes", int64_t(it->second.parts.size())},
                                              {"pieces", int64_t(it->second.part_count)},
                                              {"next", "send the next piece, or done: true with the last"}});
           }
           t = std::move(it->second);
           upload_part_bytes_ -= t.parts.size();
           upload_tickets_.erase(it);
         }
         std::string err;
         auto a = attachments_->put(t.tenant, t.parts, t.name, t.alt, t.visibility, base::now_unix_ms(), &err);
         if (!a) return ToolResult::fail(err + " (" + std::to_string(t.part_count) + " pieces)");
         backups_->nudge();
         auto out = attachment_json(*a, nullptr);
         out["pieces"] = int64_t(t.part_count);
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool(
      {"attachment_upload_url",
       "A one-time URL to upload one image or PDF (up to 32 MiB) with a plain HTTP PUT of its bytes, for files too "
       "big to pass as base64 in a tool call (over about 100 KB). From code or a shell: curl -T photo.jpg '<url>'. "
       "Without code, send the base64 in pieces with attachment_upload_part and this ticket. "
       "It works once, for 15 minutes, for this tenant; the upload answers with the attachment's url, as "
       "attachment_put does.",
       object_schema({{"tenant", tenant_prop},
                      {"name", string_property("A name for people, like truck.jpg.")},
                      {"alt", string_property("What the image shows, for screen readers and the markdown snippet.")},
                      {"visibility", enum_property("Who can fetch it.", {"public", "members"})}},
                     {"tenant"}),
       [this, known_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!tenant || !known_tenant(*tenant)) return ToolResult::fail("no domain in the host config serves that tenant");
         std::string visibility = arg(args, "visibility").value_or("public");
         if (visibility != "public" && visibility != "members") return ToolResult::fail("visibility is public or members");
         auto cfg = config_.get();
         // The tenant's own domain: the platform domain if it is the tenant's, else its first real name.
         std::string domain;
         for (const auto& [d, e] : cfg->domains) {
           if (e.tenant != *tenant) continue;
           bool real = d.find('.') != std::string::npos && d.find_first_not_of("0123456789.") != std::string::npos;
           if (d == cfg->platform_domain || (domain.empty() && real)) domain = d;
           if (domain.empty()) domain = d;
         }
         bool secure = cfg->identity.cookie_secure && !cfg->local;
         std::string origin = (secure ? "https://" : "http://") + domain +
                              (secure || cfg->listen_port == 80 ? "" : ":" + std::to_string(cfg->listen_port));
         int64_t expires = base::now_unix_ms() + 15 * 60 * 1000;
         std::string ticket = issue_upload_ticket(UploadTicket{*tenant, arg(args, "name").value_or(""), arg(args, "alt").value_or(""),
                                                               visibility, current_operator(), expires});
         std::string url = origin + "/attachment/upload?ticket=" + ticket;
         return ToolResult::ok(bj::object{{"upload_url", url},
                                          {"method", "PUT"},
                                          {"max_bytes", int64_t(kOperatorAttachmentMaxBytes)},
                                          {"expires_at", base::format_utc_ms(expires)},
                                          {"example", "curl -sS -T photo.jpg '" + url + "'"}});
       }});

  mcp_->add_tool({"attachment_list",
                  "Lists a tenant's attachments, newest first, with the content files that use each one.",
                  object_schema({{"tenant", tenant_prop}, {"unused", boolean_property("Only those nothing uses.")}},
                                {"tenant"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    if (!tenant) return ToolResult::fail("tenant is required");
                    auto refs = attachments::references(*content_, *tenant);
                    bool unused = bool_arg(args, "unused");
                    bj::array out;
                    for (const auto& a : attachments_->list(*tenant)) {
                      auto it = refs.find(a.id);
                      if (unused && it != refs.end()) continue;
                      out.push_back(attachment_json(a, it == refs.end() ? nullptr : &it->second));
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"attachment_get", "Describes one attachment, with the content files that use it.",
                  object_schema({{"tenant", tenant_prop}, {"id", mcp::integer_property("The attachment's id.")}},
                                {"tenant", "id"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto it = args.find("id");
                    if (!tenant || it == args.end() || !it->value().is_int64()) return ToolResult::fail("tenant and id are required");
                    auto a = attachments_->get(*tenant, it->value().get_int64());
                    if (!a) return ToolResult::fail("no such attachment");
                    auto refs = attachments::references(*content_, *tenant);
                    auto used = refs.find(a->id);
                    return ToolResult::ok(attachment_json(*a, used == refs.end() ? nullptr : &used->second));
                  }});

  mcp_->add_tool({"attachment_delete",
                  "Deletes an attachment. One that content still uses needs force, and those pages will show a "
                  "broken image. Its id is never reused.",
                  object_schema({{"tenant", tenant_prop},
                                 {"id", mcp::integer_property("The attachment's id.")},
                                 {"force", boolean_property("Delete even though content uses it.")}},
                                {"tenant", "id"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto it = args.find("id");
                    if (!tenant || it == args.end() || !it->value().is_int64()) return ToolResult::fail("tenant and id are required");
                    int64_t id = it->value().get_int64();
                    auto refs = attachments::references(*content_, *tenant);
                    if (auto used = refs.find(id); used != refs.end() && !bool_arg(args, "force")) {
                      std::string list;
                      for (const auto& p : used->second) list += (list.empty() ? "" : ", ") + p;
                      return ToolResult::fail("attachment " + std::to_string(id) + " is used by " + list +
                                              "; pass force to delete it anyway");
                    }
                    std::string err;
                    if (!attachments_->remove(*tenant, id, base::now_unix_ms(), &err)) return ToolResult::fail(err);
                    return ToolResult::ok(bj::object{{"deleted", id}});
                  }});
}

}  // namespace goober::host
