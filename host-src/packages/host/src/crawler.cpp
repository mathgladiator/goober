// Crawls, the operator's web tools, and the web service boxes call (800.Crawl.md;
// design/machine/017.Crawl.md). The fetching itself, with its manners, is web_fetch.cpp.

#include <iostream>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/web/html.h"
#include "goober/web/robots.h"
#include "network.h"
#include "web_fetch.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

const char* kCrawlSchema = R"sql(
CREATE TABLE IF NOT EXISTS crawls (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, spec TEXT NOT NULL, state TEXT NOT NULL,
  created_ms INTEGER NOT NULL, updated_ms INTEGER NOT NULL, fetched INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS frontier (
  crawl_id INTEGER NOT NULL, url TEXT NOT NULL, depth INTEGER NOT NULL, kind TEXT NOT NULL DEFAULT 'page',
  state TEXT NOT NULL, status INTEGER, title TEXT, error TEXT, fetched_ms INTEGER, tries INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (crawl_id, url));
CREATE INDEX IF NOT EXISTS frontier_next ON frontier (crawl_id, state, depth);
CREATE TABLE IF NOT EXISTS records (
  id INTEGER PRIMARY KEY, crawl_id INTEGER NOT NULL, url TEXT NOT NULL, kind TEXT NOT NULL, data TEXT NOT NULL,
  at_ms INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS records_by_crawl ON records (crawl_id, kind);
)sql";

constexpr int64_t kMaxPagesCap = 5000;
constexpr int64_t kMaxDepthCap = 10;

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

int64_t int_arg(const bj::object& args, const char* key, int64_t def) {
  auto* v = args.if_contains(key);
  return v && v->is_int64() ? v->as_int64() : def;
}

std::vector<std::string> strings(const bj::object& o, const char* key) {
  std::vector<std::string> out;
  if (auto* v = o.if_contains(key); v && v->is_array()) {
    for (const auto& s : v->as_array()) {
      if (s.is_string()) out.emplace_back(s.as_string());
    }
  }
  return out;
}

// What a crawl may visit (800.Crawl.md § Crawls).
struct Scope {
  std::vector<web::Url> seeds;
  std::string mode = "site";  // "site": the seeds' sites; "prefix": URLs starting with a prefix
  std::vector<std::string> prefixes, include, exclude;
  bool allows(const web::Url& u) const {
    std::string s = u.str();
    bool in = false;
    if (mode == "prefix") {
      for (const auto& p : prefixes) in |= s.starts_with(p);
    } else {
      for (const auto& seed : seeds) in |= web::same_site(seed, u);
    }
    if (!in) return false;
    for (const auto& x : exclude) {
      if (web::robots_match(x + "$", s)) return false;
    }
    if (include.empty()) return true;
    for (const auto& i : include) {
      if (web::robots_match(i + "$", s)) return true;
    }
    return false;
  }
};

Scope scope_of(const bj::object& spec) {
  Scope sc;
  for (const auto& s : strings(spec, "seeds")) {
    if (auto u = web::parse_url(s)) sc.seeds.push_back(*u);
  }
  if (auto m = arg(spec, "scope")) sc.mode = *m;
  sc.prefixes = strings(spec, "prefixes");
  sc.include = strings(spec, "include");
  sc.exclude = strings(spec, "exclude");
  return sc;
}

// The <loc> values of a sitemap or a sitemap index.
std::vector<std::string> sitemap_locs(std::string_view xml, size_t limit) {
  std::vector<std::string> out;
  size_t i = 0;
  while (out.size() < limit) {
    auto open = xml.find("<loc>", i);
    if (open == std::string_view::npos) break;
    auto close = xml.find("</loc>", open);
    if (close == std::string_view::npos) break;
    std::string loc = web::decode_entities(xml.substr(open + 5, close - open - 5));
    // CDATA-wrapped values, as some generators write them.
    if (loc.starts_with("<![CDATA[") && loc.ends_with("]]>")) loc = loc.substr(9, loc.size() - 12);
    out.push_back(loc);
    i = close + 6;
  }
  return out;
}

std::string type_of(const bj::value& item) {
  const bj::value* t = item.is_object() ? item.as_object().if_contains("@type") : nullptr;
  if (t && t->is_string()) return std::string(t->as_string());
  if (t && t->is_array() && !t->as_array().empty() && t->as_array()[0].is_string()) return std::string(t->as_array()[0].as_string());
  return "Thing";
}

}  // namespace

