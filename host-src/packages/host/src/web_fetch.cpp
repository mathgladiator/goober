// The host's polite robot (800.Crawl.md; design/machine/017.Crawl.md).

#include "web_fetch.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <thread>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/process.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/web/html.h"

namespace goober::host {
namespace fs = std::filesystem;

namespace {

constexpr int kMaxRedirects = 5;
constexpr int64_t kRobotsTtlMs = 24LL * 3600 * 1000;
constexpr int64_t kRobotsFailTtlMs = 10LL * 60 * 1000;

const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS pages (
  url TEXT PRIMARY KEY, final_url TEXT NOT NULL, status INTEGER NOT NULL, content_type TEXT NOT NULL,
  etag TEXT, last_modified TEXT, body TEXT NOT NULL, fetched_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS optout (host TEXT PRIMARY KEY, at INTEGER NOT NULL);
)sql";

bool v4_public(uint32_t a) {
  auto in = [&](uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };
  return !(in(0x00000000, 8) || in(0x0A000000, 8) || in(0x64400000, 10) || in(0x7F000000, 8) || in(0xA9FE0000, 16) ||
           in(0xAC100000, 12) || in(0xC0000000, 24) || in(0xC0000200, 24) || in(0xC0A80000, 16) || in(0xC6120000, 15) ||
           in(0xC6336400, 24) || in(0xCB007100, 24) || in(0xE0000000, 4) || in(0xF0000000, 4));
}

bool text_like(std::string_view ct) {
  std::string t = base::to_lower(ct.substr(0, ct.find(';')));
  while (!t.empty() && t.back() == ' ') t.pop_back();
  return t.empty() || t.starts_with("text/") || t == "application/xhtml+xml" || t == "application/json" ||
         t == "application/ld+json" || t == "application/xml" || t == "application/rss+xml" ||
         t == "application/atom+xml" || t.ends_with("+json") || t.ends_with("+xml");
}

// Windows-1252 for 0x80..0x9F; Latin-1 elsewhere.
constexpr uint16_t kCp1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

void put_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

std::string from_latin1(std::string_view s) {
  std::string out;
  out.reserve(s.size() + s.size() / 8);
  for (unsigned char c : s) put_utf8(out, c >= 0x80 && c < 0xA0 ? kCp1252[c - 0x80] : c);
  return out;
}

std::string charset_of(std::string_view content_type, std::string_view body, bool html) {
  auto find_charset = [](std::string_view text) -> std::string {
    std::string l = base::to_lower(text);
    auto at = l.find("charset=");
    if (at == std::string::npos) return "";
    size_t i = at + 8;
    while (i < l.size() && (l[i] == '"' || l[i] == '\'' || l[i] == ' ')) ++i;
    size_t j = i;
    while (j < l.size() && (std::isalnum(static_cast<unsigned char>(l[j])) || l[j] == '-' || l[j] == '_')) ++j;
    return l.substr(i, j - i);
  };
  std::string cs = find_charset(content_type);
  if (cs.empty() && html) cs = find_charset(body.substr(0, 2048));
  return cs;
}

bool header_safe(std::string_view v) {
  for (unsigned char c : v) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  return v.size() < 1024;
}

}  // namespace

bool public_address(const sockaddr* addr) {
  if (addr->sa_family == AF_INET) {
    return v4_public(ntohl(reinterpret_cast<const sockaddr_in*>(addr)->sin_addr.s_addr));
  }
  if (addr->sa_family != AF_INET6) return false;
  const uint8_t* b = reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr.s6_addr;
  static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  if (std::memcmp(b, kMapped, 12) == 0) return v4_public(uint32_t(b[12]) << 24 | uint32_t(b[13]) << 16 | uint32_t(b[14]) << 8 | b[15]);
  bool zero_prefix = true;
  for (int i = 0; i < 12; ++i) zero_prefix &= b[i] == 0;
  if (zero_prefix) return false;                              // ::, ::1, and IPv4-compatible
  if ((b[0] & 0xfe) == 0xfc) return false;                    // fc00::/7 unique local
  if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return false;    // fe80::/10 link-local
  if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return false;    // fec0::/10 site-local
  if (b[0] == 0xff) return false;                             // multicast
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;  // documentation
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0 && b[3] == 0) return false;        // Teredo
  if (b[0] == 0x20 && b[1] == 0x02) return false;                                  // 6to4
  if (b[0] == 0 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) return false;     // NAT64
  if (b[0] == 0x01 && b[1] == 0 && b[2] == 0 && b[3] == 0) return false;           // discard
  return (b[0] & 0xe0) == 0x20;                                                    // 2000::/3, global unicast
}

