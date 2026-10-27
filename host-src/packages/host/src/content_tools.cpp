// The operator tools for building sites (design/machine/007.Content.md §9): the guide,
// pages, templates, and attachments.

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/names.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/render/frontmatter.h"
#include "goober/render/mustache.h"
#include "goober/router/path.h"

extern char** environ;

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
attachment_put takes base64 bytes or an https url to fetch, plus name, alt, and visibility
(public, or members). It answers with /attachment/$id.$ext; use that URL in pages:
  ![A red truck](/attachment/3.jpg)      or   image: /attachment/3.jpg
PNG, JPEG, GIF, WebP, AVIF, ICO, SVG, and PDF, detected from the bytes. People can also upload
at /attachment/ in a browser, as an owner of the tenant.

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
  int out[2], err[2];
  if (pipe(out) != 0) return *error = "pipe failed", std::nullopt;
  if (pipe(err) != 0) {
    close(out[0]);
    close(out[1]);
    return *error = "pipe failed", std::nullopt;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err[1], 2);
  posix_spawn_file_actions_addclose(&actions, out[0]);
  posix_spawn_file_actions_addclose(&actions, err[0]);
  std::string limit = std::to_string(max_bytes);
  std::vector<std::string> argv_s = {"curl",  "-sS",          "-f",      "-L",         "--max-redirs", "3",
                                     "--proto", "=https",     "--proto-redir", "=https", "--max-time",  "20",
                                     "--max-filesize", limit, "-A", "goober-host", "--", url};
  std::vector<char*> argv;
  for (auto& a : argv_s) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, "curl", &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(out[1]);
  close(err[1]);
  if (rc != 0) {
    close(out[0]);
    close(err[0]);
    return *error = "could not run curl", std::nullopt;
  }
  std::string body, msg;
  char buf[65536];
  bool too_big = false;
  for (ssize_t n; (n = read(out[0], buf, sizeof buf)) > 0;) {
    if (body.size() + size_t(n) > max_bytes) {
      too_big = true;
      break;
    }
    body.append(buf, size_t(n));
  }
  close(out[0]);
  if (too_big) kill(pid, SIGTERM);
  for (ssize_t n; (n = read(err[0], buf, sizeof buf)) > 0;) msg.append(buf, size_t(std::min<ssize_t>(n, 2000)));
  close(err[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (too_big) return *error = "the file is larger than " + limit + " bytes", std::nullopt;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return *error = "fetching " + url + " failed: " + (msg.empty() ? "curl exited with an error" : msg), std::nullopt;
  }
  return body;
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
       "tags, draft, requirements, list, sort, per-page, feed). Replaces the page at that URL, whatever its format.",
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
         if (*format == "document" && !fields.empty()) {
           return ToolResult::fail("a document is served as written, with no front matter; use fragment to give a "
                                   "title, template, or fields");
         }
         if (*format == "document" && body->starts_with("---")) {
           return ToolResult::fail("a document can't start with ---, which would read as front matter");
         }
         std::string ext = *format == "markdown" ? ".md" : ".html";
         std::string file = file_for(*site_, *tenant, *path, ext);
         if (site::is_private_path(file)) return ToolResult::fail("a page's path can't have a segment starting with _");
         std::string text;
         if (*format != "document") {
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
         // One page per URL: drop the same page in the other format.
         std::string other = file.substr(0, file.rfind('.')) + (ext == ".md" ? ".html" : ".md");
         bj::array removed;
         if (content_->get(*tenant, other) && content_->remove(*tenant, other, &err)) removed.push_back(bj::value(other));
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
         if (path->ends_with(".md") || path->ends_with(".html")) p = site_->load(*tenant, *path);
         if (!p) {
           std::string file = file_for(*site_, *tenant, *path, ".md");
           p = site_->load(*tenant, file);
         }
         if (!p) return ToolResult::fail("no page at " + *path);
         return ToolResult::ok(bj::object{{"file", p->path}, {"url", p->url}, {"kind", site::kind_name(p->kind)},
                                          {"fields", p->fields}, {"body", p->body}});
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
       "Stores an image or PDF and returns its URL, /attachment/$id.$ext, to use in pages. Give base64 bytes, or an "
       "https url for the host to fetch. The type comes from the bytes: PNG, JPEG, GIF, WebP, AVIF, ICO, SVG, or PDF.",
       object_schema({{"tenant", tenant_prop},
                      {"base64", string_property("The file's bytes as base64.")},
                      {"url", string_property("An https URL to fetch the file from.")},
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
         if (bool(b64) == bool(url)) return ToolResult::fail("give exactly one of base64 or url");
         std::string bytes, err;
         std::string name = arg(args, "name").value_or("");
         if (b64) {
           auto decoded = base::base64_decode(*b64);
           if (!decoded) return ToolResult::fail("base64 is not valid");
           bytes = std::move(*decoded);
         } else {
           auto fetched = fetch_https(*url, size_t(config_.get()->body_max_bytes), &err);
           if (!fetched) return ToolResult::fail(err);
           bytes = std::move(*fetched);
           if (name.empty()) {
             std::string u = url->substr(0, url->find_first_of("?#"));
             name = u.substr(u.rfind('/') + 1);
           }
         }
         if (int64_t(bytes.size()) > config_.get()->body_max_bytes) {
           return ToolResult::fail("larger than limits.body-max-bytes");
         }
         auto a = attachments_->put(*tenant, bytes, name, arg(args, "alt").value_or(""),
                                    arg(args, "visibility").value_or("public"), base::now_unix_ms(), &err);
         if (!a) return ToolResult::fail(err);
         return ToolResult::ok(attachment_json(*a, nullptr));
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