void Host::init_web() {
  auto log = [this](const FetchOptions& o, const std::string& url, int status, const std::string& error, int64_t us, size_t bytes) {
    egress_log(o.tenant.empty() ? "_host" : o.tenant, o.box.empty() ? "operator" : o.box, "web", "fetch", "get", url, status,
               error, us, 0, bytes, o.request_id, 0);
  };
  auto storage = config_.get()->storage;
  try {
    web_ = std::make_unique<WebFetcher>(storage, [this] { return config_.get(); }, log);
    std::lock_guard lock(web_->db_mu());
    web_->db().exec(kCrawlSchema);
  } catch (const std::exception& e) {
    // Not set aside: web.db holds the sites that asked not to be crawled. The robot gets an
    // empty stand-in for its cache and refuses every fetch until web.db is repaired and the
    // host restarts.
    stores_off_.push_back("web");
    startup_notes_.push_back(std::string("the robot is off: storage/web/web.db won't open (") + e.what() + ")");
    auto standin = storage / "web" / "standin";
    std::error_code ec;
    std::filesystem::remove_all(standin, ec);
    web_ = std::make_unique<WebFetcher>(standin, [this] { return config_.get(); }, log);
    web_->refuse_all("the robot is off: its store (web.db, with sites' opt-outs) won't open");
    std::lock_guard lock(web_->db_mu());
    web_->db().exec(kCrawlSchema);
  }
}

std::string Host::web_call(CallContext& ctx, const manifest::ExternalService& svc, const bj::value& request) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  if (!request.is_object()) return fail("a fetch is {\"url\", \"views\", \"select\", \"extract\", \"max_age_ms\"}");
  const auto& r = request.as_object();
  for (const auto& kv : r) {
    if (kv.key() != "url" && kv.key() != "views" && kv.key() != "select" && kv.key() != "extract" &&
        kv.key() != "max_age_ms") {
      return fail("a fetch has no \"" + std::string(kv.key()) + "\"; it is {url, views, select, extract, max_age_ms}");
    }
  }
  auto url = arg(r, "url");
  if (!url) return fail("a fetch needs url");
  FetchOptions o;
  o.hosts = svc.web_hosts;
  o.tenant = ctx.tenant;
  o.box = ctx.manifest->box;
  o.request_id = ctx.request_id;
  o.max_age_ms = std::clamp<int64_t>(int_arg(r, "max_age_ms", 3600000), 0, 30LL * 24 * 3600 * 1000);
  // Never past the request's own deadline: waiting for a site's turn included.
  o.timeout_ms = 15000;
  if (ctx.deadline_us) o.timeout_ms = std::min<int64_t>(o.timeout_ms, (ctx.deadline_us - base::monotonic_us()) / 1000 - 100);
  if (o.timeout_ms < 500) return fail("no time left for a fetch");
  auto page = web_->fetch(*url, o);
  bj::object out{{"url", page.url}, {"status", page.status}, {"content_type", page.content_type},
                 {"from_cache", page.from_cache}};
  if (page.fetched_ms) out["fetched_at"] = base::format_utc_ms(page.fetched_ms);
  if (!page.redirects.empty()) {
    bj::array redirects;
    for (const auto& x : page.redirects) redirects.emplace_back(x);
    out["redirects"] = std::move(redirects);
  }
  if (!page.error.empty()) {
    out["error"] = page.error;
    if (page.retry_after_ms) out["retry_after_ms"] = page.retry_after_ms;
    return json::dump(out);
  }
  std::string err;
  bj::array views;
  if (auto* v = r.if_contains("views"); v && v->is_array()) views = v->as_array();
  const bj::value* extract = r.if_contains("extract");
  auto rendered = page_views(page, views, arg(r, "select").value_or(""), extract ? *extract : bj::value(nullptr), &err);
  if (!rendered) return fail(err);
  for (auto& kv : *rendered) out[kv.key()] = std::move(kv.value());
  return json::dump(out);
}