std::optional<std::string> pick_address(const std::string& host_in, bool local, std::string* why) {
  std::string host = host_in;
  if (host.size() > 2 && host.front() == '[') host = host.substr(1, host.size() - 2);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
    *why = "can't resolve " + host;
    return std::nullopt;
  }
  std::vector<std::string> v4, v6;
  bool any_private = false;
  for (addrinfo* a = res; a; a = a->ai_next) {
    char text[INET6_ADDRSTRLEN] = {};
    const void* raw = a->ai_family == AF_INET ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr)
                                              : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(a->ai_addr)->sin6_addr);
    if (!inet_ntop(a->ai_family, raw, text, sizeof text)) continue;
    if (public_address(a->ai_addr) || local) {
      auto& list = a->ai_family == AF_INET6 ? v6 : v4;
      std::string t = a->ai_family == AF_INET6 ? "[" + std::string(text) + "]" : std::string(text);
      if (std::find(list.begin(), list.end(), t) == list.end()) list.push_back(t);
    } else {
      any_private = true;
    }
  }
  freeaddrinfo(res);
  std::optional<std::string> out;
  v4.insert(v4.end(), v6.begin(), v6.end());
  for (size_t i = 0; i < v4.size() && i < 4; ++i) out = out ? *out + "," + v4[i] : v4[i];
  if (!out) *why = any_private ? host + " resolves only to private or reserved addresses" : "no usable address for " + host;
  return out;
}


bool Page::is_html() const {
  std::string t = base::to_lower(content_type);
  return t.starts_with("text/html") || t.starts_with("application/xhtml");
}

WebFetcher::WebFetcher(fs::path storage, std::function<std::shared_ptr<const config::HostConfig>()> config, Log log)
    : storage_(std::move(storage)), config_(std::move(config)), log_(std::move(log)) {
  std::error_code ec;
  fs::create_directories(storage_ / "web", ec);
  db_ = std::make_unique<sqlite::Db>(storage_ / "web" / "web.db");
  db_->exec(kSchema);
}

std::string WebFetcher::user_agent() const {
  auto cfg = config_();
  std::string contact = cfg->web.contact.empty() ? cfg->base_url() : cfg->web.contact;
  return "goober/" + std::string(kVersion) + " (+" + contact + "; a polite robot that reads robots.txt)";
}

bool WebFetcher::opt_out(const std::string& host, bool add) {
  std::lock_guard lock(db_mu_);
  if (add) {
    sqlite::Stmt s(*db_, "INSERT OR IGNORE INTO optout (host, at) VALUES (?, ?)");
    s.bind(1, base::to_lower(host)).bind(2, base::now_unix_ms()).run();
  } else {
    sqlite::Stmt s(*db_, "DELETE FROM optout WHERE host = ?");
    s.bind(1, base::to_lower(host)).run();
  }
  return db_->changes() > 0;
}

std::vector<std::string> WebFetcher::opted_out() {
  std::vector<std::string> out = config_()->web.opt_out;
  std::lock_guard lock(db_mu_);
  sqlite::Stmt s(*db_, "SELECT host FROM optout ORDER BY host");
  while (s.step()) out.push_back(s.text(0));
  return out;
}

