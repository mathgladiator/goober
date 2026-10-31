#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goober/content/content.h"
#include "goober/json/json.h"

namespace goober::site {

namespace bj = boost::json;

// What a URL turned out to be (design/machine/007.Content.md §3).
enum class Kind { kStatic, kMarkdown, kFragment, kDocument, kListing, kDirectory, kFeed, kRedirect, kSource };

const char* kind_name(Kind kind);

// A page file, parsed.
// The Content-Security-Policy of templated pages (007 §10), shared with box responses the
// host renders through a template.
const char* templated_csp();

struct Page {
  std::string path;  // in the content tree, like "blog/hello.md"
  std::string url;   // like "/blog/hello"
  Kind kind = Kind::kMarkdown;
  bj::object fields;  // front matter
  std::string body;   // without front matter
  std::string raw;    // the whole file
  // Why the front matter couldn't be trusted, or "". Such a page is never served or
  // listed, since a malformed requirements line must not make a private page public.
  std::string broken;
};

// One request for content.
// A json_ld value (an object or array, or a string holding one) as the host-built
// <script type="application/ld+json"> tag, escaped so nothing in it can end the script; or
// nullopt. Boxes' json_ld and pages' json_ld field both go through it.
std::optional<std::string> json_ld_script(const boost::json::value& ld);

struct Visit {
  std::string tenant;
  std::string path;  // a normalized URL path
  std::map<std::string, std::string> query;
  std::string accept;
  std::string origin;  // "https://host", for absolute links in feeds
  bj::value user = nullptr;
  bool drafts = false;  // render drafts too, for previews
  // Decides a page's requirements: 200 to allow, or 401, 403, or 400. Without it,
  // a page with requirements is refused with 403.
  std::function<int(const bj::value& requirements)> authorize;
};

struct Result {
  int status = 404;
  Kind kind = Kind::kStatic;
  std::string file;  // the content path that answered
  std::string content_type;
  std::string body;
  std::string etag;      // quoted
  std::string location;  // for a redirect
  bool restricted = false;  // the page has requirements, so it is cached privately
  std::string error;        // why rendering failed, for a 500
  std::vector<std::pair<std::string, std::string>> headers;
};

// The URL of a page file ("blog/index.md" is "/blog"), or nullopt when the path isn't a page.
std::optional<std::string> page_url(std::string_view content_path);

// True when any segment of a content path starts with "_", or the path is under the root's
// error-message/ (the host's error pages, served only as errors): never served, never listed.
bool is_private_path(std::string_view content_path);

// The kind of a file in the tree, from its extension and, for HTML, its front matter.
Kind kind_of(std::string_view content_path, std::string_view data);

// Renders a tenant's content tree as a site: pages, templates, listings, and feeds.
class Site {
 public:
  explicit Site(const content::ContentStore& store) : store_(store) {}

  // Answers one request for content.
  Result visit(const Visit& visit) const;

  // Loads one page file.
  std::optional<Page> load(std::string_view tenant, std::string_view content_path) const;

  // Every page under a directory prefix ("" for all), recursively, drafts included.
  std::vector<Page> pages(std::string_view tenant, std::string_view prefix) const;

  // A template's text: the tenant's, then the host tree's, then the built-in one.
  // `source`, when given, is set to "tenant", "host", or "built-in".
  std::optional<std::string> find_template(std::string_view tenant, std::string_view name,
                                           std::string* source = nullptr) const;

  // Renders a named template, with its layouts, over `data`, for a box route that
  // names a template (007 §8). `inline_templates` are the manifest's own, found first.
  // Adds site, data, and now to `data` when absent.
  std::optional<std::string> render_named(std::string_view tenant, std::string_view name, bj::object data,
                                          const std::map<std::string, std::string>* inline_templates,
                                          std::string* error) const;

  // The names of the built-in templates.
  static std::vector<std::string> builtin_names();

  // The pages a search engine or a model should see (design/machine/031.ContentKit.md § 1):
  // published (not drafts), public (no requirements), not redirects, not noindex, and not
  // "sitemap": false. `origin` is "https://example.com".
  std::vector<Page> listed_pages(std::string_view tenant) const;
  // /sitemap.xml from the pages' metadata: canonical (or the URL), lastmod (updated or date).
  std::string sitemap_xml(std::string_view tenant, const std::string& origin) const;
  // /llms.txt (llmstxt.org): the site's title and description, then each page as a link with
  // its description.
  std::string llms_txt(std::string_view tenant, const std::string& origin) const;

 private:
  Result visit_unexpanded(const Visit& visit) const;
  Result render_page(const Visit& v, const Page& page, bool listing_only_for_feed) const;
  Result directory(const Visit& v, const std::string& dir) const;
  std::vector<bj::object> entries(const Visit& v, const std::string& dir) const;
  bj::object base_data(std::string_view tenant) const;
  // json_ld: the data's "json_ld" is a script tag the host built from a box's value
  // (render_named), so {{{json_ld}}} may emit it unescaped.
  std::optional<std::string> render_template(std::string_view tenant, std::string_view name, bj::object data,
                                             const std::map<std::string, std::string>* inline_templates,
                                             std::string* error, bool json_ld = false) const;

  const content::ContentStore& store_;
};

}  // namespace goober::site