int Host::crawl_step() {
  if (!web_) return 0;
  struct Crawl {
    int64_t id;
    bj::object spec;
  };
  std::vector<Crawl> crawls;
  {
    std::lock_guard lock(web_->db_mu());
    sqlite::Stmt s(web_->db(), "SELECT id, spec FROM crawls WHERE state = 'running' ORDER BY id");
    while (s.step()) {
      std::string err;
      auto spec = json::parse(s.text(1), &err);
      if (spec && spec->is_object()) crawls.push_back(Crawl{s.int64(0), spec->as_object()});
    }
  }
  int fetched = 0;
  for (const auto& c : crawls) {
    // The next few waiting pages, shallowest first; the first whose site is ready goes.
    struct Next {
      std::string url, kind;
      int64_t depth;
    };
    std::vector<Next> next;
    {
      std::lock_guard lock(web_->db_mu());
      sqlite::Stmt s(web_->db(),
                     "SELECT url, kind, depth FROM frontier WHERE crawl_id = ? AND state = 'pending' "
                     "ORDER BY kind = 'sitemap' DESC, depth, rowid LIMIT 20");
      s.bind(1, c.id);
      while (s.step()) next.push_back(Next{s.text(0), s.text(1), s.int64(2)});
      if (next.empty()) {
        sqlite::Stmt done(web_->db(), "UPDATE crawls SET state = 'done', updated_ms = ? WHERE id = ?");
        done.bind(1, base::now_unix_ms()).bind(2, c.id).run();
        continue;
      }
    }
    Scope scope = scope_of(c.spec);
    int64_t max_pages = std::clamp<int64_t>(int_arg(c.spec, "max_pages", 100), 1, kMaxPagesCap);
    int64_t max_depth = std::clamp<int64_t>(int_arg(c.spec, "max_depth", 3), 0, kMaxDepthCap);
    for (const auto& n : next) {
      FetchOptions o;
      o.box = "crawl:" + std::to_string(c.id);
      o.timeout_ms = 20000;
      o.max_wait_ms = 0;  // a site that isn't ready yet waits; another site may be
      o.max_age_ms = int_arg(c.spec, "max_age_ms", 6LL * 3600 * 1000);
      auto page = web_->fetch(n.url, o);
      if (page.retry_after_ms && page.status == 0) continue;  // not this site's turn
      ++fetched;
      // A redirect may leave the crawl's scope; what's there isn't the crawl's to read.
      if (page.error.empty() && n.kind == "page") {
        auto final_url = web::parse_url(page.url);
        if (!final_url || !scope.allows(*final_url)) page.error = "redirected out of the crawl's scope, to " + page.url.substr(0, 200);
      }
      std::string title;
      std::vector<std::pair<std::string, int64_t>> found;  // new frontier entries
      std::vector<std::pair<std::string, std::string>> records;
      if (page.error.empty() && page.status >= 200 && page.status < 300) {
        if (n.kind == "sitemap") {
          bool index = page.body.find("<sitemapindex") != std::string::npos;
          for (const auto& loc : sitemap_locs(page.body, 50000)) {
            auto u = web::parse_url(loc);
            if (!u) continue;
            if (index) {
              // A sitemap index may only point at the crawl's own sites.
              bool ours = false;
              for (const auto& seed : scope.seeds) ours |= web::same_site(seed, *u);
              if (ours) found.emplace_back("sitemap:" + u->str(), 0);
            } else if (scope.allows(*u)) {
              found.emplace_back(u->str(), 1);
            }
          }
        } else if (page.is_html()) {
          auto doc = web::Document::parse(page.body);
          auto base = doc.base_for(*web::parse_url(page.url));
          auto meta = web::view_meta(doc, base);
          title = meta.contains("title") ? std::string(meta.at("title").as_string()) : "";
          std::string robots_meta = meta.contains("robots") ? base::to_lower(meta.at("robots").as_string()) : "";
          bool nofollow = robots_meta.find("nofollow") != std::string::npos || robots_meta.find("none") != std::string::npos;
          bool noindex = robots_meta.find("noindex") != std::string::npos || robots_meta.find("none") != std::string::npos;
          if (!nofollow && n.depth < max_depth) {
            for (const auto& l : web::view_links(doc, base)) {
              const auto& lo = l.as_object();
              if (lo.contains("rel") && base::to_lower(lo.at("rel").as_string()).find("nofollow") != std::string::npos) continue;
              auto u = web::parse_url(lo.at("url").as_string());
              if (u && scope.allows(*u)) found.emplace_back(u->str(), n.depth + 1);
            }
          }
          if (!noindex) {
            for (const auto& item : web::view_jsonld(doc)) records.emplace_back(type_of(item), json::dump(item));
            if (auto* spec = c.spec.if_contains("extract"); spec && !spec->is_null()) {
              std::string err;
              if (auto recs = web::view_extract(doc, base, *spec, &err)) {
                for (const auto& r : *recs) records.emplace_back("extract", json::dump(r));
              }
            }
          }
        }
      }
      std::lock_guard lock(web_->db_mu());
      auto& db = web_->db();
      db.exec("BEGIN IMMEDIATE");
      try {
        sqlite::Stmt u(db,
                       "UPDATE frontier SET state = ?, status = ?, title = ?, error = ?, fetched_ms = ?, tries = tries + 1 "
                       "WHERE crawl_id = ? AND url = ?");
        u.bind(1, page.error.empty() ? "done" : "error").bind(2, int64_t(page.status)).bind(3, title);
        page.error.empty() ? u.bind_null(4) : u.bind(4, page.error);
        u.bind(5, base::now_unix_ms()).bind(6, c.id).bind(7, n.url).run();
        sqlite::Stmt count(db, "SELECT count(*), coalesce(sum(kind = 'sitemap'), 0) FROM frontier WHERE crawl_id = ?");
        count.bind(1, c.id);
        int64_t total = 0, sitemaps = 0;
        if (count.step()) {
          sitemaps = count.int64(1);
          total = count.int64(0) - sitemaps;
        }
        for (const auto& [url, depth] : found) {
          bool sitemap = url.starts_with("sitemap:");
          if (!sitemap && total >= max_pages) continue;
          if (sitemap && sitemaps >= 100) continue;  // nested indexes can't grow a crawl forever
          if (sitemap) ++sitemaps;
          sqlite::Stmt ins(db,
                           "INSERT OR IGNORE INTO frontier (crawl_id, url, depth, kind, state) VALUES (?, ?, ?, ?, 'pending')");
          ins.bind(1, c.id).bind(2, sitemap ? url.substr(8) : url).bind(3, depth).bind(4, sitemap ? "sitemap" : "page").run();
          if (!sitemap) total += db.changes();
        }
        for (const auto& [kind, data] : records) {
          sqlite::Stmt r(db, "INSERT INTO records (crawl_id, url, kind, data, at_ms) VALUES (?, ?, ?, ?, ?)");
          r.bind(1, c.id).bind(2, page.url.empty() ? n.url : page.url).bind(3, kind).bind(4, data).bind(5, base::now_unix_ms()).run();
        }
        sqlite::Stmt up(db, "UPDATE crawls SET fetched = fetched + 1, updated_ms = ? WHERE id = ?");
        up.bind(1, base::now_unix_ms()).bind(2, c.id).run();
        db.exec("COMMIT");
      } catch (const std::exception& e) {
        db.exec("ROLLBACK");
        std::cerr << "goober-host: crawl " << c.id << ": " << e.what() << "\n";
      }
      break;  // one page per crawl per step, so crawls share the robot
    }
  }
  return fetched;
}