bool WebFetcher::host_allowed(const web::Url& u, const FetchOptions& o, std::string* why) {
  auto cfg = config_();
  // Outside local mode, only the scheme's own port: no reaching odd services on a host.
  if (!cfg->local && u.port != 0) return *why = "only the default ports (80, 443) are fetched", false;
  // By name only: an address would slip past a site's opt-out, which names its host
  // (security review 4, S4-9). Another name for the same server still can; see 017 §8.
  bool literal = u.host.front() == '[' ||
                 std::all_of(u.host.begin(), u.host.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
  if (!cfg->local && literal) return *why = "sites are fetched by name, not by address", false;
  for (const auto& h : opted_out()) {
    if (u.host == h || u.host.ends_with("." + h)) return *why = u.host + " asked not to be crawled", false;
  }
  if (o.hosts.empty()) return true;
  for (const auto& h : o.hosts) {
    if (h == "*") return true;
    if (h.starts_with("*.") && (u.host == h.substr(2) || u.host.ends_with(h.substr(1)))) return true;
    if (u.host == h || u.host == "www." + h) return true;
  }
  return *why = "the manifest's web.hosts doesn't include " + u.host, false;
}

bool WebFetcher::take_turn(const web::Url& u, int64_t deadline_ms, int64_t max_wait_ms, int64_t* retry_after_ms) {
  int64_t wait = 0;
  {
    std::lock_guard lock(mu_);
    auto& o = origins_[u.origin()];
    int64_t delay = std::max<int64_t>(config_()->web.min_delay_ms, o.robots ? o.robots->crawl_delay_ms() : 0);
    int64_t now = base::now_unix_ms();
    wait = std::max<int64_t>(0, o.next_ms - now);
    // Leave a second for the fetch itself, or say when to come back.
    if (now + wait > deadline_ms - 1000 || wait > max_wait_ms) {
      *retry_after_ms = std::max<int64_t>(wait, 1);
      return false;
    }
    o.next_ms = now + wait + delay;  // booked: whoever comes next waits behind us
  }
  if (wait > 0) std::this_thread::sleep_for(std::chrono::milliseconds(wait));
  return true;
}

void WebFetcher::back_off(const web::Url& u, int64_t ms) {
  std::lock_guard lock(mu_);
  auto& o = origins_[u.origin()];
  o.next_ms = std::max(o.next_ms, base::now_unix_ms() + std::min<int64_t>(ms, 3600000));
}

WebFetcher::Raw WebFetcher::get(const web::Url& u, const std::vector<std::pair<std::string, std::string>>& headers,
                                int64_t timeout_ms, size_t max_bytes) {
  Raw out;
  auto cfg = config_();
  if (cfg->sandbox) {
    out.error = "this host is a sandbox: it fetches nothing from the web";
    return out;
  }
  std::string why;
  auto addr = pick_address(u.host, cfg->local, &why);
  if (!addr) {
    out.error = why;
    return out;
  }
  // Fractions of a second count: a 1.9 s budget isn't rounded down to 1.
  int64_t ms = std::max<int64_t>(100, timeout_ms);
  std::string max_time = std::to_string(ms / 1000) + "." + std::to_string(ms % 1000 / 100);
  std::vector<std::string> argv = {"curl", "-q", "-g", "--noproxy", "*", "-sS", "-i", "--compressed", "--max-redirs", "0",
                                   "--proto", "=http,https", "--max-time", max_time};
  bool literal = u.host.front() == '[' || std::all_of(u.host.begin(), u.host.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
  if (!literal) {
    // Connect to the address that was checked, so a second DNS answer can't point elsewhere.
    argv.push_back("--resolve");
    argv.push_back(u.host + ":" + std::to_string(u.effective_port()) + ":" + *addr);
  }
  // Headers and the URL go to curl in a private file: a box's URL can carry a person's data.
  std::string conf = "url = " + base::curl_quoted(u.str()) + "\n";
  for (const auto& [k, v] : headers) {
    if (!header_safe(v)) continue;
    conf += "header = " + base::curl_quoted(k + ": " + v) + "\n";
  }
  auto conf_file = base::write_private_file(cfg->storage / "tmp", ".fetch-", conf);
  if (!conf_file) {
    out.error = "can't prepare the fetch";
    return out;
  }
  argv.push_back("-K");
  argv.push_back(conf_file->string());
  auto r = base::run_process(argv, "", max_bytes + 65536, timeout_ms + 1000);
  std::error_code rm_error;
  fs::remove(*conf_file, rm_error);
  if (!r.started) {
    out.error = "could not run curl";
    return out;
  }
  out.truncated = r.truncated;
  if (r.timed_out) {
    out.error = "the site took too long";
    return out;
  }
  if (r.exit_code != 0 && !r.truncated) {
    std::string msg = r.err;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    out.error = msg.empty() ? "the fetch failed" : msg.substr(0, 300);
    return out;
  }
  std::string_view rest = r.out;
  while (true) {
    auto end = rest.find("\r\n\r\n");
    if (end == std::string_view::npos) {
      out.error = out.truncated ? "the page's headers are too large" : "the reply couldn't be read";
      return out;
    }
    std::string_view head = rest.substr(0, end);
    rest.remove_prefix(end + 4);
    auto sp = head.find(' ');
    out.status = sp == std::string_view::npos ? 0 : std::atoi(std::string(head.substr(sp + 1, 3)).c_str());
    if (out.status >= 200 || out.status == 0) {
      size_t i = head.find("\r\n");
      while (i != std::string_view::npos) {
        size_t next = head.find("\r\n", i + 2);
        std::string_view line = head.substr(i + 2, next == std::string_view::npos ? std::string_view::npos : next - i - 2);
        if (auto colon = line.find(':'); colon != std::string_view::npos) {
          std::string_view v = line.substr(colon + 1);
          while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
          out.headers[base::to_lower(line.substr(0, colon))] = std::string(v);
        }
        i = next;
      }
      break;
    }
  }
  out.body = std::string(rest.substr(0, max_bytes));
  return out;
}

std::optional<web::Robots> WebFetcher::robots_for(const web::Url& u, int64_t deadline_ms, const FetchOptions& o,
                                                  int64_t* retry_after_ms) {
  {
    std::lock_guard lock(mu_);
    auto& origin = origins_[u.origin()];
    if (origin.robots && origin.robots_until_ms > base::now_unix_ms()) return *origin.robots;
  }
  std::optional<web::Robots> robots;
  int64_t ttl = kRobotsFailTtlMs;
  auto at = web::parse_url(u.origin() + "/robots.txt");
  std::string why;
  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    // Every hop obeys the host's rules (opt-out, ports, addresses), but not one box's own
    // allow list: what is remembered here is shared by everyone who reads this site.
    FetchOptions any = o;
    any.hosts.clear();
    if (hop > 0 && !host_allowed(*at, any, &why)) {
      robots = web::Robots::disallow_all();
      break;
    }
    // No time for our turn isn't an answer from the site: nothing is remembered.
    if (!take_turn(*at, deadline_ms, INT64_MAX, retry_after_ms)) return std::nullopt;
    int64_t started = base::monotonic_us();
    auto raw = get(*at, {{"User-Agent", user_agent()}}, std::min<int64_t>(10000, deadline_ms - base::now_unix_ms()),
                   512 << 10);
    if (log_) log_(o, at->str(), raw.status, raw.error, base::monotonic_us() - started, raw.body.size());
    if (!raw.error.empty()) {
      robots = web::Robots::disallow_all();  // unreachable: RFC 9309 says stay out
      break;
    }
    if (raw.status >= 300 && raw.status < 400 && raw.headers.count("location")) {
      auto next = web::resolve(*at, raw.headers["location"]);
      if (!next) {
        robots = web::Robots::allow_all();  // a redirect to nowhere is like a 404
        ttl = kRobotsTtlMs;
        break;
      }
      at = next;
      continue;
    }
    // RFC 9309: a 2xx is the rules, 4xx means none, and 5xx means stay out.
    if (raw.status >= 200 && raw.status < 300) {
      robots = web::Robots::parse(raw.body, "goober");
      ttl = kRobotsTtlMs;
    } else if (raw.status >= 400 && raw.status < 500) {
      robots = web::Robots::allow_all();
      ttl = kRobotsTtlMs;
    } else {
      robots = web::Robots::disallow_all();
    }
    break;
  }
  if (!robots) robots = web::Robots::disallow_all();  // too many redirects
  std::lock_guard lock(mu_);
  forget_idle_origins();
  auto& origin = origins_[u.origin()];
  origin.robots = robots;
  origin.robots_until_ms = base::now_unix_ms() + ttl;
  return robots;
}

void WebFetcher::forget_idle_origins() {
  // Many subdomains of one hostile site must not grow this without bound.
  if (origins_.size() < 10000) return;
  int64_t now = base::now_unix_ms();
  for (auto it = origins_.begin(); it != origins_.end();) {
    bool idle = it->second.next_ms < now && it->second.robots_until_ms < now + kRobotsTtlMs / 2;
    it = idle ? origins_.erase(it) : std::next(it);
  }
  if (origins_.size() >= 10000) origins_.clear();  // still full: start over, re-reading robots.txt
}

void WebFetcher::prune(int64_t now_ms) {
  std::lock_guard lock(db_mu_);
  // Pages read more than 30 days ago go, and the cache never holds more than 20,000 pages.
  sqlite::Stmt old(*db_, "DELETE FROM pages WHERE fetched_ms < ?");
  old.bind(1, now_ms - 30LL * 86400000).run();
  db_->exec("DELETE FROM pages WHERE url IN (SELECT url FROM pages ORDER BY fetched_ms DESC LIMIT -1 OFFSET 20000)");
}

Page WebFetcher::fetch(const std::string& url, const FetchOptions& o) {
  auto cfg = config_();
  Page page;
  int64_t deadline = base::now_unix_ms() + o.timeout_ms;
  auto u = web::parse_url(url);
  if (!u) {
    page.error = "not an http or https URL: " + url.substr(0, 200);
    return page;
  }
  page.url = u->str();
  // The caller's own rules come before the cache: a box can't read, from the cache, a page
  // its manifest wouldn't let it fetch, nor an opted-out site's.
  std::string why;
  if (!host_allowed(*u, o, &why)) {
    page.error = why;
    return page;
  }
  // The cache, keyed by the URL as asked.
  struct Cached {
    Page page;
    std::string etag, last_modified;
  };
  std::optional<Cached> cached;
  {
    std::lock_guard lock(db_mu_);
    sqlite::Stmt s(*db_, "SELECT final_url, status, content_type, etag, last_modified, body, fetched_ms FROM pages WHERE url = ?");
    s.bind(1, u->str());
    if (s.step()) {
      Cached c;
      c.page.url = s.text(0);
      c.page.status = int(s.int64(1));
      c.page.content_type = s.text(2);
      c.etag = s.is_null(3) ? "" : s.text(3);
      c.last_modified = s.is_null(4) ? "" : s.text(4);
      c.page.body = s.text(5);
      c.page.fetched_ms = s.int64(6);
      c.page.from_cache = true;
      cached = std::move(c);
    }
  }
  // Where the cached page ended up after redirects must pass the caller's rules too.
  if (cached) {
    auto final_url = web::parse_url(cached->page.url);
    if (!final_url || !host_allowed(*final_url, o, &why)) {
      page.error = final_url ? why : "the cached page can't be read";
      return page;
    }
  }
  if (cached && (o.cached_only || base::now_unix_ms() - cached->page.fetched_ms < o.max_age_ms)) return cached->page;
  if (o.cached_only) {
    page.error = "not fetched yet";
    return page;
  }
  web::Url at = *u;
  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    if (!host_allowed(at, o, &why)) {
      page.error = why;
      return page;
    }
    auto robots = robots_for(at, deadline, o, &page.retry_after_ms);
    if (!robots) {
      page.error = "the robot is pacing itself on " + at.host + "; ask again in " +
                   std::to_string((page.retry_after_ms + 999) / 1000) + " s";
      return page;
    }
    if (!robots->allowed(at.target())) {
      page.error = "robots.txt at " + at.origin() + " doesn't allow " + at.target().substr(0, 200);
      return page;
    }
    // Only the first request may decline to wait: a redirect already under way follows through.
    if (!take_turn(at, deadline, hop == 0 ? o.max_wait_ms : INT64_MAX, &page.retry_after_ms)) {
      page.error = "the robot is pacing itself on " + at.host + "; ask again in " +
                   std::to_string((page.retry_after_ms + 999) / 1000) + " s";
      return page;
    }
    std::vector<std::pair<std::string, std::string>> headers = {
        {"User-Agent", user_agent()},
        {"Accept", "text/html,application/xhtml+xml,application/json;q=0.9,text/plain;q=0.8,*/*;q=0.1"},
        {"Accept-Language", "en"}};
    if (hop == 0 && cached && cached->page.status >= 200 && cached->page.status < 300) {
      if (!cached->etag.empty()) headers.emplace_back("If-None-Match", cached->etag);
      if (!cached->last_modified.empty()) headers.emplace_back("If-Modified-Since", cached->last_modified);
    }
    int64_t started = base::monotonic_us();
    int64_t left = deadline - base::now_unix_ms();
    auto raw = get(at, headers, std::max<int64_t>(1000, left), size_t(cfg->web.max_page_bytes));
    if (log_) log_(o, at.str(), raw.status, raw.error, base::monotonic_us() - started, raw.body.size());
    if (!raw.error.empty()) {
      page.error = raw.error;
      return page;
    }
    if (raw.status == 304 && cached) {
      // Unchanged: the copy we have is current again, and cost the site almost nothing.
      std::lock_guard lock(db_mu_);
      sqlite::Stmt s(*db_, "UPDATE pages SET fetched_ms = ? WHERE url = ?");
      s.bind(1, base::now_unix_ms()).bind(2, u->str()).run();
      Page p = cached->page;
      p.fetched_ms = base::now_unix_ms();
      return p;
    }
    if (raw.status >= 300 && raw.status < 400 && raw.headers.count("location")) {
      auto next = web::resolve(at, raw.headers["location"]);
      if (!next) {
        page.status = raw.status;
        page.error = "a redirect to something that isn't an http(s) URL";
        return page;
      }
      page.redirects.push_back(at.str());
      at = *next;
      continue;
    }
    page.url = at.str();
    page.status = raw.status;
    page.fetched_ms = base::now_unix_ms();
    page.content_type = raw.headers.count("content-type") ? raw.headers["content-type"] : "";
    if (raw.status == 429 || raw.status == 503) {
      int64_t after = 60000;
      if (auto it = raw.headers.find("retry-after"); it != raw.headers.end()) {
        int64_t s = std::atoll(it->second.substr(0, 9).c_str());
        if (s > 0) after = std::min<int64_t>(s, 3600) * 1000;
      }
      back_off(at, after);
      page.error = "the site asked us to slow down";
      page.retry_after_ms = after;
      return page;
    }
    if (raw.truncated) {
      page.error = "the page is larger than " + std::to_string(cfg->web.max_page_bytes) + " bytes";
      return page;
    }
    if (!text_like(page.content_type)) {
      page.error = "not a page: " + page.content_type.substr(0, 100);
      return page;
    }
    std::string cs = charset_of(page.content_type, raw.body, page.is_html() || page.content_type.empty());
    page.body = (cs == "iso-8859-1" || cs == "latin1" || cs == "windows-1252" || cs == "cp1252") ? from_latin1(raw.body)
                                                                                              : base::valid_utf8(raw.body);
    {
      std::lock_guard lock(db_mu_);
      sqlite::Stmt s(*db_,
                     "INSERT INTO pages (url, final_url, status, content_type, etag, last_modified, body, fetched_ms) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (url) DO UPDATE SET final_url = excluded.final_url, "
                     "status = excluded.status, content_type = excluded.content_type, etag = excluded.etag, "
                     "last_modified = excluded.last_modified, body = excluded.body, fetched_ms = excluded.fetched_ms");
      s.bind(1, u->str()).bind(2, page.url).bind(3, int64_t(page.status)).bind(4, page.content_type);
      raw.headers.count("etag") ? s.bind(5, raw.headers["etag"]) : s.bind_null(5);
      raw.headers.count("last-modified") ? s.bind(6, raw.headers["last-modified"]) : s.bind_null(6);
      s.bind(7, page.body).bind(8, page.fetched_ms).run();
    }
    return page;
  }
  page.error = "more than " + std::to_string(kMaxRedirects) + " redirects";
  return page;
}

