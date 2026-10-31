// The operator tools for building sites (design/machine/007.Content.md §9): the guide,
// pages, templates, and attachments.

#include <fcntl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <tuple>

#include "backups.h"
#include "goober/base/encoding.h"
#include "goober/base/files.h"
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
#include "network.h"


namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

constexpr const char* kGuide = R"guide(# Building a site on goober

Each tenant has a content tree. Everything below is written with the tools named in brackets,
persisted to disk through the host WAL, and live at once. tenant may be left out of any tool
when the host serves one tenant (or you're focused on one).

## A blog in three calls (posts, an index, a feed, the sitemap)
1. _data/site.json once: content_put {path: "_data/site.json", text: "{\"title\": \"Example Co\",
   \"description\": \"...\", \"author\": \"Jeffrey M. Barber\"}"}.
2. The index once: page_put {path: "/updates", format: "markdown", title: "Updates", body: "News.",
   fields: {list: "updates", "list-template": "post", "per-page": 10}}.
3. Each post: page_put {path: "/updates/<slug>", format: "markdown", template: "post", title,
   body, fields: {date: "2026-10-29", summary: "...", author?: "...", tags: ["beef"], image?: "/hero.jpg",
   draft?: true}}.
The built-in post template shows the title, the byline (author, else site.author), the date, the
image, and the tags, and the host builds one BlogPosting JSON-LD from the front matter (unless the
page has its own json_ld). Every page gets a meta description (summary), a canonical link, and
Open Graph and Twitter tags from its front matter. The post is then at the top of /updates
(newest first, drafts hidden, 10 a page with ?page=2, rel prev/next, and canonical URLs per page),
in /updates/feed.xml and /updates/rss.xml (with its author), in /sitemap.xml (with lastmod: the updated field, else
date), and in /llms.txt. Preview a draft with page_render {path, drafts: true}. To restyle, copy
template_get post (or list, base) and template_put your own under the same name.
Moving hand-written HTML documents to posts: page_put each as markdown (or fragment, for HTML
you keep) with template: "post" and its title, date, and summary as fields; page_put replaces the
document at that URL. Then make /updates the listing above and content_delete a hand-kept
updates/feed.xml (the listing serves the feed).

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
Preview anything, drafts included, with page_render before telling a person it is done: it
answers status, hashes, and the deploy checks; add verbose: true for the HTML. page_get answers
the page's etag; give it to page_put as if_etag ("none" for a new page) and the write is refused
if someone changed the page since. list_etags gives many at once.
Try before writing: page_render also takes unsaved pages (each shaped like page_put's
arguments), templates {name: text}, partials {name: html}, and files {path: text}, and renders
as if they were written; nothing is stored. Add feed: true to get a listing's Atom too:
  page_render {tenant, path: "/updates", pages: [{path: "/updates/hello", format: "markdown",
    body: "...", fields: {date: "2026-10-29", tags: ["news"]}}], templates: {post: "..."}, feed: true}
Structured data: a json_ld field (a JSON object or array) becomes the host-built
<script type="application/ld+json"> tag, escaped so no value can end it; put {{{json_ld}}}
in the template's <head>, without a <script> of its own. {{title}} inside a script would be
HTML-escaped (I&#39;m), so build JSON-LD this way instead.

## Templates [template_put, template_get, template_list]
Mustache, stored as _templates/$name.html. A page picks one with the template field; the
default is "page", or "list" for a listing. Built-in base, page, and list templates exist, so
a site looks clean with no templates at all; write your own to restyle. template_get gives any
template's source, the built-ins too, to copy from.
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
What it puts between <!--head--> and <!--/head--> goes to base's <head> instead ({{{head}}}).
Inside a <script>, {{{json.title}}} is the value as JSON, safe there; {{title}} is HTML-escaped.
template_delete removes one. A template is live at once for every page naming it: try changes
with page_render's templates (unsaved), or under a new name on a draft page.
So a typical site has base (the <html>, header, nav, footer), page, and list.

## Listings
A page with the field list: blog lists the pages directly inside blog/ (blog/x.md, and
blog/y/index.md as /blog/y). Drafts and pages the visitor can't open are left out.
"list-template": "post" keeps only the pages written with that template.
  sort: -date (default), date, title, -title, order, path      per-page: 20 (at most 200)
  ?page=2 pages through; ?tag=news filters. Every listing serves /blog/feed.xml (Atom) and
  /blog/rss.xml (RSS 2.0, for RSS-to-email and automation tools), advertised in the base
  template's <head>, unless its front matter says feed: false.
Every format is listed: markdown, fragments, and documents with front matter alike (a
document's title comes from its title field, else its <title>). The feed holds the newest 20
by the listing's sort: each entry's title, link, id, updated (its date, else the feed's), summary (the
summary field, else the start of the text), its author (the author field, else site.author),
and a category per tag; the feed's author is the site's title. It carries no full content.
A directory without an index page is listed only if _templates/directory.html exists.

## Deploying many files, and checking what visitors get [content_deploy, url_check, page_lint]
content_deploy {pages: [page_put's arguments + sha256? + if_etag?], files: [{path, text | base64,
sha256?, if_etag?}]} checks every item first (nothing is written if one fails), writes them all,
then fetches each URL signed out and answers one line each: status, type, etag, ld+json count,
and any problem (unrendered {{, more than one or an unparseable ld+json, empty <p>, http: links,
raw markdown links; strict: true adds title 50-60 and description 120-160 characters and exactly
one ld+json). url_check does the fetching for any of this host's URLs; page_lint runs the
checks over every listed page. The host serves /sitemap.xml and /llms.txt from the pages' front
matter unless a file or a box route claims them; a page with noindex: true or sitemap: false is
left out. A box's own sitemap joins by naming it in _data/site.json: "sitemaps":
["/places/sitemap.xml"] makes /sitemap.xml an index of it and /sitemap-pages.xml.

## Site data [content_put]
_data/site.json holds the site's title, navigation, and footer:
  {"title": "Bob's Plumbing", "nav": [{"label": "Blog", "url": "/blog"}], "footer": "Since 1987",
   "stylesheet": "/css/site.css"}
Static files (CSS, JS, fonts, favicons, CSV) go anywhere with content_put; the type comes from
the extension (.csv, .json, .xml, .atom, .rss, .webmanifest, .ics, .svg, .txt, .md, images, fonts).
Paths with a segment that starts with "_" are never served. Text is stored byte for byte, but a
tool-call layer can drop CR bytes on the way: give sha256 to make sure, or send base64.

## Installable apps [pwa_put, pwa_get, pwa_list, pwa_delete]
pwa_put {tenant, name, app: {scope: "/" or "/shop/", manifest: {name, short_name, icons,
theme_color, ...}, offline: {fallback, precache}, push}} makes that part of the site an app:
manifest, offline worker, and push, served by the host. Pages in scope get the tags through
<!--#include pwa-->, which the built-in base template already has; add it to your own
layout's <head>. Icons need 192x192 and 512x512 PNGs for browsers to offer install.
box_guide's "Installable apps" section has push.

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

## Boxes with templates [box_render]
A box route may name a template; browsers then get the box's JSON rendered through it, while
API callers get the JSON. A box template takes front matter like any template here:
"---\nlayout: base\n---\n<h1>{{title}}</h1>" puts the box's page in this site's base, header,
nav, and footer included. The box's JSON fills the layout too ("title", "summary", "noindex":
true), and <!--head-->...<!--/head--> adds to <head>. A request the route's requirements refuse
gets its 401 or sign-in redirect before anything renders. page_render renders pages, not box
routes: for a box route it names the route. box_render previews one with sample JSON and
unsaved templates (a new base too), without running the box; version_upload {dry_run: true}
checks a manifest without storing it.
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

// What page_put writes for its arguments (path, format, body, title, template, fields): the
// file, its text with front matter, and the other files answering the same URL, which the
// write removes. page_render previews the same thing without writing it.
struct BuiltPage {
  std::string file, text;
  std::vector<std::string> others;
  bj::object fields;
};
std::optional<BuiltPage> build_page(const site::Site& site, const std::string& tenant, const bj::object& args,
                                    std::string* error) {
  auto text_of = [&](const char* k) -> std::optional<std::string> {
    auto* v = args.if_contains(k);
    return v && v->is_string() ? std::optional<std::string>(std::string(v->as_string())) : std::nullopt;
  };
  auto path = text_of("path"), format = text_of("format"), body = text_of("body");
  if (!path || !format || !body) return *error = "path, format, and body are required", std::nullopt;
  if (*format != "markdown" && *format != "fragment" && *format != "document") {
    return *error = "format must be markdown, fragment, or document", std::nullopt;
  }
  BuiltPage out;
  bj::object& fields = out.fields;
  if (auto* f = args.if_contains("fields")) {
    if (!f->is_object()) return *error = "fields must be an object", std::nullopt;
    fields = f->as_object();
  }
  if (auto t = text_of("title")) fields["title"] = *t;
  if (auto t = text_of("template")) fields["template"] = *t;
  if (*format == "document") {
    // A document is served as written; front matter (document: true) carries only what
    // decides who sees it and how it's listed.
    for (const auto& [k, v] : fields) {
      if (k == "template" || k == "list" || k == "sort" || k == "per-page" || k == "feed") {
        return *error = "a document is served as written, so it takes no " + std::string(k) +
                        "; use fragment to wrap HTML in a template",
               std::nullopt;
      }
    }
    if (body->starts_with("---")) return *error = "a document can't start with ---, which would read as front matter", std::nullopt;
    if (!fields.empty()) fields["document"] = true;
  }
  std::string ext = *format == "markdown" ? ".md" : ".html";
  out.file = file_for(site, tenant, *path, ext);
  if (site::is_private_path(out.file)) return *error = "a page's path can't have a segment starting with _", std::nullopt;
  if (*format != "document" || !fields.empty()) {
    out.text = "---\n";
    for (const auto& [k, v] : fields) {
      std::string key(k);
      bool ok = !key.empty();
      for (char c : key) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
      if (!ok) return *error = "field names are letters, digits, _ and -: " + key, std::nullopt;
      out.text += front_matter_line(key, v);
    }
    out.text += "---\n";
  }
  out.text += *body;
  if (auto url = site::page_url(out.file)) {
    std::string stem = url->substr(1);
    std::vector<std::string> others = stem.empty() ? std::vector<std::string>{"index.md", "index.html"}
                                                   : std::vector<std::string>{stem + ".md", stem + ".html",
                                                                              stem + "/index.md", stem + "/index.html"};
    for (auto& o : others) {
      if (o != out.file) out.others.push_back(std::move(o));
    }
  }
  return out;
}

// Fetches an https URL with the system curl, for attachment_put (007 §7).
std::optional<std::string> fetch_https(const std::string& url, size_t max_bytes, std::string* error) {
  if (!url.starts_with("https://")) return *error = "only https URLs can be fetched", std::nullopt;
  if (outbound_sandbox()) return *error = "this host is a sandbox: nothing is fetched from outside it", std::nullopt;
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

namespace {

// What a served HTML page holds that a deploy should check.
struct Inspection {
  int ld_json = 0, ld_json_bad = 0, mustache = 0, empty_p = 0, http_links = 0, markdown_links = 0;
  std::string title, description;
  bj::object to_json() const {
    return bj::object{{"ld_json", ld_json}, {"ld_json_unparseable", ld_json_bad}, {"mustache", mustache},
                      {"empty_p", empty_p}, {"http_links", http_links}, {"markdown_links", markdown_links},
                      {"title", title}, {"description_chars", int64_t(description.size())}};
  }
};

std::string html_unescape_attr(std::string s) {
  for (auto [from, to] : {std::pair{"&quot;", "\""}, std::pair{"&#39;", "'"}, std::pair{"&lt;", "<"},
                          std::pair{"&gt;", ">"}, std::pair{"&amp;", "&"}}) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + 1)) s.replace(at, std::strlen(from), to);
  }
  return s;
}

Inspection inspect_html(const std::string& html) {
  Inspection in;
  std::string lower = base::to_lower(html);
  for (size_t at = lower.find("<script type=\"application/ld+json\""); at != std::string::npos;
       at = lower.find("<script type=\"application/ld+json\"", at + 1)) {
    ++in.ld_json;
    size_t open = html.find('>', at), close = lower.find("</script>", at);
    std::string err;
    if (open == std::string::npos || close == std::string::npos || !json::parse(html.substr(open + 1, close - open - 1), &err)) {
      ++in.ld_json_bad;
    }
  }
  for (size_t at = html.find("{{"); at != std::string::npos; at = html.find("{{", at + 2)) ++in.mustache;
  for (size_t at = lower.find("<p"); at != std::string::npos; at = lower.find("<p", at + 2)) {
    size_t gt = lower.find('>', at);
    if (gt == std::string::npos || (lower[at + 2] != '>' && lower[at + 2] != ' ')) continue;
    size_t i = gt + 1;
    while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
    if (lower.compare(i, 4, "</p>") == 0) ++in.empty_p;
  }
  for (const char* attr : {"href=\"http:", "src=\"http:"}) {
    for (size_t at = lower.find(attr); at != std::string::npos; at = lower.find(attr, at + 1)) {
      // A plain-HTTP host on this machine (local mode) links to itself over http: that's not the problem.
      size_t host = at + std::strlen(attr) + 2;
      if (lower.compare(host, 9, "localhost") == 0 || lower.compare(host, 9, "127.0.0.1") == 0) continue;
      ++in.http_links;
    }
  }
  for (const char* md : {"](http", "](/"}) {
    for (size_t at = html.find(md); at != std::string::npos; at = html.find(md, at + 1)) ++in.markdown_links;
  }
  if (size_t t = lower.find("<title>"); t != std::string::npos) {
    size_t e = lower.find("</title>", t);
    if (e != std::string::npos) in.title = html_unescape_attr(html.substr(t + 7, e - t - 7));
  }
  if (size_t d = lower.find("<meta name=\"description\" content=\""); d != std::string::npos) {
    size_t s = d + 34, e = html.find('"', s);
    if (e != std::string::npos) in.description = html_unescape_attr(html.substr(s, e - s));
  }
  return in;
}

// The problems a deploy must not ship (always), and, when strict, what search wants too.
bj::array problems_of(const Inspection& in, bool html, bool strict) {
  bj::array out;
  if (!html) return out;
  if (in.mustache) out.push_back(bj::value(std::to_string(in.mustache) + " unrendered {{"));
  if (in.ld_json_bad) out.push_back(bj::value(std::to_string(in.ld_json_bad) + " ld+json that doesn't parse"));
  if (in.ld_json > 1) out.push_back(bj::value(std::to_string(in.ld_json) + " ld+json tags (one, please)"));
  if (in.empty_p) out.push_back(bj::value(std::to_string(in.empty_p) + " empty <p>"));
  if (in.http_links) out.push_back(bj::value(std::to_string(in.http_links) + " http: links"));
  if (in.markdown_links) out.push_back(bj::value(std::to_string(in.markdown_links) + " raw markdown links"));
  if (strict) {
    if (in.ld_json == 0) out.push_back(bj::value("no ld+json"));
    size_t t = in.title.size(), d = in.description.size();
    if (t < 50 || t > 60) out.push_back(bj::value("title is " + std::to_string(t) + " characters (50 to 60)"));
    if (d < 120 || d > 160) out.push_back(bj::value("description is " + std::to_string(d) + " characters (120 to 160)"));
  }
  return out;
}

// The content file answering a URL or naming itself, like page_get resolves it.
std::optional<std::string> answering_file(const content::ContentStore& store, const std::string& tenant, const std::string& path) {
  std::string rel = path;
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
  if (!rel.empty() && store.get(tenant, rel)) return rel;
  while (!rel.empty() && rel.back() == '/') rel.pop_back();
  std::vector<std::string> candidates = rel.empty() ? std::vector<std::string>{"index.md", "index.html"}
                                                    : std::vector<std::string>{rel + ".md", rel + ".html", rel + "/index.md",
                                                                               rel + "/index.html"};
  for (const auto& c : candidates) {
    if (store.get(tenant, c)) return c;
  }
  return std::nullopt;
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
                                                 "[\"news\"]} or {\"list\": \"blog\"} for a listing.")},
                      {"if_etag", string_property("Write only if the page's current etag (page_get, list_etags) is "
                                                  "this; \"none\" writes only a new page.")}},
                     {"tenant", "path", "format", "body"}),
       [this, known_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto path = arg(args, "path");
         auto format = arg(args, "format");
         auto body = arg(args, "body");
         if (!tenant || !path || !format || !body) return ToolResult::fail("tenant, path, format, and body are required");
         if (!known_tenant(*tenant)) return ToolResult::fail("no domain in the host config serves tenant " + *tenant);
         std::string err;
         auto built = build_page(*site_, *tenant, args, &err);
         if (!built) return ToolResult::fail(err);
         const std::string& file = built->file;
         bj::object& fields = built->fields;
         if (auto want = arg(args, "if_etag")) {
           std::string current = "none";
           for (const auto& f : std::vector<std::string>{file}) {
             if (auto got = content_->get(*tenant, f)) current = got->etag;
           }
           if (current == "none") {
             for (const auto& o : built->others) {
               if (auto got = content_->get(*tenant, o)) {
                 current = got->etag;
                 break;
               }
             }
           }
           if (*want != current && "\"" + *want + "\"" != current) {
             return ToolResult::fail("nothing written: if_etag is " + *want + " but the page's etag is " + current +
                                     " (someone wrote it since you read it)");
           }
         }
         if (!content_->put(*tenant, file, built->text, &err)) return ToolResult::fail(err);
         // One page per URL: drop every other file that answers it (the other format, and
         // name.html beside name/index.html), so what was just written is what's served.
         bj::array removed;
         for (const auto& other : built->others) {
           if (content_->get(*tenant, other) && content_->remove(*tenant, other, &err)) removed.push_back(bj::value(other));
         }
         bj::array warnings;
         std::string tmpl = fields.contains("template") && fields["template"].is_string()
                                ? std::string(fields["template"].get_string())
                                : (fields.contains("list") ? "list" : "page");
         if (*format != "document" && !site_->find_template(*tenant, tmpl)) {
           warnings.push_back(bj::value("no template named " + tmpl + ": write it with template_put"));
         }
         auto url = site::page_url(file);
         auto written = content_->get(*tenant, file);
         return ToolResult::ok(bj::object{{"tenant", *tenant}, {"file", file}, {"url", url ? *url : ""},
                                          {"etag", written ? written->etag : ""},
                                          {"replaced", std::move(removed)}, {"warnings", std::move(warnings)}});
       }});

  mcp_->add_tool(
      {"page_get", "Reads the page at a URL (or a file path like blog/hello.md): its file, format, etag (give it to "
                   "page_put as if_etag), front matter, and body.",
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
         auto f = content_->get(*tenant, p->path);
         return ToolResult::ok(bj::object{{"file", p->path}, {"url", p->url}, {"kind", site::kind_name(p->kind)},
                                          {"etag", f ? f->etag : ""}, {"fields", p->fields}, {"body", p->body}});
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
                  },
                  {{"path", "dir"}, {"prefix", "dir"}}});

  mcp_->add_tool(
      {"page_render",
       "Renders a URL exactly as a visitor gets it (status, kind, and HTML), as a host operator, so pages with "
       "requirements render too. With drafts, drafts render; use it to preview before telling someone it's done. "
       "To try something without writing it, pass unsaved pages (each shaped like page_put's arguments), templates, "
       "partials, or files: the render sees them as if written, and nothing is stored. With feed, a listing's "
       "/feed.xml is rendered too.",
       object_schema({{"tenant", tenant_prop},
                      {"path", string_property("The URL, like /blog or /blog/hello.")},
                      {"query", object_property("Query parameters, like {\"page\": \"2\"}.")},
                      {"drafts", boolean_property("Render drafts too.")},
                      {"pages", bj::object{{"type", "array"},
                                           {"description", "Unsaved pages: [{path, format, body, title?, template?, fields?}], "
                                                           "as page_put takes them."},
                                           {"items", bj::object{{"type", "object"}}}}},
                      {"templates", object_property("Unsaved templates by name, like {\"post\": \"<article>{{{body}}}</article>\"}.")},
                      {"partials", object_property("Unsaved partials by name, like {\"nav\": \"<nav>...</nav>\"}.")},
                      {"files", object_property("Other unsaved files by path, like {\"_data/site.json\": \"{...}\"}.")},
                      {"feed", boolean_property("Also render the listing's feed, path/feed.xml.")},
                      {"verbose", boolean_property("Include the HTML (and the feed's XML). Without it: status, hashes, and "
                                                   "the checks a deploy makes.")}},
                     {"tenant", "path"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto path = arg(args, "path");
         if (!tenant || !path) return ToolResult::fail("tenant and path are required");
         // The would-be files, seen only by this render (content::ContentStore::Preview).
         content::Overlay overlay{*tenant, {}};
         bj::array previewed;
         auto add = [&](const std::string& file, std::optional<std::string> text) {
           overlay.files[file] = std::move(text);
         };
         if (auto* pages = args.if_contains("pages")) {
           if (!pages->is_array()) return ToolResult::fail("pages must be an array of page_put arguments");
           for (const auto& pv : pages->as_array()) {
             if (!pv.is_object()) return ToolResult::fail("each page is an object of page_put arguments");
             std::string err;
             auto built = build_page(*site_, *tenant, pv.as_object(), &err);
             if (!built) return ToolResult::fail("pages: " + err);
             for (const auto& o : built->others) add(o, std::nullopt);
             add(built->file, built->text);
             previewed.push_back(bj::value(built->file));
           }
         }
         for (auto [key, dir, ext] : {std::tuple{"templates", "_templates/", ".html"}, std::tuple{"partials", "_partials/", ".html"},
                                      std::tuple{"files", "", ""}}) {
           auto* m = args.if_contains(key);
           if (!m) continue;
           if (!m->is_object()) return ToolResult::fail(std::string(key) + " must be an object of names to text");
           for (const auto& [name, text] : m->as_object()) {
             if (!text.is_string()) return ToolResult::fail(std::string(key) + "." + std::string(name) + " must be text");
             std::string t(text.as_string());
             if (std::string_view(key) == "templates") {
               if (!valid_template_name(std::string(name))) return ToolResult::fail("a template name is lowercase letters, digits, _ and -");
               std::string err;
               auto doc = render::split_front_matter(t, &err);
               if (!err.empty() || !render::check_mustache(doc.body, &err)) {
                 return ToolResult::fail("templates." + std::string(name) + ": " + err);
               }
             }
             std::string file = std::string(dir) + std::string(name) + ext;
             if (!base::is_safe_relative_path(file)) return ToolResult::fail(std::string(key) + ": not a safe path: " + file);
             add(file, t);
             previewed.push_back(bj::value(file));
           }
         }
         content::ContentStore::Preview preview(overlay.files.empty() ? nullptr : &overlay);
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
         if (!previewed.empty()) {
           out["previewed"] = std::move(previewed);
           out["stored"] = false;
         }
         if (bool_arg(args, "feed")) {
           site::Visit fv = v;
           fv.path = (v.path == "/" ? "" : v.path) + "/feed.xml";
           fv.query.clear();
           auto f = site_->visit(fv);
           bj::object fo{{"status", f.status}, {"content_type", f.content_type}, {"sha256", base::sha256_hex(f.body)},
                         {"bytes", int64_t(f.body.size())}};
           if (bool_arg(args, "verbose") || f.status != 200) fo["body"] = f.status == 200 ? f.body : f.error;
           out["feed"] = std::move(fo);
         }
         if (!r.location.empty()) out["location"] = r.location;
         if (!r.error.empty()) out["error"] = r.error;
         // Pages are what page_render renders; a box route answering the path is named, with
         // box_render to preview it (a site's engineers, 2026-10-30).
         if (r.status == 404) {
           auto active = boxes_->active();
           for (const auto& [d, e] : config_.get()->domains) {
             if (e.tenant != *tenant) continue;
             auto m = active->table_for(d).match(normal->segments, "get");
             if (!m.value) continue;
             out["box_route"] = bj::object{{"box", m.value->box->manifest.box}, {"route", m.value->route->name},
                                           {"template", m.value->route->template_name}};
             out["note"] = "a box route answers this path, not a page: preview it with box_render";
             break;
           }
         }
         constexpr size_t kMax = 200 * 1024;
         out["bytes"] = r.body.size();
         out["sha256"] = base::sha256_hex(r.body);
         out["etag"] = r.etag;
         bool html = r.content_type.find("text/html") != std::string::npos;
         if (html) {
           auto in = inspect_html(r.body);
           out["checks"] = in.to_json();
           out["problems"] = problems_of(in, true, false);
         }
         // Compact unless asked (031 § 3): the HTML is behind verbose.
         if (bool_arg(args, "verbose") && !(r.kind == site::Kind::kStatic && r.content_type.find("text") == std::string::npos)) {
           out["body"] = r.body.size() > kMax ? r.body.substr(0, kMax) + "\n[truncated]" : r.body;
         }
         return ToolResult::ok(std::move(out));
       }});

  // A box page through its template and the tenant's layout, without running the box or
  // storing anything (a site's engineers, 2026-10-30): the data stands in for the box's JSON.
  mcp_->add_tool(
      {"box_render",
       "Previews a box route's page: its template, the layout the template's front matter names (layout: base), "
       "and shared partials, over JSON you give in place of the box's answer. The box doesn't run and nothing is "
       "stored. There is no request, so the route's gate (public, requirements) isn't applied: a people-only route "
       "renders whatever data you pass, user or not; the real route answers 401 or the sign-in redirect before any "
       "template. Name the route by path (a GET route with a template, on the tenant's domain) or by box and template. "
       "Unsaved templates, the box's or the site's (like base), are used first, so a template change or a new "
       "layout can be tried before version_upload or template_put.",
       object_schema({{"tenant", tenant_prop},
                      {"path", string_property("A URL a box route with a template answers, like /account.")},
                      {"domain", string_property("The domain for path; default the tenant's first public domain.")},
                      {"box", string_property("The box, with template, instead of path.")},
                      {"template", string_property("The template's name, with box.")},
                      {"data", object_property("The JSON object the box would answer. \"user\" is the signed-in person "
                                               "(omit for signed out), as the host would set it.")},
                      {"templates", object_property("Unsaved templates by name, found before the box's and the site's.")},
                      {"partials", object_property("Unsaved partials by name, like {\"nav\": \"<nav>...</nav>\"}.")},
                      {"verbose", boolean_property("Include the HTML.")}},
                     {"tenant", "data"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto* data = args.if_contains("data");
         if (!tenant || !data || !data->is_object()) return ToolResult::fail("tenant and data (an object) are required");
         auto cfg = config_.get();
         auto active = boxes_->active();
         std::string box_name = arg(args, "box").value_or(""), template_name = arg(args, "template").value_or("");
         std::string url_path = "/";
         std::shared_ptr<const boxes::ActiveBox> box;
         bj::object route_out;
         if (auto path = arg(args, "path")) {
           std::string domain = arg(args, "domain").value_or("");
           for (const auto& [d, e] : cfg->domains) {
             if (domain.empty() && e.tenant == *tenant && d.find('.') != std::string::npos) domain = d;
           }
           for (const auto& [d, e] : cfg->domains) {
             if (domain.empty() && e.tenant == *tenant) domain = d;
           }
           auto entry = cfg->domains.find(domain);
           if (entry == cfg->domains.end() || entry->second.tenant != *tenant) {
             return ToolResult::fail("no domain " + domain + " of tenant " + *tenant);
           }
           auto normal = router::normalize_path(path->substr(0, path->find_first_of("?#")));
           if (!normal) return ToolResult::fail("not a valid URL path");
           auto m = active->table_for(domain).match(normal->segments, "get");
           if (!m.value) return ToolResult::fail("no box route answers GET " + normal->path + " on " + domain);
           if (m.value->route->template_name.empty()) {
             return ToolResult::fail("route " + m.value->route->name + " of box " + m.value->box->manifest.box +
                                     " has no template: its box answers the HTML itself");
           }
           box = m.value->box;
           template_name = m.value->route->template_name;
           url_path = normal->path;
           route_out = bj::object{{"box", box->manifest.box}, {"version", box->version}, {"route", m.value->route->name},
                                  {"template", template_name}, {"public", m.value->route->is_public}};
         } else {
           if (box_name.empty() || template_name.empty()) return ToolResult::fail("give path, or box and template");
           auto it = active->boxes.find(box_name);
           if (it == active->boxes.end()) return ToolResult::fail("no active box named " + box_name);
           box = it->second;
           route_out = bj::object{{"box", box_name}, {"version", box->version}, {"template", template_name}};
         }
         // The box's templates, then the unsaved ones over them; the site's are found after both.
         std::map<std::string, std::string> inline_templates = box->manifest.templates;
         bj::array previewed;
         if (auto* t = args.if_contains("templates")) {
           if (!t->is_object()) return ToolResult::fail("templates must be an object of names to text");
           for (const auto& [name, text] : t->as_object()) {
             if (!text.is_string()) return ToolResult::fail("templates." + std::string(name) + " must be text");
             if (!valid_template_name(std::string(name))) return ToolResult::fail("a template name is lowercase letters, digits, _ and -");
             std::string err;
             auto doc = render::split_front_matter(std::string(text.as_string()), &err);
             if (!err.empty() || !render::check_mustache(doc.body, &err)) {
               return ToolResult::fail("templates." + std::string(name) + ": " + err);
             }
             inline_templates[std::string(name)] = std::string(text.as_string());
             previewed.push_back(bj::value("template " + std::string(name)));
           }
         }
         content::Overlay overlay{*tenant, {}};
         if (auto* pm = args.if_contains("partials")) {
           if (!pm->is_object()) return ToolResult::fail("partials must be an object of names to text");
           for (const auto& [name, text] : pm->as_object()) {
             if (!text.is_string() || !site::is_partial_name(std::string(name))) {
               return ToolResult::fail("partials: each is a name ([a-z0-9_-]) and its text");
             }
             overlay.files["_partials/" + std::string(name) + ".html"] = std::string(text.as_string());
             previewed.push_back(bj::value("partial " + std::string(name)));
           }
         }
         content::ContentStore::Preview preview(overlay.files.empty() ? nullptr : &overlay);
         bj::object d = data->as_object();
         bool signed_in = d.contains("user") && !d.at("user").is_null();
         if (!d.contains("user")) d["user"] = nullptr;
         bj::object query_obj;
         d["request"] = bj::object{{"path", url_path}, {"query", std::move(query_obj)}};
         std::string err;
         auto html = site_->render_named(*tenant, template_name, std::move(d), &inline_templates, &err);
         bj::object out{{"route", std::move(route_out)}, {"stored", false}};
         if (!previewed.empty()) out["previewed"] = std::move(previewed);
         if (!html) {
           out["status"] = 500;
           out["error"] = err;
           return ToolResult::ok(std::move(out));
         }
         auto e = site::expand_includes(*content_, *tenant, *html, url_path, signed_in);
         out["status"] = 200;
         out["bytes"] = e.html.size();
         out["sha256"] = base::sha256_hex(e.html);
         if (!e.missing.empty()) out["partials_missing"] = bj::value_from(e.missing);
         if (!e.errors.empty()) out["partial_errors"] = bj::value_from(e.errors);
         auto in = inspect_html(e.html);
         out["checks"] = in.to_json();
         out["problems"] = problems_of(in, true, false);
         constexpr size_t kMax = 200 * 1024;
         if (bool_arg(args, "verbose")) out["body"] = e.html.size() > kMax ? e.html.substr(0, kMax) + "\n[truncated]" : e.html;
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool(
      {"template_put",
       "Writes a Mustache template, _templates/$name.html, after checking it parses. Start it with front matter "
       "\"---\\nlayout: base\\n---\\n\" to wrap it in another template. Use tenant _host to restyle every tenant. "
       "A template is live for every page naming it at once: to try one, preview it with page_render's templates "
       "(nothing stored), or put it under a new name (post-v2) and point a draft page at it. To add tags to <head> "
       "from a template wrapped in a layout, put them between <!--head--> and <!--/head-->; the built-in base prints "
       "them in its <head> ({{{head}}} in your own base). In a <script>, use {{{json.title}}}: each value as JSON. "
       "Remove one with template_delete.",
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

  mcp_->add_tool({"template_delete",
                  "Removes a tenant's template; pages naming it then use the built-in one of that name, if any, or fail "
                  "to render (page_lint finds them).",
                  object_schema({{"tenant", string_property("A tenant name, or _host.")},
                                 {"name", string_property("The template's name.")}},
                                {"tenant", "name"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto name = arg(args, "name");
                    if (!tenant || !name || !valid_template_name(*name)) return ToolResult::fail("tenant and a template name are required");
                    std::string path = "_templates/" + *name + ".html", err;
                    if (!content_->get(*tenant, path)) return ToolResult::fail("no template " + *name + " for " + *tenant);
                    if (!content_->remove(*tenant, path, &err)) return ToolResult::fail(err);
                    return ToolResult::ok(bj::object{{"tenant", *tenant}, {"deleted", path}});
                  }});

  mcp_->add_tool({"template_get",
                  "Reads a template's Mustache source as this tenant's pages get it: the tenant's own, else _host's, "
                  "else the built-in one (base, page, list), so a built-in can be copied and changed.",
                  object_schema({{"tenant", tenant_prop}, {"name", string_property("The template's name, like page or list.")}},
                                {"tenant", "name"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto name = arg(args, "name");
                    if (!tenant || !name) return ToolResult::fail("tenant and name are required");
                    std::string source;
                    auto text = site_->find_template(*tenant, *name, &source);
                    if (!text) return ToolResult::fail("no template named " + *name + " for " + *tenant);
                    return ToolResult::ok(bj::object{{"tenant", *tenant}, {"name", *name}, {"source", source}, {"template", *text}});
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


// ---- The content kit (design/machine/031.ContentKit.md): deploying many files in one call with
// checks before and after, etags, checking served URLs, and linting pages.

HttpOut Host::fetch_self(const std::string& domain, const std::string& path) {
  // A path on this host, nothing else: "@10.0.0.5/x" after the port would name another host.
  bool ok = path.starts_with("/") && !path.starts_with("//") && path.size() <= 2048;
  for (char c : path) ok = ok && c != '@' && c != '\\' && static_cast<unsigned char>(c) > 0x20 && c != 0x7f;
  if (!ok) {
    HttpOut bad;
    bad.error = "a path on the site, starting with / (no @, \\, spaces, or control characters)";
    return bad;
  }
  auto cfg = config_.get();
  std::string address = cfg->listen_address == "0.0.0.0" || cfg->listen_address == "::" || cfg->listen_address.empty()
                            ? "127.0.0.1"
                            : cfg->listen_address;
  if (address.find(':') != std::string::npos) address = "[" + address + "]";
  // Signed out, as a visitor or a search engine sees it: no cookie, no token.
  return send_http("GET", "http://" + address + ":" + std::to_string(server_->port()) + path,
                   {{"host", domain}, {"accept", "text/html,application/xhtml+xml,*/*;q=0.8"}}, "", "", true,
                   cfg->storage / "tmp", 15000);
}

void Host::register_content_kit_tools() {
  using mcp::boolean_property;
  using mcp::object_schema;
  using mcp::string_property;
  auto known_tenant = [this](const std::string& t) {
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == t) return true;
    }
    return false;
  };
  auto tenant_prop = string_property("A tenant name from the host config; may be left out when the host has one.");
  auto list_of = [](const char* what) {
    return bj::value(bj::object{{"type", "array"}, {"items", bj::object{{"type", "string"}}}, {"description", what}});
  };
  // A served URL checked as a visitor gets it.
  auto check_path = [this](const std::string& domain, const std::string& path, bool strict) {
    auto r = fetch_self(domain, path);
    bool html = r.content_type.find("text/html") != std::string::npos;
    auto in = inspect_html(html ? r.body : "");
    bj::object o{{"status", r.status}, {"content_type", r.content_type}, {"bytes", int64_t(r.body.size())},
                 {"sha256", base::sha256_hex(r.body)}};
    if (!r.error.empty()) o["error"] = r.error;
    if (html) o["checks"] = in.to_json();
    auto problems = problems_of(in, html, strict);
    if (r.status != 200) problems.push_back(bj::value(r.status == 0 && !r.error.empty() ? r.error : "status " + std::to_string(r.status)));
    o["problems"] = std::move(problems);
    return o;
  };

  mcp_->add_tool(
      {"list_etags",
       "The current etag (and file) of each path: a URL like /updates/hello (the file that answers it) or a file "
       "like css/site.css. Use them as if_etag in page_put or content_deploy so a write never clobbers a newer one.",
       object_schema({{"tenant", tenant_prop}, {"paths", list_of("Up to 200 URLs or file paths.")}}, {"tenant", "paths"}),
       [this, known_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!tenant || !known_tenant(*tenant)) return ToolResult::fail("no domain in the host config serves that tenant");
         auto* paths = args.if_contains("paths");
         if (!paths || !paths->is_array() || paths->as_array().size() > 200) return ToolResult::fail("paths is a list of up to 200");
         bj::object out;
         for (const auto& p : paths->as_array()) {
           if (!p.is_string()) continue;
           std::string path(p.as_string());
           auto file = answering_file(*content_, *tenant, path);
           auto f = file ? content_->get(*tenant, *file) : std::nullopt;
           out[path] = f ? bj::value(bj::object{{"file", *file}, {"etag", f->etag}}) : bj::value(nullptr);
         }
         return ToolResult::ok(bj::object{{"tenant", *tenant}, {"etags", std::move(out)}});
       }});

  mcp_->add_tool(
      {"content_deploy",
       "Deploys many pages and files in one call, checked before and after (031 § 4). Before: each item's sha256 "
       "(of its body or bytes, as you sent them) must match what arrived, and its if_etag (from list_etags; \"none\" "
       "for a new one) must match what's there; if any item fails, nothing is written. Then everything is written, "
       "and each URL is fetched signed out, as a visitor gets it: status, content type, and for HTML no unrendered "
       "{{, at most one parseable ld+json, no empty <p>, no http: links, no raw markdown links (strict adds title and "
       "description lengths and exactly one ld+json). Answers one line per item, and ok.",
       object_schema({{"tenant", tenant_prop},
                      {"pages", bj::object{{"type", "array"},
                                           {"items", bj::object{{"type", "object"}}},
                                           {"description", "page_put's arguments each ({path, format, body, title?, template?, "
                                                           "fields?}), plus sha256? of body and if_etag?."}}},
                      {"files", bj::object{{"type", "array"},
                                           {"items", bj::object{{"type", "object"}}},
                                           {"description", "Files: {path, text | base64, sha256?, if_etag?}."}}},
                      {"verify", boolean_property("Fetch each URL after writing (default true).")},
                      {"strict", boolean_property("Also title 50-60, description 120-160, exactly one ld+json.")}},
                     {"tenant"}),
       [this, known_tenant, check_path](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!tenant || !known_tenant(*tenant)) return ToolResult::fail("no domain in the host config serves that tenant");
         struct Item {
           std::string kind, path, file, data, url;
           std::vector<std::string> others;
         };
         std::vector<Item> items;
         bj::array lines;
         bool ok = true;
         auto etag_check = [&](const bj::object& o, const std::vector<std::string>& files, std::string* why) {
           auto want = o.if_contains("if_etag");
           if (!want || !want->is_string()) return true;
           std::string current = "none";
           for (const auto& f : files) {
             if (auto got = content_->get(*tenant, f)) {
               current = got->etag;
               break;
             }
           }
           std::string w(want->as_string());
           if (w == current || (w.size() + 2 == current.size() && "\"" + w + "\"" == current)) return true;
           *why = "if_etag " + w + " but it is " + current;
           return false;
         };
         auto sha_check = [&](const bj::object& o, const std::string& data, std::string* why) {
           auto want = o.if_contains("sha256");
           if (!want || !want->is_string()) return true;
           std::string got = base::sha256_hex(data);
           if (base::to_lower(std::string(want->as_string())) == got) return true;
           *why = "sha256 " + got + " arrived, not " + std::string(want->as_string());
           return false;
         };
         if (auto* pages = args.if_contains("pages")) {
           if (!pages->is_array() || pages->as_array().size() > 100) return ToolResult::fail("pages is a list of up to 100");
           for (const auto& pv : pages->as_array()) {
             if (!pv.is_object()) return ToolResult::fail("each page is an object of page_put's arguments");
             const auto& po = pv.as_object();
             std::string err, path = po.contains("path") && po.at("path").is_string() ? std::string(po.at("path").as_string()) : "";
             auto built = build_page(*site_, *tenant, po, &err);
             std::string body = po.contains("body") && po.at("body").is_string() ? std::string(po.at("body").as_string()) : "";
             std::vector<std::string> answering = built ? built->others : std::vector<std::string>{};
             if (built) answering.insert(answering.begin(), built->file);
             std::string why = err;
             if (built && sha_check(po, body, &why) && etag_check(po, answering, &why)) {
               auto url = site::page_url(built->file);
               items.push_back({"page", path, built->file, built->text, url ? *url : path, built->others});
               continue;
             }
             ok = false;
             lines.push_back(bj::value("✗ " + path + ": " + why));
           }
         }
         if (auto* files = args.if_contains("files")) {
           if (!files->is_array() || files->as_array().size() > 200) return ToolResult::fail("files is a list of up to 200");
           for (const auto& fv : files->as_array()) {
             if (!fv.is_object()) return ToolResult::fail("each file is {path, text | base64, sha256?, if_etag?}");
             const auto& fo = fv.as_object();
             std::string path = fo.contains("path") && fo.at("path").is_string() ? std::string(fo.at("path").as_string()) : "";
             while (!path.empty() && path.front() == '/') path.erase(path.begin());
             std::string data, why;
             bool has_text = fo.contains("text") && fo.at("text").is_string();
             bool has_b64 = fo.contains("base64") && fo.at("base64").is_string();
             if (path.empty() || has_text == has_b64) {
               why = "a file is {path, text | base64}";
             } else if (has_text) {
               data = std::string(fo.at("text").as_string());
             } else if (auto d = base::base64_decode(std::string(fo.at("base64").as_string()))) {
               data = std::move(*d);
             } else {
               why = "base64 is not valid";
             }
             if (why.empty() && sha_check(fo, data, &why) && etag_check(fo, {path}, &why)) {
               items.push_back({"file", path, path, std::move(data), "/" + path, {}});
               continue;
             }
             ok = false;
             lines.push_back(bj::value("✗ " + path + ": " + why));
           }
         }
         if (!ok) {
           lines.push_back(bj::value("nothing was written"));
           return ToolResult::ok(bj::object{{"ok", false}, {"lines", std::move(lines)}});
         }
         if (items.empty()) return ToolResult::fail("give pages or files");
         for (const auto& it : items) {
           std::string err;
           if (!content_->put(*tenant, it.file, it.data, &err)) {
             lines.push_back(bj::value("✗ " + it.path + ": " + err));
             ok = false;
             continue;
           }
           for (const auto& o : it.others) {
             if (content_->get(*tenant, o)) content_->remove(*tenant, o, &err);
           }
         }
         auto vb = args.if_contains("verify");
         bool verify = !(vb && vb->is_bool() && !vb->as_bool());
         auto sb = args.if_contains("strict");
         bool strict = sb && sb->is_bool() && sb->as_bool();
         std::string domain = site_of(*tenant);
         for (const auto& it : items) {
           auto f = content_->get(*tenant, it.file);
           std::string etag = f ? f->etag : "";
           if (!verify) {
             lines.push_back(bj::value("✓ " + it.url + " written " + etag));
             continue;
           }
           auto c = check_path(domain, it.url == "/index.html" ? "/" : it.url, strict);
           const auto& problems = c.at("problems").as_array();
           if (it.kind == "file" && c.at("status").as_int64() == 200 && f && std::string(c.at("sha256").as_string()) != base::sha256_hex(f->data)) {
             ok = false;
             lines.push_back(bj::value("✗ " + it.url + " served bytes differ from what was written"));
             continue;
           }
           std::string line = (problems.empty() ? "✓ " : "✗ ") + it.url + " " + std::to_string(c.at("status").as_int64()) +
                              " " + std::string(c.at("content_type").as_string()) + " " + etag;
           if (auto* ch = c.if_contains("checks")) line += " ld=" + std::to_string(ch->at("ld_json").as_int64());
           for (const auto& p : problems) line += "; " + std::string(p.as_string());
           if (!problems.empty()) ok = false;
           lines.push_back(bj::value(line));
         }
         return ToolResult::ok(bj::object{{"ok", ok}, {"lines", std::move(lines)}});
       }});

  mcp_->add_tool(
      {"url_check",
       "Fetches URLs on this host's sites, signed out, as a visitor gets them, and answers for each: status, content "
       "type, bytes, sha256, and for HTML the counts a deploy checks (ld+json, unrendered {{, empty <p>, http: links, "
       "raw markdown links, title, description length) with the problems found. Replaces hand-written curl checks.",
       object_schema({{"urls", list_of("Up to 50: https://example.com/updates, or a path with tenant.")},
                      {"tenant", string_property("For paths without a host.")},
                      {"strict", boolean_property("Also title 50-60, description 120-160, exactly one ld+json.")}},
                     {"urls"}),
       [this, check_path](const bj::object& args) {
         auto* urls = args.if_contains("urls");
         if (!urls || !urls->is_array() || urls->as_array().empty() || urls->as_array().size() > 50) {
           return ToolResult::fail("urls is a list of 1 to 50");
         }
         auto cfg = config_.get();
         auto sb = args.if_contains("strict");
         bool strict = sb && sb->is_bool() && sb->as_bool();
         std::string tenant = arg(args, "tenant").value_or("");
         bj::array out;
         for (const auto& u : urls->as_array()) {
           std::string url = u.is_string() ? std::string(u.as_string()) : "";
           std::string domain, path;
           if (url.starts_with("https://") || url.starts_with("http://")) {
             auto rest = url.substr(url.find("//") + 2);
             auto slash = rest.find('/');
             domain = base::to_lower(rest.substr(0, slash));
             path = slash == std::string::npos ? "/" : rest.substr(slash);
           } else if (url.starts_with("/") && !tenant.empty()) {
             domain = site_of(tenant);
             path = url;
           }
           auto d = cfg->domains.find(domain);
           if (domain.empty() || d == cfg->domains.end()) {
             out.push_back(bj::object{{"url", url}, {"error", "not a URL of this host's sites (use web_fetch for others)"}});
             continue;
           }
           // A focused operator checks only their tenants' sites.
           if (auto refused = scope_refusal(operator_scope(current_operator()), current_operator(), "url_check",
                                            bj::object{{"tenant", d->second.tenant}})) {
             out.push_back(bj::object{{"url", url}, {"error", *refused}});
             continue;
           }
           auto c = check_path(domain, path, strict);
           c["url"] = url;
           out.push_back(std::move(c));
         }
         return ToolResult::ok(bj::object{{"results", std::move(out)}});
       }});

  mcp_->add_tool(
      {"page_lint",
       "Lints a tenant's pages as served (signed out): no unrendered {{, at most one parseable ld+json, no empty "
       "<p>, no http: links, no raw markdown links; strict adds a title of 50 to 60 characters, a description of 120 "
       "to 160, and exactly one ld+json. Every listed page by default (sitemap's set), or the paths given.",
       object_schema({{"tenant", tenant_prop}, {"paths", list_of("URLs to lint; default every listed page.")},
                      {"strict", boolean_property("The search checks too.")}},
                     {"tenant"}),
       [this, known_tenant, check_path](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!tenant || !known_tenant(*tenant)) return ToolResult::fail("no domain in the host config serves that tenant");
         std::vector<std::string> paths;
         if (auto* p = args.if_contains("paths"); p && p->is_array()) {
           for (const auto& v : p->as_array()) {
             if (v.is_string()) paths.emplace_back(v.as_string());
           }
         } else {
           for (const auto& page : site_->listed_pages(*tenant)) paths.push_back(page.url);
         }
         if (paths.size() > 300) paths.resize(300);
         auto sb = args.if_contains("strict");
         bool strict = sb && sb->is_bool() && sb->as_bool();
         std::string domain = site_of(*tenant);
         bj::array out;
         int clean = 0;
         for (const auto& path : paths) {
           auto c = check_path(domain, path, strict);
           if (c.at("problems").as_array().empty()) {
             ++clean;
             continue;
           }
           out.push_back(bj::object{{"url", path}, {"problems", c.at("problems")}});
         }
         return ToolResult::ok(bj::object{{"checked", int64_t(paths.size())}, {"clean", clean}, {"pages", std::move(out)}});
       }});

  mcp_->add_tool({"tool_schemas",
                  "Each operator tool's schema version (a hash of its input schema, also in tools/list _meta) and the "
                  "old argument names it still accepts. Compare with what you cached: a changed hash means re-read "
                  "tools/list. Arguments a tool doesn't know are named in its answer's ignored_arguments.",
                  object_schema({}, {}),
                  [this](const bj::object&) { return ToolResult::ok(bj::object{{"tools", mcp_->schemas()}}); }});
}

}  // namespace goober::host
