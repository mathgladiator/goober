#pragma once

// The host's polite robot (800.Crawl.md; design/machine/017.Crawl.md), private to the host
// package: every fetch for operators, crawls, and boxes goes through here, so robots.txt,
// pacing per site, public addresses only, caching, and the egress log are in one place.

#include <sys/socket.h>

#include <boost/json.hpp>

#include <climits>

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "goober/config/config.h"
#include "goober/sqlite/sqlite.h"
#include "goober/web/robots.h"
#include "goober/web/url.h"

namespace goober::host {

namespace bj = boost::json;

// Whether an address is on the public internet: not loopback, private, link-local, shared
// (CGNAT), multicast, documentation, or reserved, in IPv4 or IPv6 (mapped addresses too).
bool public_address(const sockaddr* addr);

struct FetchOptions {
  int64_t timeout_ms = 15000;     // the whole fetch, waiting for the site's turn included
  int64_t max_age_ms = 3600000;   // a cached copy younger than this is used as it is
  bool cached_only = false;       // never go out
  int64_t max_wait_ms = INT64_MAX;  // how long to wait for the site's turn; past it, retry_after_ms says when
  std::vector<std::string> hosts;  // a box's allow list ("*", "x.com", "*.x.com"); empty is any
  std::string tenant, box, request_id;  // for the egress log
};

struct Page {
  std::string url;  // the final URL, after redirects
  int status = 0;
  std::string content_type;
  std::string body;  // UTF-8
  int64_t fetched_ms = 0;
  bool from_cache = false;
  std::vector<std::string> redirects;
  std::string error;           // why there is no page; status may still say what the site answered
  int64_t retry_after_ms = 0;  // when the error is "wait your turn"
  bool is_html() const;
};

class WebFetcher {
 public:
  using Log = std::function<void(const FetchOptions&, const std::string& url, int status, const std::string& error,
                                 int64_t us, size_t bytes)>;
  WebFetcher(std::filesystem::path storage, std::function<std::shared_ptr<const config::HostConfig>()> config, Log log);

  Page fetch(const std::string& url, const FetchOptions& options);
  // The User-Agent: goober/<version> (+contact).
  std::string user_agent() const;

  // Hosts that asked not to be crawled, besides the config's web.opt-out.
  bool opt_out(const std::string& host, bool add);
  std::vector<std::string> opted_out();

  // Drops cached pages read more than 30 days ago, and the oldest past 20,000.
  void prune(int64_t now_ms);

  sqlite::Db& db() { return *db_; }
  std::mutex& db_mu() { return db_mu_; }

 private:
  struct Origin {
    int64_t next_ms = 0;  // when this site may be asked again
    std::optional<web::Robots> robots;
    int64_t robots_until_ms = 0;
  };
  struct Raw {
    int status = 0;
    std::map<std::string, std::string> headers;  // lowercase names
    std::string body, error;
    bool truncated = false;
  };
  bool host_allowed(const web::Url& u, const FetchOptions& o, std::string* why);
  // Waits for the origin's turn (if it comes before the deadline) and books the next one.
  bool take_turn(const web::Url& u, int64_t deadline_ms, int64_t max_wait_ms, int64_t* retry_after_ms);
  // The origin's robots.txt, or nullopt when there was no time to ask (retry_after_ms set);
  // only a real answer, or a real failure of the site, is remembered.
  std::optional<web::Robots> robots_for(const web::Url& u, int64_t deadline_ms, const FetchOptions& o,
                                        int64_t* retry_after_ms);
  void forget_idle_origins();  // with mu_ held
  Raw get(const web::Url& u, const std::vector<std::pair<std::string, std::string>>& headers, int64_t timeout_ms,
          size_t max_bytes);
  void back_off(const web::Url& u, int64_t ms);

  std::filesystem::path storage_;
  std::function<std::shared_ptr<const config::HostConfig>()> config_;
  Log log_;
  std::mutex mu_;
  std::map<std::string, Origin> origins_;
  std::mutex db_mu_;
  std::unique_ptr<sqlite::Db> db_;
};

// The views a caller asked for, of one page: {"html", "text", "markdown", "links", "meta",
// "jsonld", "tables", "select", "extract"}. nullopt with `error` for a bad request.
std::optional<bj::object> page_views(const Page& page, const bj::array& views, const std::string& select,
                                      const bj::value& extract, std::string* error);

}  // namespace goober::host