std::optional<bj::object> page_views(const Page& page, const bj::array& views_in, const std::string& select,
                                      const bj::value& extract, std::string* error) {
  bj::array views = views_in;
  if (views.empty() && select.empty() && extract.is_null()) {
    views = page.is_html() ? bj::array{"meta", "markdown"} : bj::array{"text"};
  }
  bj::object out;
  std::optional<web::Document> doc;
  auto html = [&]() -> const web::Document& {
    if (!doc) doc = web::Document::parse(page.is_html() ? page.body : "");
    return *doc;
  };
  auto page_url = web::parse_url(page.url);
  web::Url base = page_url ? html().base_for(*page_url) : web::Url{};
  for (const auto& v : views) {
    std::string name = v.is_string() ? std::string(v.as_string()) : "";
    if (name == "html") {
      out["html"] = page.body;
    } else if (name == "text") {
      out["text"] = page.is_html() ? web::view_text(html()) : page.body;
    } else if (name == "json") {
      std::string err;
      auto j = json::parse(page.body, &err);
      out["json"] = j ? *j : bj::value(nullptr);
    } else if (name == "markdown") {
      out["markdown"] = page.is_html() ? web::view_markdown(html(), base) : page.body;
    } else if (name == "links") {
      out["links"] = web::view_links(html(), base);
    } else if (name == "meta") {
      out["meta"] = web::view_meta(html(), base);
    } else if (name == "jsonld") {
      out["jsonld"] = web::view_jsonld(html());
    } else if (name == "tables") {
      out["tables"] = web::view_tables(html());
    } else {
      *error = "unknown view \"" + name + "\": html, text, markdown, links, meta, jsonld, tables, or json";
      return std::nullopt;
    }
  }
  if (!select.empty()) {
    auto r = web::view_select(html(), select, error);
    if (!r) return std::nullopt;
    out["select"] = std::move(*r);
  }
  if (!extract.is_null()) {
    auto r = web::view_extract(html(), base, extract, error);
    if (!r) return std::nullopt;
    out["extract"] = std::move(*r);
  }
  return out;
}

}  // namespace goober::host