void Host::run_crawls() {
  std::unique_lock lock(timer_thread_mu_);
  while (!timers_stopping_) {
    lock.unlock();
    int fetched = 0;
    try {
      static thread_local int64_t pruned_ms = 0;
      if (web_ && base::now_unix_ms() - pruned_ms > 3600000) {
        pruned_ms = base::now_unix_ms();
        web_->prune(pruned_ms);
      }
      fetched = crawl_step();
      loop_beat("crawler");
    } catch (const std::exception& e) {
      std::cerr << "goober-host: crawler: " << e.what() << "\n";
      loop_beat("crawler", e.what());
    }
    lock.lock();
    timer_cv_.wait_for(lock, std::chrono::milliseconds(fetched ? 50 : 500), [this] { return timers_stopping_; });
  }
}

void Host::register_web_tools() {
  using mcp::object_schema;
  using mcp::string_property;
  auto views_prop = bj::value(bj::object{
      {"type", "array"},
      {"items", bj::object{{"type", "string"},
                           {"enum", bj::array{"html", "text", "markdown", "links", "meta", "jsonld", "tables", "json"}}}},
      {"description", "Which views of the page to return. Default: meta and markdown for HTML, text otherwise."}});
  auto extract_prop = bj::value(bj::object{
      {"type", "object"},
      {"description", "Records from repeated elements: {\"items\": \".product\", \"fields\": {\"name\": \".title\", "
                      "\"price\": \".price\", \"url\": \"a@href\", \"sku\": \"@data-sku\"}}. A field is a CSS selector "
                      "within the item (its text), sel@attr for an attribute, or @attr on the item itself."}});
  mcp_->add_tool(
      {"web_fetch",
       "Reads a web page through goober's polite robot (robots.txt obeyed, one request per site every few seconds, "
       "public addresses only, cached) and returns views that simplify it: html (the full page), text, markdown, links, "
       "meta (title, description, Open Graph, feeds, and the shop platform, like shopify), jsonld (schema.org data such "
       "as Product, Offer, LocalBusiness), tables, json, plus select (a CSS selector) and extract (records). Use it to "
       "look at a site before writing a crawl or a box. For this host's own sites, url_check reads them as visitors "
       "do. A read tool; the fetched page is cached for the robot.",
       object_schema({{"url", string_property("An http or https URL.")},
                      {"views", views_prop},
                      {"select", string_property("A CSS selector: tag, #id, .class, [attr=v], :first-child, "
                                                 ":nth-child(n), descendant and > combinators, commas.")},
                      {"extract", extract_prop},
                      {"max_age_ms", mcp::integer_property("Use a cached copy younger than this (default an hour; 0 "
                                                           "to fetch again).")}},
                     {"url"}),
       [this](const bj::object& args) {
         FetchOptions o;
         o.max_age_ms = std::max<int64_t>(0, int_arg(args, "max_age_ms", 3600000));
         o.timeout_ms = 30000;
         auto page = web_->fetch(arg(args, "url").value_or(""), o);
         if (!page.error.empty()) {
           return ToolResult::fail(page.error + (page.status ? " (status " + std::to_string(page.status) + ")" : ""));
         }
         bj::array views;
         if (auto* v = args.if_contains("views"); v && v->is_array()) views = v->as_array();
         const bj::value* extract = args.if_contains("extract");
         std::string err;
         auto out = page_views(page, views, arg(args, "select").value_or(""), extract ? *extract : bj::value(nullptr), &err);
         if (!out) return ToolResult::fail(err);
         (*out)["url"] = page.url;
         (*out)["status"] = page.status;
         (*out)["content_type"] = page.content_type;
         (*out)["fetched_at"] = base::format_utc_ms(page.fetched_ms);
         (*out)["from_cache"] = page.from_cache;
         return ToolResult::ok(std::move(*out));
       }});

  mcp_->add_tool(
      {"crawl_start",
       "Starts a background crawl from seed URLs, following links within a scope, politely, and recording every page, "
       "the schema.org JSON-LD on each, and records from an extract spec. Read results with crawl_pages and "
       "crawl_records. Good for building an inventory of a site's products. Runs in the background on the host's "
       "robot (host-wide, not a tenant's); answers the crawl's id (a number, for crawl_pages, crawl_records, and "
       "crawl_stop) and name. A changing tool.",
       object_schema(
           {{"name", string_property("A label, like hill-ranch.")},
            {"seeds", bj::value(bj::object{{"type", "array"}, {"items", bj::object{{"type", "string"}}},
                                           {"description", "Starting URLs."}})},
            {"scope", mcp::enum_property("site: the seeds' own sites (www. or not); prefix: URLs starting with one of "
                                         "prefixes.",
                                         {"site", "prefix"})},
            {"prefixes", bj::value(bj::object{{"type", "array"}, {"items", bj::object{{"type", "string"}}}})},
            {"include", bj::value(bj::object{{"type", "array"}, {"items", bj::object{{"type", "string"}}},
                                             {"description", "Only URLs matching one of these (* is a wildcard), like "
                                                             "*/products/*."}})},
            {"exclude", bj::value(bj::object{{"type", "array"}, {"items", bj::object{{"type", "string"}}},
                                             {"description", "Never URLs matching these, like *?sort=*."}})},
            {"max_pages", mcp::integer_property("At most this many pages (default 100, at most 5000).")},
            {"max_depth", mcp::integer_property("Links followed from a seed (default 3, at most 10).")},
            {"sitemaps", mcp::boolean_property("Read each seed site's /sitemap.xml (and the sitemaps it indexes) for URLs.")},
            {"extract", extract_prop}},
           {"seeds"}),
       [this](const bj::object& args) {
         bj::object spec;
         Scope sc = scope_of(args);
         if (sc.seeds.empty()) return ToolResult::fail("seeds needs at least one http or https URL");
         if (sc.mode != "site" && sc.mode != "prefix") return ToolResult::fail("scope is site or prefix");
         if (sc.mode == "prefix" && sc.prefixes.empty()) return ToolResult::fail("scope prefix needs prefixes");
         for (const char* k : {"seeds", "scope", "prefixes", "include", "exclude", "max_pages", "max_depth", "sitemaps", "extract"}) {
           if (auto* v = args.if_contains(k)) spec[k] = *v;
         }
         if (auto* e = args.if_contains("extract"); e && !e->is_null()) {
           std::string err;
           auto probe = web::Document::parse("<p></p>");
           if (!web::view_extract(probe, sc.seeds[0], *e, &err)) return ToolResult::fail(err);
         }
         std::string name = arg(args, "name").value_or(sc.seeds[0].host);
         int64_t now = base::now_unix_ms();
         std::lock_guard lock(web_->db_mu());
         auto& db = web_->db();
         sqlite::Stmt ins(db, "INSERT INTO crawls (name, spec, state, created_ms, updated_ms) VALUES (?, ?, 'running', ?, ?)");
         ins.bind(1, name).bind(2, json::dump(spec)).bind(3, now).bind(4, now).run();
         int64_t id = db.last_insert_rowid();
         std::set<std::string> sitemaps;
         for (const auto& seed : sc.seeds) {
           sqlite::Stmt f(db, "INSERT OR IGNORE INTO frontier (crawl_id, url, depth, kind, state) VALUES (?, ?, 0, 'page', 'pending')");
           f.bind(1, id).bind(2, seed.str()).run();
           if (args.contains("sitemaps") && args.at("sitemaps").is_bool() && args.at("sitemaps").as_bool()) {
             sitemaps.insert(seed.origin() + "/sitemap.xml");
           }
         }
         for (const auto& s : sitemaps) {
           sqlite::Stmt f(db, "INSERT OR IGNORE INTO frontier (crawl_id, url, depth, kind, state) VALUES (?, ?, 0, 'sitemap', 'pending')");
           f.bind(1, id).bind(2, s).run();
         }
         return ToolResult::ok(bj::object{{"crawl", id}, {"name", name}, {"state", "running"},
                                          {"note", "Pages come at the robot's pace: a few seconds apart per site."}});
       }});

  auto crawl_prop = mcp::integer_property("The crawl's id, from crawl_start or crawl_list.");
  mcp_->add_tool({"crawl_list", "Lists crawls: name, state, pages fetched and waiting, and records found.",
                  object_schema({}, {}), [this](const bj::object&) {
                    std::lock_guard lock(web_->db_mu());
                    sqlite::Stmt s(web_->db(),
                                   "SELECT c.id, c.name, c.state, c.fetched, c.created_ms, c.updated_ms, "
                                   "(SELECT count(*) FROM frontier f WHERE f.crawl_id = c.id AND f.state = 'pending'), "
                                   "(SELECT count(*) FROM records r WHERE r.crawl_id = c.id) FROM crawls c ORDER BY c.id DESC LIMIT 100");
                    bj::array out;
                    while (s.step()) {
                      out.push_back(bj::object{{"crawl", s.int64(0)}, {"name", s.text(1)}, {"state", s.text(2)},
                                               {"fetched", s.int64(3)}, {"waiting", s.int64(6)}, {"records", s.int64(7)},
                                               {"started_at", base::format_utc_ms(s.int64(4))},
                                               {"updated_at", base::format_utc_ms(s.int64(5))}});
                    }
                    return ToolResult::ok(std::move(out));
                  }});
  mcp_->add_tool({"crawl_stop", "Stops a running crawl; what it found stays. crawl_rerun starts it over. A changing tool.",
                  object_schema({{"crawl", crawl_prop}}, {"crawl"}), [this](const bj::object& args) {
                    std::lock_guard lock(web_->db_mu());
                    sqlite::Stmt s(web_->db(), "UPDATE crawls SET state = 'stopped', updated_ms = ? WHERE id = ? AND state = 'running'");
                    s.bind(1, base::now_unix_ms()).bind(2, int_arg(args, "crawl", 0)).run();
                    return web_->db().changes() ? ToolResult::ok(bj::object{{"stopped", int_arg(args, "crawl", 0)}})
                                                : ToolResult::fail("no running crawl with that id");
                  }});
  mcp_->add_tool({"crawl_rerun",
                  "Runs a crawl again from its seeds, to see what changed: pages are fetched again (conditionally, so "
                  "unchanged ones cost the site almost nothing) and records are replaced. A changing tool.",
                  object_schema({{"crawl", crawl_prop}}, {"crawl"}), [this](const bj::object& args) {
                    int64_t id = int_arg(args, "crawl", 0);
                    std::lock_guard lock(web_->db_mu());
                    auto& db = web_->db();
                    sqlite::Stmt find(db, "SELECT spec FROM crawls WHERE id = ?");
                    find.bind(1, id);
                    if (!find.step()) return ToolResult::fail("no crawl with that id");
                    std::string err;
                    auto spec = json::parse(find.text(0), &err);
                    if (!spec || !spec->is_object()) return ToolResult::fail("the crawl's spec can't be read");
                    auto& o = spec->as_object();
                    // One transaction on the robot's shared connection: rolled back on any failure,
                    // so a failed rerun never leaves it open under every later crawl step.
                    db.exec("BEGIN IMMEDIATE");
                    try {
                      for (const char* sql : {"DELETE FROM frontier WHERE crawl_id = ?", "DELETE FROM records WHERE crawl_id = ?"}) {
                        sqlite::Stmt d(db, sql);
                        d.bind(1, id).run();
                      }
                      Scope sc = scope_of(o);
                      std::set<std::string> sitemaps;
                      bool with_sitemaps = o.contains("sitemaps") && o.at("sitemaps").is_bool() && o.at("sitemaps").as_bool();
                      for (const auto& seed : sc.seeds) {
                        sqlite::Stmt f(db, "INSERT OR IGNORE INTO frontier (crawl_id, url, depth, kind, state) VALUES (?, ?, 0, 'page', 'pending')");
                        f.bind(1, id).bind(2, seed.str()).run();
                        if (with_sitemaps) sitemaps.insert(seed.origin() + "/sitemap.xml");
                      }
                      for (const auto& s : sitemaps) {
                        sqlite::Stmt f(db, "INSERT OR IGNORE INTO frontier (crawl_id, url, depth, kind, state) VALUES (?, ?, 0, 'sitemap', 'pending')");
                        f.bind(1, id).bind(2, s).run();
                      }
                      // Fetched again, not served from the cache: a rerun is for seeing changes.
                      o["max_age_ms"] = 60000;
                      sqlite::Stmt up(db, "UPDATE crawls SET state = 'running', fetched = 0, spec = ?, updated_ms = ? WHERE id = ?");
                      up.bind(1, json::dump(*spec)).bind(2, base::now_unix_ms()).bind(3, id).run();
                      db.exec("COMMIT");
                    } catch (const std::exception& e) {
                      db.exec("ROLLBACK");
                      return ToolResult::fail(std::string("the rerun failed: ") + e.what());
                    }
                    return ToolResult::ok(bj::object{{"crawl", id}, {"state", "running"}});
                  }});
  mcp_->add_tool(
      {"crawl_pages", "The pages a crawl visited or will visit: URL, state (pending, done, error), status, title, error.",
       object_schema({{"crawl", crawl_prop},
                      {"state", mcp::enum_property("Only pages in this state.", {"pending", "done", "error"})},
                      {"query", string_property("Only pages whose URL or title contains this.")},
                      {"limit", mcp::integer_property("At most this many (default 100, at most 1000).")},
                      {"offset", mcp::integer_property("Skip this many.")}},
                     {"crawl"}),
       [this](const bj::object& args) {
         std::lock_guard lock(web_->db_mu());
         sqlite::Stmt s(web_->db(),
                        "SELECT url, kind, depth, state, status, title, error, fetched_ms FROM frontier WHERE crawl_id = ? "
                        "AND (? = '' OR state = ?) AND (? = '' OR instr(url, ?) > 0 OR instr(coalesce(title, ''), ?) > 0) "
                        "ORDER BY rowid LIMIT ? OFFSET ?");
         std::string state = arg(args, "state").value_or(""), q = arg(args, "query").value_or("");
         s.bind(1, int_arg(args, "crawl", 0)).bind(2, state).bind(3, state).bind(4, q).bind(5, q).bind(6, q);
         s.bind(7, std::clamp<int64_t>(int_arg(args, "limit", 100), 1, 1000)).bind(8, std::max<int64_t>(0, int_arg(args, "offset", 0)));
         bj::array out;
         while (s.step()) {
           bj::object p{{"url", s.text(0)}, {"kind", s.text(1)}, {"depth", s.int64(2)}, {"state", s.text(3)}};
           if (!s.is_null(4)) p["status"] = s.int64(4);
           if (!s.is_null(5) && !s.text(5).empty()) p["title"] = s.text(5);
           if (!s.is_null(6)) p["error"] = s.text(6);
           if (!s.is_null(7)) p["fetched_at"] = base::format_utc_ms(s.int64(7));
           out.push_back(std::move(p));
         }
         return ToolResult::ok(std::move(out));
       }});
  mcp_->add_tool(
      {"crawl_records",
       "What a crawl found: schema.org JSON-LD items by their @type (Product, Offer, LocalBusiness, ...) and records from "
       "its extract spec (kind extract), each with the page it came from.",
       object_schema({{"crawl", crawl_prop},
                      {"kind", string_property("Only this kind, like Product or extract.")},
                      {"query", string_property("Only records whose JSON contains this text.")},
                      {"limit", mcp::integer_property("At most this many (default 100, at most 1000).")},
                      {"offset", mcp::integer_property("Skip this many.")}},
                     {"crawl"}),
       [this](const bj::object& args) {
         std::lock_guard lock(web_->db_mu());
         sqlite::Stmt s(web_->db(),
                        "SELECT url, kind, data FROM records WHERE crawl_id = ? AND (? = '' OR kind = ?) "
                        "AND (? = '' OR instr(data, ?) > 0) ORDER BY id LIMIT ? OFFSET ?");
         std::string kind = arg(args, "kind").value_or(""), q = arg(args, "query").value_or("");
         s.bind(1, int_arg(args, "crawl", 0)).bind(2, kind).bind(3, kind).bind(4, q).bind(5, q);
         s.bind(6, std::clamp<int64_t>(int_arg(args, "limit", 100), 1, 1000)).bind(7, std::max<int64_t>(0, int_arg(args, "offset", 0)));
         bj::array out;
         while (s.step()) {
           std::string err;
           auto data = json::parse(s.text(2), &err);
           out.push_back(bj::object{{"url", s.text(0)}, {"kind", s.text(1)}, {"data", data ? *data : bj::value(nullptr)}});
         }
         return ToolResult::ok(std::move(out));
       }});
  mcp_->add_tool({"web_optout",
                  "Adds (or with remove, takes away) a host on the list the robot never fetches, for a site owner who "
                  "asked. Subdomains are included. Lists the whole list either way. Host-wide; a changing tool.",
                  object_schema({{"host", string_property("A host name, like ranch.example.")},
                                 {"remove", mcp::boolean_property("Take it off the list instead.")}},
                                {}),
                  [this](const bj::object& args) {
                    if (auto h = arg(args, "host")) {
                      bool remove = args.contains("remove") && args.at("remove").is_bool() && args.at("remove").as_bool();
                      web_->opt_out(*h, !remove);
                    }
                    bj::array out;
                    for (const auto& h : web_->opted_out()) out.emplace_back(h);
                    return ToolResult::ok(bj::object{{"opted_out", std::move(out)}, {"user_agent", web_->user_agent()}});
                  }});
}

}  // namespace goober::host
