#include "goober/operations/traffic.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "goober/base/time.h"
#include "goober/sqlite/sqlite.h"

namespace goober::operations {

namespace fs = std::filesystem;

namespace {

constexpr int64_t kMinuteMs = 60 * 1000;
constexpr int64_t kHourMs = 60 * kMinuteMs;
// One resolution (045.Limits.md): five-minute buckets, kept seven days. Nothing longer.
constexpr int64_t kBucketMs = 5 * kMinuteMs;
constexpr int64_t kKeptMs = 7 * 24 * kHourMs;
// The most bytes one pass reads from a log before it commits, so a first start over a
// large history stays within memory.
constexpr size_t kChunkBytes = 8u << 20;
constexpr int kRotationsKept = 64;  // logs::kKeepFiles (032 § 2)

std::string text(const bj::object& o, std::string_view k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

int64_t number(const bj::object& o, std::string_view k) {
  auto* v = o.if_contains(k);
  if (!v) return 0;
  if (v->is_int64()) return v->as_int64();
  if (v->is_uint64()) return int64_t(v->as_uint64());
  if (v->is_double()) return int64_t(v->as_double());
  return 0;
}

bool starts(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}

// One row's sums, for either table.
struct Sums {
  int64_t n = 0, n3 = 0, n4 = 0, n5 = 0, us = 0, us_max = 0, fuel = 0, sql = 0, outbox = 0;
  Histogram hist;
  void add_status(int64_t status) {
    ++n;
    if (status >= 300 && status < 400) ++n3;
    if (status >= 400 && status < 500) ++n4;
    if (status >= 500) ++n5;
  }
  void merge(const Sums& o) {
    n += o.n, n3 += o.n3, n4 += o.n4, n5 += o.n5, us += o.us, fuel += o.fuel, sql += o.sql, outbox += o.outbox;
    us_max = std::max(us_max, o.us_max);
    hist.merge(o.hist);
  }
};

struct EdgeSums {
  int64_t n = 0, errors = 0, us = 0, bytes_out = 0, bytes_in = 0;
  void merge(const EdgeSums& o) {
    n += o.n, errors += o.errors, us += o.us, bytes_out += o.bytes_out, bytes_in += o.bytes_in;
  }
};

using RowKey = std::tuple<int64_t, std::string, std::string, std::string>;  // t, tenant, box, route
using EdgeKey = std::tuple<int64_t, std::string, std::string, std::string, std::string>;

// t, tenant, status, place, box, route, error_by, ref
using ErrorKey = std::tuple<int64_t, std::string, int64_t, std::string, std::string, std::string, std::string, std::string>;

// What went out to one service method (028 § 4.4).
struct SvcSums {
  int64_t n = 0, failed = 0, n4 = 0, n5 = 0, neterr = 0, outbox = 0, us = 0, bytes_out = 0, bytes_in = 0;
  Histogram hist;
  void merge(const SvcSums& o) {
    n += o.n, failed += o.failed, n4 += o.n4, n5 += o.n5, neterr += o.neterr, outbox += o.outbox, us += o.us;
    bytes_out += o.bytes_out, bytes_in += o.bytes_in;
    hist.merge(o.hist);
  }
};
// t, tenant, box, service, method
using SvcKey = std::tuple<int64_t, std::string, std::string, std::string, std::string>;
// t, tenant, service, status
using SvcStatusKey = std::tuple<int64_t, std::string, std::string, int64_t>;
struct SvcFailure {
  int64_t at_ms = 0, status = 0, outbox_id = 0;
  std::string tenant, box, service, method, error, url, request_id;
};
constexpr int64_t kServiceFailuresKeptMs = kKeptMs;
constexpr int kServiceFailuresPerService = 100;

struct Batch {
  std::map<RowKey, Sums> rows;
  std::map<EdgeKey, EdgeSums> edges;
  std::map<ErrorKey, int64_t> errors;
  std::map<SvcKey, SvcSums> services;
  std::map<SvcStatusKey, int64_t> svc_statuses;
  std::vector<SvcFailure> svc_failures;
};

// One call out: counted under its five minutes, and kept when it failed.
void add_service(Batch& b, int64_t at, const std::string& tenant, const std::string& box, const std::string& service,
                 const std::string& method, int64_t status, const std::string& error, int64_t us, int64_t bytes_out,
                 int64_t bytes_in, bool outbox, const std::string& url, const std::string& request_id, int64_t outbox_id) {
  SvcSums s;
  s.n = 1;
  bool failed = status == 0 || status >= 400 || !error.empty();
  s.failed = failed;
  s.n4 = status >= 400 && status < 500;
  s.n5 = status >= 500;
  s.neterr = status == 0 || (status < 400 && !error.empty());
  s.outbox = outbox;
  s.us = std::max<int64_t>(0, us);
  s.bytes_out = bytes_out;
  s.bytes_in = bytes_in;
  s.hist.add(s.us);
  int64_t bucket = at / kBucketMs * kBucketMs;
  b.services[{bucket, tenant, box, service, method}].merge(s);
  if (!failed) return;
  b.svc_statuses[{bucket, tenant, service, status}] += 1;
  b.svc_failures.push_back(SvcFailure{at, status, outbox_id, tenant, box, service, method, error.substr(0, 300),
                                      url.substr(0, 200), request_id});
}

// Where an error happened, in as few names as the site has places (028 § 4.2).
std::string error_place(const bj::object& line, const LineKey& key, int64_t status) {
  if (!key.box.empty()) return "";
  if (status == 404 && !line.contains("file")) return "(not found)";
  std::string path = text(line, "path");
  // A page that exists (a gate refused it, or it failed to render): its path.
  if (line.contains("file") && path.size() <= 200) return path;
  if (key.route != "page" && key.route != "unknown") return key.route;
  return "(other)";
}

void add_access(Batch& b, const bj::object& line, const TenantOf& tenant_of) {
  auto key = key_line(line, tenant_of);
  if (!key) return;
  Sums s;
  int64_t us = std::max<int64_t>(0, number(line, "us"));
  s.add_status(number(line, "status"));
  s.us = us;
  s.us_max = us;
  s.fuel = number(line, "fuel");
  s.sql = number(line, "sql");
  s.outbox = number(line, "outbox");
  s.hist.add(us);
  int64_t bucket = key->at_ms / kBucketMs * kBucketMs;
  b.rows[{bucket, key->tenant, key->box, key->route}].merge(s);
  // An error from outside; a box-to-box call's is the calling box's business (box_failures).
  int64_t status = number(line, "status");
  std::string internal = text(line, "internal");
  if (status >= 400 && internal.empty()) {
    std::string by = text(line, "error_by");
    if (by.empty()) by = line.contains("outcome") ? (status >= 500 ? "box_failed" : "box") : "host";
    std::string ref = text(line, "ref");
    if (ref != "same" && ref != "other") ref = "none";
    std::string place = error_place(line, *key, status);
    std::string route = key->box.empty() ? "" : key->route;
    b.errors[{bucket, key->tenant, status, place, key->box, route, by, ref}] += 1;
  }
  // A box-to-box call: "a -> b" (039).
  if (auto arrow = internal.find(" -> "); arrow != std::string::npos) {
    EdgeSums e{1, number(line, "status") >= 500 ? 1 : 0, us, 0, 0};
    std::string from = internal.substr(0, arrow), to = internal.substr(arrow + 4);
    b.edges[{bucket, key->tenant, from, "box", to}].merge(e);
  }
}

void add_egress(Batch& b, const bj::object& line) {
  auto at = parse_utc_ms(text(line, "at"));
  if (!at) return;
  int64_t status = number(line, "status");
  EdgeSums e{1, (status == 0 || status >= 400 || line.contains("error")) ? 1 : 0, number(line, "us"),
             number(line, "bytes_out"), number(line, "bytes_in")};
  int64_t bucket = *at / kBucketMs * kBucketMs;
  std::string tenant = text(line, "tenant"), box = text(line, "box"), service = text(line, "service");
  b.edges[{bucket, tenant, box, "service", service}].merge(e);
  int64_t outbox_id = number(line, "outbox_id");
  add_service(b, *at, tenant, box, service, text(line, "method"), status, text(line, "error"), number(line, "us"),
              number(line, "bytes_out"), number(line, "bytes_in"), outbox_id != 0, text(line, "url"),
              text(line, "request_id"), outbox_id);
}

// The mailer's line for every mail it sent (logs/mail.log): service "mailer", method the mailer
// (ses, smtp, dev). A box's mail names its tenant; the host's own (sign-in codes, invites) its
// realm, and is from "host" rather than "boxes".
void add_mail(Batch& b, const bj::object& line) {
  auto at = parse_utc_ms(text(line, "at"));
  if (!at) return;
  std::string tenant = text(line, "tenant"), from = tenant.empty() ? "host" : "boxes";
  if (tenant.empty()) tenant = text(line, "realm");
  std::string via = text(line, "via");
  if (via.empty()) via = "?";
  bool sent = text(line, "status") == "sent";
  std::string error = text(line, "error");
  if (!sent && error.empty()) error = "not sent";
  add_service(b, *at, tenant, from, "mailer", via, sent ? 250 : 0, sent ? "" : error, number(line, "us"), 0, 0, false, "", "",
              0);
}

std::optional<std::pair<uint64_t, uint64_t>> inode_and_size(const fs::path& p) {
  struct stat st {};
  if (::stat(p.c_str(), &st) != 0) return std::nullopt;
  return std::make_pair(uint64_t(st.st_ino), uint64_t(st.st_size));
}

// Up to kChunkBytes of whole lines from `offset`; `*next` is where the next read starts.
std::string read_lines(const fs::path& p, uint64_t offset, uint64_t* next) {
  *next = offset;
  std::ifstream in(p, std::ios::binary);
  if (!in) return "";
  in.seekg(std::streamoff(offset));
  std::string data(kChunkBytes, '\0');
  in.read(data.data(), std::streamsize(data.size()));
  data.resize(size_t(in.gcount()));
  auto last = data.rfind('\n');
  if (last == std::string::npos) return "";
  data.resize(last + 1);
  *next = offset + data.size();
  return data;
}

}  // namespace

int Histogram::bucket(int64_t us) {
  for (int b = 0; b < kBuckets - 1; ++b) {
    if (us <= upper(b)) return b;
  }
  return kBuckets - 1;
}

int64_t Histogram::upper(int b) {
  int64_t whole = int64_t(32) << (b / 2);
  return b % 2 ? int64_t(std::llround(double(whole) * 1.4142135623730951)) : whole;
}

void Histogram::merge(const Histogram& other) {
  for (int b = 0; b < kBuckets; ++b) counts[b] += other.counts[b];
}

int64_t Histogram::total() const {
  int64_t t = 0;
  for (auto c : counts) t += c;
  return t;
}

int64_t Histogram::percentile(double q) const {
  int64_t t = total();
  if (t == 0) return 0;
  int64_t target = std::max<int64_t>(1, int64_t(std::ceil(q * double(t))));
  int64_t seen = 0;
  for (int b = 0; b < kBuckets; ++b) {
    seen += counts[b];
    if (seen >= target) return upper(b);
  }
  return upper(kBuckets - 1);
}

std::string Histogram::encode() const {
  int last = kBuckets - 1;
  while (last >= 0 && counts[last] == 0) --last;
  std::string out;
  for (int b = 0; b <= last; ++b) {
    if (b) out += ',';
    out += std::to_string(counts[b]);
  }
  return out;
}

Histogram Histogram::decode(std::string_view s) {
  Histogram h;
  int b = 0;
  size_t i = 0;
  while (i < s.size() && b < kBuckets) {
    size_t end = s.find(',', i);
    if (end == std::string_view::npos) end = s.size();
    int64_t v = 0;
    for (size_t j = i; j < end; ++j) {
      if (s[j] < '0' || s[j] > '9') break;
      v = v * 10 + (s[j] - '0');
    }
    h.counts[b++] = v;
    i = end + 1;
  }
  return h;
}

std::optional<int64_t> parse_utc_ms(std::string_view t) {
  // YYYY-MM-DDTHH:MM:SS(.mmm)Z
  if (t.size() < 20 || t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' || t[16] != ':') return std::nullopt;
  auto num = [&](size_t at, size_t len) -> int64_t {
    int64_t v = 0;
    for (size_t i = at; i < at + len; ++i) {
      if (t[i] < '0' || t[i] > '9') return -1;
      v = v * 10 + (t[i] - '0');
    }
    return v;
  };
  int64_t y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2), s = num(17, 2);
  if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) {
    return std::nullopt;
  }
  int64_t ms = 0;
  if (t.size() >= 24 && t[19] == '.') {
    ms = num(20, 3);
    if (ms < 0) return std::nullopt;
  }
  return ((days_from_civil(y, unsigned(mo), unsigned(d)) * 24 + h) * 60 + mi) * 60000 + s * 1000 + ms;
}

std::optional<LineKey> key_line(const bj::object& line, const TenantOf& tenant_of) {
  // A route_try (an operator's try of a box version) isn't traffic: no one visited.
  if (line.contains("try")) return std::nullopt;
  auto at = parse_utc_ms(text(line, "at"));
  if (!at) return std::nullopt;
  LineKey k;
  k.at_ms = *at;
  std::string domain = text(line, "domain");
  std::optional<std::string> domain_tenant = tenant_of ? tenant_of(domain) : std::nullopt;
  k.tenant = text(line, "tenant");
  if (k.tenant.empty() && domain_tenant) k.tenant = *domain_tenant;
  k.box = text(line, "box");
  if (!k.box.empty()) {
    k.route = text(line, "route");
    if (k.route.empty()) k.route = "?";
    return k;
  }
  std::string path = text(line, "path");
  if (line.contains("panel")) {
    k.route = "panel";
  } else if (line.contains("operations") || starts(path, "/goober-operations-mcp")) {
    k.route = "operations";
  } else if (path == "/internal/routes/mta/stalwart" || line.contains("mail_in")) {
    k.route = "mail_in";
  } else if (path == "/goober/health") {
    k.route = "health";
  } else if (!domain_tenant && k.tenant.empty()) {
    k.route = "unknown";
  } else if (starts(path, "/identity/")) {
    k.route = "identity";
  } else if (path == "/oauth" || starts(path, "/oauth/") || starts(path, "/.well-known/")) {
    k.route = "oauth";
  } else if (path == "/attachment" || starts(path, "/attachment/")) {
    k.route = "attachment";
  } else if (path == "/mcp") {
    k.route = "mcp";
  } else if (starts(path, "/goober/unsubscribe/")) {
    k.route = "unsubscribe";
  } else {
    k.route = "page";
  }
  return k;
}

Traffic::Traffic(fs::path db_file, fs::path logs, TenantOf tenant_of)
    : logs_(std::move(logs)), tenant_of_(std::move(tenant_of)) {
  fs::create_directories(db_file.parent_path());
  db_ = std::make_unique<sqlite::Db>(db_file);
  // Derived from the logs, which stay on disk: a lost row on power loss is recounted from
  // the cursor, which commits with it.
  db_->exec("PRAGMA synchronous=NORMAL;");
  // Before 2026-10-31 there were minute rows kept 48 hours and hour rows kept 400 days. They
  // go, with the cursors, so the tailer counts the logs still on disk again into five-minute
  // buckets; the gauges, which no log holds, are carried over at five minutes.
  bool old = false;
  {
    sqlite::Stmt q(*db_, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = 'minutes'");
    old = q.step() && q.int64(0) > 0;
  }
  // Space a delete frees goes back to the disk as it goes (045.Limits.md); a new file takes it
  // at once, an old one with the VACUUM below.
  db_->exec("PRAGMA auto_vacuum = INCREMENTAL");
  db_->exec(
      "CREATE TABLE IF NOT EXISTS route_5m (t INTEGER NOT NULL, tenant TEXT NOT NULL, box TEXT NOT NULL, route TEXT NOT NULL,"
      " n INTEGER NOT NULL, n3 INTEGER NOT NULL, n4 INTEGER NOT NULL, n5 INTEGER NOT NULL,"
      " us INTEGER NOT NULL, us_max INTEGER NOT NULL, fuel INTEGER NOT NULL, sql INTEGER NOT NULL,"
      " outbox INTEGER NOT NULL, hist TEXT NOT NULL, PRIMARY KEY (t, tenant, box, route))");
  db_->exec(
      "CREATE TABLE IF NOT EXISTS edge_5m (t INTEGER NOT NULL, tenant TEXT NOT NULL, from_box TEXT NOT NULL,"
      " to_kind TEXT NOT NULL, to_name TEXT NOT NULL, n INTEGER NOT NULL, errors INTEGER NOT NULL, us INTEGER NOT NULL,"
      " bytes_out INTEGER NOT NULL, bytes_in INTEGER NOT NULL, PRIMARY KEY (t, tenant, from_box, to_kind, to_name))");
  db_->exec(
      "CREATE TABLE IF NOT EXISTS error_5m (t INTEGER NOT NULL, tenant TEXT NOT NULL, status INTEGER NOT NULL,"
      " place TEXT NOT NULL, box TEXT NOT NULL, route TEXT NOT NULL, error_by TEXT NOT NULL, ref TEXT NOT NULL,"
      " n INTEGER NOT NULL, PRIMARY KEY (t, tenant, status, place, box, route, error_by, ref))");
  db_->exec(
      "CREATE TABLE IF NOT EXISTS service_5m (t INTEGER NOT NULL, tenant TEXT NOT NULL, box TEXT NOT NULL,"
      " service TEXT NOT NULL, method TEXT NOT NULL, n INTEGER NOT NULL, failed INTEGER NOT NULL, n4 INTEGER NOT NULL,"
      " n5 INTEGER NOT NULL, neterr INTEGER NOT NULL, outbox INTEGER NOT NULL, us INTEGER NOT NULL,"
      " bytes_out INTEGER NOT NULL, bytes_in INTEGER NOT NULL, hist TEXT NOT NULL,"
      " PRIMARY KEY (t, tenant, box, service, method))");
  db_->exec(
      "CREATE TABLE IF NOT EXISTS service_status_5m (t INTEGER NOT NULL, tenant TEXT NOT NULL, service TEXT NOT NULL,"
      " status INTEGER NOT NULL, n INTEGER NOT NULL, PRIMARY KEY (t, tenant, service, status))");
  db_->exec(
      "CREATE TABLE IF NOT EXISTS gauge_5m (t INTEGER NOT NULL, name TEXT NOT NULL, tenant TEXT NOT NULL,"
      " key TEXT NOT NULL, value INTEGER NOT NULL, PRIMARY KEY (t, name, tenant, key))");
  db_->exec(
      "CREATE TABLE IF NOT EXISTS service_failures (at_ms INTEGER NOT NULL, tenant TEXT NOT NULL, box TEXT NOT NULL,"
      " service TEXT NOT NULL, method TEXT NOT NULL, status INTEGER NOT NULL, error TEXT NOT NULL, url TEXT NOT NULL,"
      " request_id TEXT NOT NULL, outbox_id INTEGER NOT NULL)");
  db_->exec("CREATE INDEX IF NOT EXISTS service_failures_at ON service_failures (at_ms)");
  db_->exec(
      "CREATE TABLE IF NOT EXISTS cursors (name TEXT PRIMARY KEY, inode INTEGER NOT NULL, offset INTEGER NOT NULL)");
  // Log volume (032 § 10): five-minute rows by who wrote, and a day's total per log for 90 days.
  db_->exec(
      "CREATE TABLE IF NOT EXISTS log_5m (t INTEGER NOT NULL, log TEXT NOT NULL, tenant TEXT NOT NULL, box TEXT NOT NULL,"
      " route TEXT NOT NULL, bytes INTEGER NOT NULL, lines INTEGER NOT NULL, PRIMARY KEY (t, log, tenant, box, route))");
  db_->exec(
      "CREATE TABLE IF NOT EXISTS log_day (day INTEGER NOT NULL, log TEXT NOT NULL, bytes INTEGER NOT NULL,"
      " lines INTEGER NOT NULL, counted INTEGER NOT NULL, PRIMARY KEY (day, log))");
  // Per-domain controls (047 § 6): one row per control per five minutes.
  db_->exec(
      "CREATE TABLE IF NOT EXISTS control_5m (t INTEGER NOT NULL, tenant TEXT NOT NULL, domain TEXT NOT NULL,"
      " control TEXT NOT NULL, matched INTEGER NOT NULL, allowed INTEGER NOT NULL, rate INTEGER NOT NULL,"
      " concurrent INTEGER NOT NULL, body INTEGER NOT NULL, closed INTEGER NOT NULL, would INTEGER NOT NULL,"
      " n5 INTEGER NOT NULL, peak INTEGER NOT NULL, clients TEXT NOT NULL, would_clients TEXT NOT NULL,"
      " top_client TEXT NOT NULL, top_n INTEGER NOT NULL, would_top TEXT NOT NULL, would_top_n INTEGER NOT NULL,"
      " hist TEXT NOT NULL, PRIMARY KEY (domain, control, t))");
  // The MCP queue (047 § 14): one row per queue, domain, and class of agent per five minutes.
  db_->exec(
      "CREATE TABLE IF NOT EXISTS mcp_queue_5m (t INTEGER NOT NULL, queue TEXT NOT NULL, tenant TEXT NOT NULL,"
      " domain TEXT NOT NULL, cls TEXT NOT NULL, offered INTEGER NOT NULL, queued INTEGER NOT NULL, ran INTEGER NOT NULL,"
      " refused_rate INTEGER NOT NULL, refused_full INTEGER NOT NULL, refused_wait INTEGER NOT NULL, gone INTEGER NOT NULL,"
      " peak_waiting INTEGER NOT NULL, peak_running INTEGER NOT NULL, waited TEXT NOT NULL,"
      " PRIMARY KEY (queue, domain, cls, t))");
  if (old) {
    {
      sqlite::Stmt q(*db_, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = 'gauge_minutes'");
      if (q.step() && q.int64(0) > 0) {
        std::string b = std::to_string(kBucketMs);
        db_->exec("INSERT OR REPLACE INTO gauge_5m SELECT t / " + b + " * " + b +
                  ", name, tenant, key, max(value) FROM gauge_minutes GROUP BY 1, 2, 3, 4");
      }
    }
    for (const char* t : {"minutes", "hours", "edge_minutes", "edge_hours", "error_minutes", "error_hours", "service_minutes",
                          "service_hours", "service_status_minutes", "service_status_hours", "gauge_minutes", "gauge_hours"}) {
      db_->exec(std::string("DROP TABLE IF EXISTS ") + t);
    }
    db_->exec("DELETE FROM cursors");
    db_->exec("DELETE FROM service_failures");
    db_->exec("PRAGMA wal_checkpoint(TRUNCATE)");
    db_->exec("VACUUM");
  }
  // The write-ahead log is cut back to this after a checkpoint, not left at its high mark.
  db_->exec("PRAGMA journal_size_limit = 1048576");
}

Traffic::~Traffic() = default;

size_t Traffic::catch_up(int64_t now_ms) {
  std::lock_guard lock(mu_);
  size_t lines = tail("access", now_ms) + tail("egress", now_ms) + tail("mail", now_ms);
  flush_gauges();
  flush_logs();
  flush_controls();
  flush_mcp_queue();
  if (now_ms - last_drop_ms_ > 10 * kMinuteMs) {
    drop_old(now_ms);
  } else if (service_failures_added_) {
    trim_service_failures(now_ms);
  }
  service_failures_added_ = false;
  return lines;
}

size_t Traffic::tail(const std::string& name, int64_t) {
  fs::path current = logs_ / (name + ".log");
  uint64_t inode = 0, offset = 0;
  bool known = false;
  {
    sqlite::Stmt s(*db_, "SELECT inode, offset FROM cursors WHERE name = ?");
    s.bind(1, name);
    if (s.step()) {
      inode = uint64_t(s.int64(0));
      offset = uint64_t(s.int64(1));
      known = true;
    }
  }
  // Which files to read, in order, from where: what's left of the file the cursor was in,
  // then the current one.
  std::vector<std::pair<fs::path, uint64_t>> plan;
  auto cur = inode_and_size(current);
  if (!known) {
    // First start: the whole history on disk, oldest first.
    for (int i = kRotationsKept; i >= 1; --i) {
      fs::path old = current.string() + "." + std::to_string(i);
      if (inode_and_size(old)) plan.emplace_back(old, 0);
    }
    if (cur) plan.emplace_back(current, 0);
  } else if (cur && cur->first == inode) {
    plan.emplace_back(current, cur->second < offset ? 0 : offset);  // truncated: from the start
  } else {
    // Rotated: the cursor's file is now a .N, which is finished from the offset.
    for (int i = 1; i <= kRotationsKept; ++i) {
      fs::path old = current.string() + "." + std::to_string(i);
      if (auto o = inode_and_size(old); o && o->first == inode) {
        plan.emplace_back(old, offset);
        // Anything rotated since then, newest last.
        for (int j = i - 1; j >= 1; --j) plan.emplace_back(current.string() + "." + std::to_string(j), 0);
        break;
      }
    }
    if (cur) plan.emplace_back(current, 0);
  }

  size_t lines = 0;
  for (const auto& [file, from] : plan) {
    auto id = inode_and_size(file);
    if (!id) continue;
    uint64_t at = from;
    while (true) {
      uint64_t next = at;
      std::string data = read_lines(file, at, &next);
      Batch batch;
      size_t i = 0;
      while (i < data.size()) {
        size_t end = data.find('\n', i);
        std::string_view one(data.data() + i, end - i);
        i = end + 1;
        std::string err;
        auto v = json::parse(one, &err);
        if (!v || !v->is_object()) continue;
        ++lines;
        if (name == "access") {
          add_access(batch, v->as_object(), tenant_of_);
        } else if (name == "mail") {
          add_mail(batch, v->as_object());
        } else {
          add_egress(batch, v->as_object());
        }
      }
      db_->exec("BEGIN IMMEDIATE");
      try {
        for (const auto& [k, sums] : batch.rows) {
          auto [t, tenant, box, route] = k;
          const char* table = "route_5m";
          int64_t when = t;
          Sums total = sums;
          {
            sqlite::Stmt s(*db_, std::string("SELECT n, n3, n4, n5, us, us_max, fuel, sql, outbox, hist FROM ") + table +
                                     " WHERE t = ? AND tenant = ? AND box = ? AND route = ?");
            s.bind(1, when).bind(2, tenant).bind(3, box).bind(4, route);
            if (s.step()) {
              Sums old{s.int64(0), s.int64(1), s.int64(2), s.int64(3), s.int64(4),
                       s.int64(5), s.int64(6), s.int64(7), s.int64(8), Histogram::decode(s.text(9))};
              total.merge(old);
            }
          }
          sqlite::Stmt w(*db_, std::string("INSERT OR REPLACE INTO ") + table +
                                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
          w.bind(1, when).bind(2, tenant).bind(3, box).bind(4, route).bind(5, total.n).bind(6, total.n3);
          w.bind(7, total.n4).bind(8, total.n5).bind(9, total.us).bind(10, total.us_max).bind(11, total.fuel);
          w.bind(12, total.sql).bind(13, total.outbox).bind(14, total.hist.encode());
          w.run();
        }
        for (const auto& [k, e] : batch.edges) {
          auto [t, tenant, from_box, to_kind, to_name] = k;
          const char* table = "edge_5m";
          int64_t when = t;
          sqlite::Stmt w(*db_, std::string("INSERT INTO ") + table +
                                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (t, tenant, from_box, to_kind, "
                                   "to_name) DO UPDATE SET n = n + excluded.n, errors = errors + excluded.errors, "
                                   "us = us + excluded.us, bytes_out = bytes_out + excluded.bytes_out, "
                                   "bytes_in = bytes_in + excluded.bytes_in");
          w.bind(1, when).bind(2, tenant).bind(3, from_box).bind(4, to_kind).bind(5, to_name).bind(6, e.n);
          w.bind(7, e.errors).bind(8, e.us).bind(9, e.bytes_out).bind(10, e.bytes_in);
          w.run();
        }
        for (const auto& [k, n] : batch.errors) {
          auto [t, tenant, status, place, box, route, by, ref] = k;
          sqlite::Stmt w(*db_, std::string("INSERT INTO error_5m") +
                                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (t, tenant, status, place, box, route, "
                                   "error_by, ref) DO UPDATE SET n = n + excluded.n");
          w.bind(1, t).bind(2, tenant).bind(3, status).bind(4, place).bind(5, box).bind(6, route);
          w.bind(7, by).bind(8, ref).bind(9, n);
          w.run();
        }
        for (const auto& [k, x] : batch.services) {
          auto [t, tenant, box, service, method] = k;
          const char* table = "service_5m";
          SvcSums total = x;
          {
            sqlite::Stmt s(*db_, std::string("SELECT n, failed, n4, n5, neterr, outbox, us, bytes_out, bytes_in, hist FROM ") +
                                     table + " WHERE t = ? AND tenant = ? AND box = ? AND service = ? AND method = ?");
            s.bind(1, t).bind(2, tenant).bind(3, box).bind(4, service).bind(5, method);
            if (s.step()) {
              SvcSums old{s.int64(0), s.int64(1), s.int64(2), s.int64(3), s.int64(4), s.int64(5),
                          s.int64(6), s.int64(7), s.int64(8), Histogram::decode(s.text(9))};
              total.merge(old);
            }
          }
          sqlite::Stmt w(*db_, std::string("INSERT OR REPLACE INTO ") + table +
                                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
          w.bind(1, t).bind(2, tenant).bind(3, box).bind(4, service).bind(5, method).bind(6, total.n);
          w.bind(7, total.failed).bind(8, total.n4).bind(9, total.n5).bind(10, total.neterr).bind(11, total.outbox);
          w.bind(12, total.us).bind(13, total.bytes_out).bind(14, total.bytes_in).bind(15, total.hist.encode());
          w.run();
        }
        for (const auto& [k, n] : batch.svc_statuses) {
          auto [t, tenant, service, status] = k;
          sqlite::Stmt w(*db_, std::string("INSERT INTO service_status_5m") +
                                   " VALUES (?, ?, ?, ?, ?) ON CONFLICT (t, tenant, service, status) DO UPDATE SET n = n + "
                                   "excluded.n");
          w.bind(1, t).bind(2, tenant).bind(3, service).bind(4, status).bind(5, n);
          w.run();
        }
        for (const auto& f : batch.svc_failures) {
          sqlite::Stmt w(*db_, "INSERT INTO service_failures VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
          w.bind(1, f.at_ms).bind(2, f.tenant).bind(3, f.box).bind(4, f.service).bind(5, f.method).bind(6, f.status);
          w.bind(7, f.error).bind(8, f.url).bind(9, f.request_id).bind(10, f.outbox_id);
          w.run();
          service_failures_added_ = true;
        }
        sqlite::Stmt c(*db_, "INSERT OR REPLACE INTO cursors VALUES (?, ?, ?)");
        c.bind(1, name).bind(2, int64_t(id->first)).bind(3, int64_t(next));
        c.run();
        db_->exec("COMMIT");
      } catch (...) {
        db_->exec("ROLLBACK");
        throw;
      }
      if (next == at) break;
      at = next;
    }
  }
  return lines;
}

namespace {
constexpr const char* kRollups[] = {"route_5m", "edge_5m", "error_5m", "service_5m", "service_status_5m", "gauge_5m", "log_5m",
                                    "control_5m", "mcp_queue_5m"};
constexpr int64_t kDayMs = 24 * kHourMs;
constexpr int64_t kLogDaysKeptMs = 90 * kDayMs;
std::string base_day(int64_t ms) { return base::format_utc_ms(ms).substr(0, 10); }
}  // namespace

void Traffic::drop_old(int64_t now_ms) {
  last_drop_ms_ = now_ms;
  for (const char* table : kRollups) {
    sqlite::Stmt s(*db_, std::string("DELETE FROM ") + table + " WHERE t < ?");
    s.bind(1, now_ms - kKeptMs);
    s.run();
  }
  {
    sqlite::Stmt s(*db_, "DELETE FROM log_day WHERE day < ?");
    s.bind(1, now_ms - kLogDaysKeptMs).run();
  }
  trim_service_failures(now_ms);
  // Over its byte budget (045.Limits.md), the oldest hour goes, until it fits or only the
  // last hour is left.
  while (budget_bytes_ > 0 && size_bytes_locked() > budget_bytes_) {
    int64_t oldest = 0;
    {
      sqlite::Stmt q(*db_, "SELECT min(t) FROM (SELECT min(t) AS t FROM route_5m UNION ALL SELECT min(t) FROM service_5m"
                           " UNION ALL SELECT min(t) FROM gauge_5m UNION ALL SELECT min(t) FROM control_5m"
                           " UNION ALL SELECT min(t) FROM mcp_queue_5m)");
      if (!q.step() || q.is_null(0)) break;
      oldest = q.int64(0);
    }
    if (oldest >= now_ms - kHourMs) break;
    int64_t cut = oldest + kHourMs;
    for (const char* table : kRollups) {
      sqlite::Stmt s(*db_, std::string("DELETE FROM ") + table + " WHERE t < ?");
      s.bind(1, cut);
      s.run();
    }
    sqlite::Stmt f(*db_, "DELETE FROM service_failures WHERE at_ms < ?");
    f.bind(1, cut).run();
    ++budget_drops_;
    db_->exec("PRAGMA incremental_vacuum");
  }
  db_->exec("PRAGMA incremental_vacuum");
  db_->exec("PRAGMA wal_checkpoint(TRUNCATE)");
}

int64_t Traffic::size_bytes_locked() {
  auto pragma = [&](const char* name) {
    sqlite::Stmt q(*db_, std::string("PRAGMA ") + name);
    return q.step() ? q.int64(0) : int64_t(0);
  };
  return (pragma("page_count") - pragma("freelist_count")) * pragma("page_size");
}

int64_t Traffic::size_bytes() {
  std::lock_guard lock(mu_);
  return size_bytes_locked();
}

void Traffic::set_budget(int64_t bytes) {
  std::lock_guard lock(mu_);
  budget_bytes_ = bytes;
}

// The newest failures per service, for 7 days (028 § 4.4).
void Traffic::trim_service_failures(int64_t now_ms) {
  sqlite::Stmt old(*db_, "DELETE FROM service_failures WHERE at_ms < ?");
  old.bind(1, now_ms - kServiceFailuresKeptMs).run();
  db_->exec("DELETE FROM service_failures WHERE rowid IN (SELECT rowid FROM (SELECT rowid, row_number() OVER "
            "(PARTITION BY service ORDER BY at_ms DESC, rowid DESC) AS k FROM service_failures) WHERE k > " +
            std::to_string(kServiceFailuresPerService) + ")");
}

void Traffic::gauge(int64_t at_ms, const std::string& name, const std::string& tenant,
                    const std::map<std::string, int64_t>& values) {
  std::lock_guard lock(gauge_mu_);
  int64_t bucket = at_ms / kBucketMs * kBucketMs;
  auto set = [&](const std::string& key, int64_t value) {
    gauge_now_[{name, tenant, key}] = {at_ms, value};
    auto& m = gauge_pending_[{bucket, name, tenant, key}];
    m = std::max(m, value);
  };
  // A key sampled before and missing now is 0 now.
  for (auto it = gauge_now_.lower_bound({name, tenant, ""});
       it != gauge_now_.end() && std::get<0>(it->first) == name && std::get<1>(it->first) == tenant;) {
    std::string key = std::get<2>(it->first);
    ++it;
    if (!values.count(key)) {
      if (gauge_now_[{name, tenant, key}].second == 0) {
        gauge_now_.erase({name, tenant, key});  // forgotten once it has read 0
      } else {
        set(key, 0);
      }
    }
  }
  for (const auto& [key, value] : values) set(key, value);
}

void Traffic::flush_gauges() {
  decltype(gauge_pending_) pending;
  {
    std::lock_guard lock(gauge_mu_);
    pending.swap(gauge_pending_);
  }
  if (pending.empty()) return;
  db_->exec("BEGIN IMMEDIATE");
  try {
    for (const auto& [k, v] : pending) {
      auto [t, name, tenant, key] = k;
      sqlite::Stmt w(*db_,
                     "INSERT INTO gauge_5m VALUES (?, ?, ?, ?, ?) ON CONFLICT (t, name, tenant, key) DO UPDATE SET value = "
                     "max(value, excluded.value)");
      w.bind(1, t).bind(2, name).bind(3, tenant).bind(4, key).bind(5, v);
      w.run();
    }
    db_->exec("COMMIT");
  } catch (...) {
    db_->exec("ROLLBACK");
    throw;
  }
}

struct Traffic::Window {
  static constexpr const char* table = "route_5m";
  static constexpr const char* edge_table = "edge_5m";
  static constexpr const char* error_table = "error_5m";
  int64_t span_ms, step_ms;
  static std::optional<Window> of(std::string_view w) {
    if (w == "1h") return Window{kHourMs, kBucketMs};
    if (w == "12h") return Window{12 * kHourMs, 2 * kBucketMs};
    if (w == "7d") return Window{7 * 24 * kHourMs, kHourMs};
    return std::nullopt;
  }
  // The window's last point ends at the step after now.
  int64_t to(int64_t now) const { return (now / step_ms + 1) * step_ms; }
  int64_t from(int64_t now) const { return to(now) - span_ms; }
};

bool Traffic::valid_window(std::string_view window) { return Window::of(window).has_value(); }

bj::object Traffic::series(std::string_view window, const std::string& tenant, const std::string& box,
                           int64_t now_ms) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  std::lock_guard lock(mu_);
  int64_t from = w.from(now_ms), to = w.to(now_ms);
  size_t points = size_t(w.span_ms / w.step_ms);
  std::vector<Sums> sums(points);
  sqlite::Stmt s(*db_, std::string("SELECT t, n, n3, n4, n5, us, us_max, hist FROM ") + w.table +
                           " WHERE t >= ? AND t < ? AND (? = '' OR tenant = ?) AND (? = '' OR box = ?)");
  s.bind(1, from).bind(2, to).bind(3, tenant).bind(4, tenant).bind(5, box).bind(6, box);
  while (s.step()) {
    size_t i = size_t((s.int64(0) - from) / w.step_ms);
    if (i >= points) continue;
    Sums one{s.int64(1), s.int64(2), s.int64(3), s.int64(4), s.int64(5), s.int64(6), 0, 0, 0,
             Histogram::decode(s.text(7))};
    sums[i].merge(one);
  }
  bj::array out;
  for (size_t i = 0; i < points; ++i) {
    const auto& p = sums[i];
    out.push_back(bj::object{{"t", from + int64_t(i) * w.step_ms},
                             {"n", p.n},
                             {"n3", p.n3},
                             {"n4", p.n4},
                             {"n5", p.n5},
                             {"us", p.us},
                             {"p50_us", p.hist.percentile(0.5)},
                             {"p99_us", p.hist.percentile(0.99)}});
  }
  return bj::object{{"window", std::string(window)},
                    {"step_ms", w.step_ms},
                    {"from_ms", from},
                    {"to_ms", to},
                    {"points", std::move(out)}};
}

bj::array Traffic::routes(std::string_view window, const std::string& tenant, const std::string& box, int64_t now_ms) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  std::lock_guard lock(mu_);
  std::map<std::tuple<std::string, std::string, std::string>, Sums> by;
  sqlite::Stmt s(*db_, std::string("SELECT tenant, box, route, n, n3, n4, n5, us, us_max, fuel, sql, outbox, hist FROM ") +
                           w.table + " WHERE t >= ? AND t < ? AND (? = '' OR tenant = ?) AND (? = '' OR box = ?)");
  s.bind(1, w.from(now_ms)).bind(2, w.to(now_ms)).bind(3, tenant).bind(4, tenant).bind(5, box).bind(6, box);
  while (s.step()) {
    Sums one{s.int64(3), s.int64(4), s.int64(5), s.int64(6), s.int64(7), s.int64(8), s.int64(9), s.int64(10),
             s.int64(11), Histogram::decode(s.text(12))};
    by[{s.text(0), s.text(1), s.text(2)}].merge(one);
  }
  std::vector<std::pair<std::tuple<std::string, std::string, std::string>, Sums>> list(by.begin(), by.end());
  std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
  bj::array out;
  for (const auto& [k, p] : list) {
    out.push_back(bj::object{{"tenant", std::get<0>(k)},
                             {"box", std::get<1>(k)},
                             {"route", std::get<2>(k)},
                             {"n", p.n},
                             {"n3", p.n3},
                             {"n4", p.n4},
                             {"n5", p.n5},
                             {"us", p.us},
                             {"us_max", p.us_max},
                             {"p50_us", p.hist.percentile(0.5)},
                             {"p99_us", p.hist.percentile(0.99)},
                             {"fuel", p.fuel},
                             {"sql", p.sql},
                             {"outbox", p.outbox}});
  }
  return out;
}

bj::array Traffic::hot_boxes(std::string_view window, int64_t now_ms) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, std::string("SELECT box, SUM(n), SUM(n5), SUM(us), SUM(fuel), SUM(sql) FROM ") + w.table +
                           " WHERE t >= ? AND t < ? AND box != '' GROUP BY box ORDER BY SUM(us) DESC");
  s.bind(1, w.from(now_ms)).bind(2, w.to(now_ms));
  bj::array out;
  int64_t all = 0;
  while (s.step()) {
    all += s.int64(3);
    out.push_back(bj::object{{"box", s.text(0)},
                             {"n", s.int64(1)},
                             {"n5", s.int64(2)},
                             {"us", s.int64(3)},
                             {"fuel", s.int64(4)},
                             {"sql", s.int64(5)}});
  }
  for (auto& v : out) {
    auto& o = v.as_object();
    o["share"] = all ? double(o["us"].as_int64()) / double(all) : 0.0;
  }
  return out;
}

bj::array Traffic::edges(std::string_view window, const std::string& box, int64_t now_ms) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, std::string("SELECT tenant, from_box, to_kind, to_name, SUM(n), SUM(errors), SUM(us), "
                                   "SUM(bytes_out), SUM(bytes_in) FROM ") +
                           w.edge_table +
                           " WHERE t >= ? AND t < ? AND (? = '' OR from_box = ? OR (to_kind = 'box' AND to_name = ?))"
                           " GROUP BY tenant, from_box, to_kind, to_name ORDER BY SUM(n) DESC");
  s.bind(1, w.from(now_ms)).bind(2, w.to(now_ms)).bind(3, box).bind(4, box).bind(5, box);
  bj::array out;
  while (s.step()) {
    out.push_back(bj::object{{"tenant", s.text(0)},
                             {"from_box", s.text(1)},
                             {"to_kind", s.text(2)},
                             {"to_name", s.text(3)},
                             {"n", s.int64(4)},
                             {"errors", s.int64(5)},
                             {"us", s.int64(6)},
                             {"bytes_out", s.int64(7)},
                             {"bytes_in", s.int64(8)}});
  }
  return out;
}

bj::object Traffic::errors(std::string_view window, const std::string& tenant, int64_t now_ms) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  std::lock_guard lock(mu_);
  int64_t from = w.from(now_ms), to = w.to(now_ms);
  size_t count = size_t(w.span_ms / w.step_ms);
  struct Point {
    int64_t n = 0, n4 = 0, n5 = 0, host = 0, box = 0, box_failed = 0;
  };
  std::vector<Point> points(count);
  Point totals;
  {
    sqlite::Stmt s(*db_, std::string("SELECT t, SUM(n) FROM ") + w.table +
                             " WHERE t >= ? AND t < ? AND (? = '' OR tenant = ?) GROUP BY t");
    s.bind(1, from).bind(2, to).bind(3, tenant).bind(4, tenant);
    while (s.step()) {
      size_t i = size_t((s.int64(0) - from) / w.step_ms);
      if (i < count) points[i].n += s.int64(1);
      totals.n += s.int64(1);
    }
  }
  std::map<int64_t, Point> statuses;
  struct Place {
    int64_t n = 0, same = 0, other = 0, none = 0;
  };
  using PlaceKey = std::tuple<std::string, std::string, std::string, std::string, int64_t, std::string>;
  std::map<PlaceKey, Place> places;
  int64_t same = 0, other = 0, none = 0;
  sqlite::Stmt s(*db_, std::string("SELECT t, tenant, status, place, box, route, error_by, ref, n FROM ") + w.error_table +
                           " WHERE t >= ? AND t < ? AND (? = '' OR tenant = ?)");
  s.bind(1, from).bind(2, to).bind(3, tenant).bind(4, tenant);
  while (s.step()) {
    int64_t status = s.int64(2), n = s.int64(8);
    std::string by = s.text(6), ref = s.text(7);
    auto add = [&](Point& p) {
      (status >= 500 ? p.n5 : p.n4) += n;
      (by == "box" ? p.box : by == "box_failed" ? p.box_failed : p.host) += n;
    };
    size_t i = size_t((s.int64(0) - from) / w.step_ms);
    if (i < count) add(points[i]);
    add(totals);
    auto& st = statuses[status];
    st.n += n;
    add(st);
    auto& pl = places[{s.text(1), s.text(3), s.text(4), s.text(5), status, by}];
    pl.n += n;
    (ref == "same" ? pl.same : ref == "other" ? pl.other : pl.none) += n;
    (ref == "same" ? same : ref == "other" ? other : none) += n;
  }
  bj::array pts;
  for (size_t i = 0; i < count; ++i) {
    const auto& p = points[i];
    pts.push_back(bj::object{{"t", from + int64_t(i) * w.step_ms}, {"n", p.n}, {"n4", p.n4}, {"n5", p.n5},
                             {"host", p.host}, {"box", p.box}, {"box_failed", p.box_failed}});
  }
  bj::array sts;
  for (const auto& [status, p] : statuses) {
    sts.push_back(bj::object{{"status", status}, {"n", p.n4 + p.n5}, {"host", p.host}, {"box", p.box},
                             {"box_failed", p.box_failed}});
  }
  std::vector<std::pair<PlaceKey, Place>> list(places.begin(), places.end());
  std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
  if (list.size() > 200) list.resize(200);
  bj::array pls;
  for (const auto& [k, p] : list) {
    pls.push_back(bj::object{{"tenant", std::get<0>(k)}, {"place", std::get<1>(k)}, {"box", std::get<2>(k)},
                             {"route", std::get<3>(k)}, {"status", std::get<4>(k)}, {"error_by", std::get<5>(k)},
                             {"n", p.n}, {"same", p.same}, {"other", p.other}, {"none", p.none}});
  }
  return bj::object{{"window", std::string(window)},
                    {"step_ms", w.step_ms},
                    {"from_ms", from},
                    {"to_ms", to},
                    {"totals", bj::object{{"n", totals.n}, {"n4", totals.n4}, {"n5", totals.n5}, {"host", totals.host},
                                          {"box", totals.box}, {"box_failed", totals.box_failed}}},
                    {"points", std::move(pts)},
                    {"statuses", std::move(sts)},
                    {"places", std::move(pls)},
                    {"referred", bj::object{{"same", same}, {"other", other}, {"none", none}}}};
}

namespace {

bj::object svc_fields(const SvcSums& s) {
  return bj::object{{"calls", s.n},           {"failed", s.failed},       {"n4", s.n4},
                    {"n5", s.n5},             {"neterr", s.neterr},       {"outbox", s.outbox},
                    {"us", s.us},             {"p50_us", s.hist.percentile(0.5)}, {"p99_us", s.hist.percentile(0.99)},
                    {"bytes_out", s.bytes_out}, {"bytes_in", s.bytes_in}};
}

}  // namespace

bj::object Traffic::services(std::string_view window, const std::string& tenant, const std::string& service,
                             int64_t now_ms, size_t latest) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  std::lock_guard lock(mu_);
  int64_t from = w.from(now_ms), to = w.to(now_ms);
  size_t count = size_t(w.span_ms / w.step_ms);
  std::vector<SvcSums> points(count);
  SvcSums totals;
  struct PerService {
    SvcSums sums;
    std::set<std::string> tenants, boxes;
  };
  std::map<std::string, PerService> by_service;
  std::map<std::tuple<std::string, std::string, std::string, std::string>, SvcSums> by_method;
  sqlite::Stmt s(*db_, std::string("SELECT t, tenant, box, service, method, n, failed, n4, n5, neterr, outbox, us, bytes_out, "
                                   "bytes_in, hist FROM ") +
                           "service_5m" +
                           " WHERE t >= ? AND t < ? AND (? = '' OR tenant = ?) AND (? = '' OR service = ?)");
  s.bind(1, from).bind(2, to).bind(3, tenant).bind(4, tenant).bind(5, service).bind(6, service);
  while (s.step()) {
    SvcSums one{s.int64(5), s.int64(6), s.int64(7), s.int64(8), s.int64(9), s.int64(10),
                s.int64(11), s.int64(12), s.int64(13), Histogram::decode(s.text(14))};
    size_t i = size_t((s.int64(0) - from) / w.step_ms);
    if (i < count) points[i].merge(one);
    totals.merge(one);
    auto& ps = by_service[s.text(3)];
    ps.sums.merge(one);
    if (!s.text(1).empty()) ps.tenants.insert(s.text(1));
    if (!s.text(2).empty()) ps.boxes.insert(s.text(2));
    by_method[{s.text(1), s.text(2), s.text(3), s.text(4)}].merge(one);
  }
  bj::array pts;
  for (size_t i = 0; i < count; ++i) {
    const auto& p = points[i];
    pts.push_back(bj::object{{"t", from + int64_t(i) * w.step_ms}, {"calls", p.n}, {"failed", p.failed},
                             {"outbox", p.outbox}, {"p50_us", p.hist.percentile(0.5)}, {"p99_us", p.hist.percentile(0.99)}});
  }
  std::vector<std::pair<std::string, PerService>> svcs(by_service.begin(), by_service.end());
  std::stable_sort(svcs.begin(), svcs.end(), [](const auto& a, const auto& b) { return a.second.sums.n > b.second.sums.n; });
  bj::array services_out;
  for (const auto& [name, ps] : svcs) {
    auto o = svc_fields(ps.sums);
    o["service"] = name;
    bj::array ts, bs;
    for (const auto& t : ps.tenants) ts.emplace_back(t);
    for (const auto& b : ps.boxes) bs.emplace_back(b);
    o["tenants"] = std::move(ts);
    o["boxes"] = std::move(bs);
    services_out.push_back(std::move(o));
  }
  std::vector<std::pair<std::tuple<std::string, std::string, std::string, std::string>, SvcSums>> ms(by_method.begin(),
                                                                                                       by_method.end());
  std::stable_sort(ms.begin(), ms.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
  if (ms.size() > 500) ms.resize(500);
  bj::array methods;
  for (const auto& [k, x] : ms) {
    auto o = svc_fields(x);
    o["tenant"] = std::get<0>(k);
    o["box"] = std::get<1>(k);
    o["service"] = std::get<2>(k);
    o["method"] = std::get<3>(k);
    methods.push_back(std::move(o));
  }
  bj::array statuses;
  {
    sqlite::Stmt st(*db_, std::string("SELECT service, status, SUM(n) FROM ") +
                              "service_status_5m" +
                              " WHERE t >= ? AND t < ? AND (? = '' OR tenant = ?) AND (? = '' OR service = ?)"
                              " GROUP BY service, status ORDER BY SUM(n) DESC LIMIT 200");
    st.bind(1, from).bind(2, to).bind(3, tenant).bind(4, tenant).bind(5, service).bind(6, service);
    while (st.step()) statuses.push_back(bj::object{{"service", st.text(0)}, {"status", st.int64(1)}, {"n", st.int64(2)}});
  }
  bj::array last;
  {
    sqlite::Stmt st(*db_, "SELECT at_ms, tenant, box, service, method, status, error, url, request_id, outbox_id FROM "
                          "service_failures WHERE at_ms >= ? AND (? = '' OR tenant = ?) AND (? = '' OR service = ?)"
                          " ORDER BY at_ms DESC, rowid DESC LIMIT ?");
    st.bind(1, from).bind(2, tenant).bind(3, tenant).bind(4, service).bind(5, service).bind(6, int64_t(latest));
    while (st.step()) {
      bj::object o{{"at_ms", st.int64(0)}, {"tenant", st.text(1)}, {"box", st.text(2)}, {"service", st.text(3)},
                   {"method", st.text(4)}, {"status", st.int64(5)}, {"error", st.text(6)}};
      if (!st.text(7).empty()) o["url"] = st.text(7);
      if (!st.text(8).empty()) o["request_id"] = st.text(8);
      if (st.int64(9)) o["outbox_id"] = st.int64(9);
      last.push_back(std::move(o));
    }
  }
  return bj::object{{"window", std::string(window)},
                    {"step_ms", w.step_ms},
                    {"from_ms", from},
                    {"to_ms", to},
                    {"totals", svc_fields(totals)},
                    {"points", std::move(pts)},
                    {"services", std::move(services_out)},
                    {"methods", std::move(methods)},
                    {"statuses", std::move(statuses)},
                    {"latest", std::move(last)}};
}

bj::object Traffic::gauge_series(std::string_view window, const std::string& name, const std::string& tenant,
                                 int64_t now_ms) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  std::lock_guard lock(mu_);
  int64_t from = w.from(now_ms), to = w.to(now_ms);
  size_t count = size_t(w.span_ms / w.step_ms);
  std::vector<int64_t> points(count, 0);
  std::vector<bool> seen(count, false);
  // Each five minutes' sum over tenants and keys; a point is its largest.
  sqlite::Stmt s(*db_, std::string("SELECT t, SUM(value) FROM ") + "gauge_5m" +
                           " WHERE name = ? AND t >= ? AND t < ? AND (? = '' OR tenant = ?) GROUP BY t");
  s.bind(1, name).bind(2, from).bind(3, to).bind(4, tenant).bind(5, tenant);
  while (s.step()) {
    size_t i = size_t((s.int64(0) - from) / w.step_ms);
    if (i >= count) continue;
    points[i] = std::max(points[i], s.int64(1));
    seen[i] = true;
  }
  bj::array pts;
  for (size_t i = 0; i < count; ++i) {
    bj::object p{{"t", from + int64_t(i) * w.step_ms}, {"value", points[i]}};
    if (!seen[i]) p["value"] = nullptr;  // not sampled: the host was down, or nothing was measured
    pts.push_back(std::move(p));
  }
  bj::array now;
  {
    std::lock_guard g(gauge_mu_);
    for (const auto& [k, v] : gauge_now_) {
      const auto& [n, t, key] = k;
      if (n != name || (!tenant.empty() && t != tenant)) continue;
      now.push_back(bj::object{{"tenant", t}, {"key", key}, {"value", v.second}, {"at_ms", v.first}});
    }
  }
  return bj::object{{"window", std::string(window)},
                    {"step_ms", w.step_ms},
                    {"from_ms", from},
                    {"to_ms", to},
                    {"points", std::move(pts)},
                    {"now", std::move(now)}};
}

// ---- Log volume (032 § 10) ----

void Traffic::log_written(int64_t at_ms, const std::string& log, const std::string& tenant, const std::string& box,
                          const std::string& route, int64_t bytes) {
  if (bytes <= 0) return;
  std::lock_guard lock(log_mu_);
  auto& v = log_pending_[{at_ms / kBucketMs * kBucketMs, log, tenant, box, route}];
  v.first += bytes;
  v.second += 1;
}

void Traffic::flush_logs() {
  decltype(log_pending_) pending;
  {
    std::lock_guard lock(log_mu_);
    pending.swap(log_pending_);
  }
  if (pending.empty()) return;
  std::map<std::pair<int64_t, std::string>, std::pair<int64_t, int64_t>> days;
  db_->exec("BEGIN IMMEDIATE");
  try {
    for (const auto& [k, v] : pending) {
      const auto& [t, log, tenant, box, route] = k;
      sqlite::Stmt w(*db_,
                     "INSERT INTO log_5m VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT (t, log, tenant, box, route) DO UPDATE SET "
                     "bytes = bytes + excluded.bytes, lines = lines + excluded.lines");
      w.bind(1, t).bind(2, log).bind(3, tenant).bind(4, box).bind(5, route).bind(6, v.first).bind(7, v.second);
      w.run();
      auto& d = days[{t / kDayMs * kDayMs, log}];
      d.first += v.first;
      d.second += v.second;
    }
    // A day the files seeded becomes a counted one only from nothing: the counts never add to a seed.
    for (const auto& [k, v] : days) {
      sqlite::Stmt d(*db_,
                     "INSERT INTO log_day VALUES (?, ?, ?, ?, 1) ON CONFLICT (day, log) DO UPDATE SET "
                     "bytes = CASE WHEN counted THEN bytes + excluded.bytes ELSE bytes END, "
                     "lines = CASE WHEN counted THEN lines + excluded.lines ELSE lines END");
      d.bind(1, k.first).bind(2, k.second).bind(3, v.first).bind(4, v.second);
      d.run();
    }
    db_->exec("COMMIT");
  } catch (...) {
    db_->exec("ROLLBACK");
    throw;
  }
}

int Traffic::seed_log_days(int64_t now_ms) {
  int64_t today = now_ms / kDayMs * kDayMs;
  // (day, log) -> bytes, from name.log.N files: each holds lines from the day its first line has
  // (a log rotates at each UTC day, and within one by size).
  std::map<std::pair<int64_t, std::string>, int64_t> days;
  auto scan = [&](const fs::path& dir, const std::string& prefix) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      std::string file = it->path().filename().string();
      auto dot = file.find(".log.");
      if (dot == std::string::npos || dot == 0) continue;
      std::string n = file.substr(dot + 5);
      if (n.empty() || !std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
      std::ifstream in(it->path());
      std::string first;
      if (!std::getline(in, first)) continue;
      auto a = first.find("\"at\":\"");
      if (a == std::string::npos) continue;
      auto at = parse_utc_ms(first.substr(a + 6, 24));
      if (!at) continue;
      std::error_code e2;
      auto size = fs::file_size(it->path(), e2);
      if (e2) continue;
      int64_t day = *at / kDayMs * kDayMs;
      if (day >= today || day < today - kLogDaysKeptMs) continue;
      days[{day, prefix + file.substr(0, dot)}] += int64_t(size);
    }
  };
  scan(logs_, "");
  scan(logs_ / "boxes", "boxes/");
  if (days.empty()) return 0;
  std::lock_guard lock(mu_);
  int added = 0;
  for (const auto& [k, bytes] : days) {
    sqlite::Stmt s(*db_, "INSERT OR IGNORE INTO log_day VALUES (?, ?, ?, 0, 0)");
    s.bind(1, k.first).bind(2, k.second).bind(3, bytes);
    s.run();
    added += db_->changes() > 0 ? 1 : 0;
  }
  return added;
}

bj::object Traffic::log_velocity(std::string_view window, const std::string& log, int64_t now_ms, size_t top) {
  auto w = Window::of(window).value_or(*Window::of("7d"));
  std::lock_guard lock(mu_);
  int64_t from = w.from(now_ms), to = w.to(now_ms);
  int64_t today = now_ms / kDayMs * kDayMs;
  size_t count = size_t(w.span_ms / w.step_ms);
  auto wanted = [&](const std::string& l) { return log.empty() || l == log; };

  struct Stat {
    int64_t last_bytes = 0, last_lines = 0, today_bytes = 0;
    std::vector<int64_t> history;  // the seven days before today that are known
    std::vector<int64_t> points;
  };
  std::map<std::string, Stat> stats;
  {
    sqlite::Stmt s(*db_, "SELECT log, sum(bytes), sum(lines) FROM log_5m WHERE t >= ? GROUP BY log");
    s.bind(1, now_ms - kDayMs);
    while (s.step()) {
      if (!wanted(s.text(0))) continue;
      auto& st = stats[s.text(0)];
      st.last_bytes = s.int64(1);
      st.last_lines = s.int64(2);
    }
  }
  bj::array days;
  {
    sqlite::Stmt s(*db_, "SELECT day, log, bytes, lines, counted FROM log_day WHERE day >= ? ORDER BY day DESC, log");
    s.bind(1, today - kLogDaysKeptMs);
    while (s.step()) {
      std::string l = s.text(1);
      if (!wanted(l)) continue;
      int64_t day = s.int64(0), bytes = s.int64(2);
      auto& st = stats[l];
      if (day == today) st.today_bytes = bytes;
      if (day < today && day >= today - 7 * kDayMs) st.history.push_back(bytes);
      bj::object row{{"day", base_day(day)}, {"log", l}, {"bytes", bytes}, {"counted", s.int64(4) != 0}};
      row["lines"] = s.int64(4) ? bj::value(s.int64(3)) : bj::value(nullptr);
      days.push_back(std::move(row));
    }
  }
  {
    sqlite::Stmt s(*db_, "SELECT t, log, sum(bytes) FROM log_5m WHERE t >= ? AND t < ? GROUP BY t, log");
    s.bind(1, from).bind(2, to);
    while (s.step()) {
      if (!wanted(s.text(1))) continue;
      auto& st = stats[s.text(1)];
      if (st.points.empty()) st.points.assign(count, 0);
      size_t i = size_t((s.int64(0) - from) / w.step_ms);
      if (i < count) st.points[i] += s.int64(2);
    }
  }
  bj::array logs, series;
  for (auto& [l, st] : stats) {
    bj::object o{{"log", l}, {"last_24h_bytes", st.last_bytes}, {"last_24h_lines", st.last_lines},
                 {"today_bytes", st.today_bytes}, {"days_known", int64_t(st.history.size())}};
    bj::value median = nullptr, growth = nullptr;
    bool grew = false;
    if (!st.history.empty()) {
      auto h = st.history;
      std::sort(h.begin(), h.end());
      int64_t m = h.size() % 2 ? h[h.size() / 2] : (h[h.size() / 2 - 1] + h[h.size() / 2]) / 2;
      median = m;
      if (h.size() >= 3 && m > 0) {
        double g = double(st.last_bytes) / double(m);
        growth = std::round(g * 100) / 100;
        grew = g >= kGrowthFactor && st.last_bytes >= kGrowthFloorBytes;
      }
    }
    o["median_day_bytes"] = median;
    o["growth"] = growth;
    o["grew"] = grew;
    logs.push_back(std::move(o));
    if (st.points.empty()) st.points.assign(count, 0);
    bj::array pts;
    for (auto v : st.points) pts.push_back(v);
    series.push_back(bj::object{{"log", l}, {"points", std::move(pts)}});
  }
  // Biggest first.
  std::sort(logs.begin(), logs.end(), [](const bj::value& a, const bj::value& b) {
    return a.at("last_24h_bytes").as_int64() > b.at("last_24h_bytes").as_int64();
  });
  bj::array tops;
  {
    std::map<std::string, int64_t> per_log;
    struct Row {
      std::string log, tenant, box, route;
      int64_t bytes, lines;
    };
    std::vector<Row> rows;
    sqlite::Stmt s(*db_, "SELECT log, tenant, box, route, sum(bytes), sum(lines) FROM log_5m WHERE t >= ? AND t < ? "
                         "GROUP BY log, tenant, box, route");
    s.bind(1, from).bind(2, to);
    while (s.step()) {
      if (!wanted(s.text(0))) continue;
      rows.push_back({s.text(0), s.text(1), s.text(2), s.text(3), s.int64(4), s.int64(5)});
      per_log[rows.back().log] += rows.back().bytes;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
    for (size_t i = 0; i < rows.size() && i < top; ++i) {
      const auto& r = rows[i];
      int64_t all = per_log[r.log];
      tops.push_back(bj::object{{"log", r.log}, {"tenant", r.tenant}, {"box", r.box}, {"route", r.route}, {"bytes", r.bytes},
                                {"lines", r.lines}, {"share", all > 0 ? std::round(double(r.bytes) * 1000 / double(all)) / 1000 : 0.0}});
    }
  }
  return bj::object{{"window", std::string(window)},
                    {"step_ms", w.step_ms},
                    {"from_ms", from},
                    {"to_ms", to},
                    {"logs", std::move(logs)},
                    {"series", std::move(series)},
                    {"days", std::move(days)},
                    {"top", std::move(tops)}};
}

std::string sketch_union(const std::string& a, const std::string& b) {
  if (a.size() != 64) return b.size() == 64 ? b : std::string();
  if (b.size() != 64) return a;
  auto nibble = [](char c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; };
  std::string out(64, '0');
  for (size_t i = 0; i < 64; ++i) out[i] = "0123456789abcdef"[(nibble(a[i]) | nibble(b[i])) & 15];
  return out;
}

void ControlRow::merge(const ControlRow& o) {
  matched += o.matched;
  allowed += o.allowed;
  rate += o.rate;
  concurrent += o.concurrent;
  body += o.body;
  closed += o.closed;
  would += o.would;
  n5 += o.n5;
  peak = std::max(peak, o.peak);
  clients = sketch_union(clients, o.clients);
  would_clients = sketch_union(would_clients, o.would_clients);
  // The busiest client of two halves of a bucket: the same one adds up, else the larger stands
  // (approximate, as 047 § 6 allows).
  if (o.top_client == top_client) top_n += o.top_n;
  else if (o.top_n > top_n) top_client = o.top_client, top_n = o.top_n;
  if (o.would_top == would_top) would_top_n += o.would_top_n;
  else if (o.would_top_n > would_top_n) would_top = o.would_top, would_top_n = o.would_top_n;
  hist.merge(o.hist);
}

void Traffic::add_control_rows(std::vector<ControlRow> rows) {
  if (rows.empty()) return;
  std::lock_guard lock(control_mu_);
  for (auto& r : rows) control_pending_.push_back(std::move(r));
}

namespace {

constexpr const char* kControlColumns =
    "t, tenant, domain, control, matched, allowed, rate, concurrent, body, closed, would, n5, peak, clients, would_clients, "
    "top_client, top_n, would_top, would_top_n, hist";

ControlRow control_row_of(const sqlite::Stmt& s) {
  ControlRow r;
  r.t = s.int64(0);
  r.tenant = s.text(1);
  r.domain = s.text(2);
  r.control = s.text(3);
  r.matched = s.int64(4);
  r.allowed = s.int64(5);
  r.rate = s.int64(6);
  r.concurrent = s.int64(7);
  r.body = s.int64(8);
  r.closed = s.int64(9);
  r.would = s.int64(10);
  r.n5 = s.int64(11);
  r.peak = s.int64(12);
  r.clients = s.text(13);
  r.would_clients = s.text(14);
  r.top_client = s.text(15);
  r.top_n = s.int64(16);
  r.would_top = s.text(17);
  r.would_top_n = s.int64(18);
  r.hist = Histogram::decode(s.text(19));
  return r;
}

}  // namespace

void Traffic::flush_controls() {
  std::vector<ControlRow> pending;
  {
    std::lock_guard lock(control_mu_);
    pending.swap(control_pending_);
  }
  if (pending.empty()) return;
  db_->exec("BEGIN IMMEDIATE");
  try {
    for (auto& r : pending) {
      r.t = r.t / kBucketMs * kBucketMs;
      // A bucket the host flushed half of before a restart adds to what is there.
      {
        sqlite::Stmt q(*db_, std::string("SELECT ") + kControlColumns + " FROM control_5m WHERE domain = ? AND control = ? AND t = ?");
        q.bind(1, r.domain).bind(2, r.control).bind(3, r.t);
        if (q.step()) {
          auto old = control_row_of(q);
          old.merge(r);
          old.tenant = r.tenant;
          r = std::move(old);
        }
      }
      sqlite::Stmt w(*db_, std::string("INSERT OR REPLACE INTO control_5m (") + kControlColumns +
                               ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
      w.bind(1, r.t).bind(2, r.tenant).bind(3, r.domain).bind(4, r.control).bind(5, r.matched).bind(6, r.allowed);
      w.bind(7, r.rate).bind(8, r.concurrent).bind(9, r.body).bind(10, r.closed).bind(11, r.would).bind(12, r.n5);
      w.bind(13, r.peak).bind(14, r.clients).bind(15, r.would_clients).bind(16, r.top_client).bind(17, r.top_n);
      w.bind(18, r.would_top).bind(19, r.would_top_n).bind(20, r.hist.encode());
      w.run();
    }
    db_->exec("COMMIT");
  } catch (...) {
    db_->exec("ROLLBACK");
    throw;
  }
}

std::vector<ControlRow> Traffic::control_rows(const std::string& domain, const std::string& control, int64_t from_ms,
                                              int64_t to_ms) {
  std::lock_guard lock(mu_);
  std::vector<ControlRow> out;
  sqlite::Stmt s(*db_, std::string("SELECT ") + kControlColumns +
                           " FROM control_5m WHERE domain = ? AND (? = '' OR control = ?) AND t >= ? AND t < ? ORDER BY t");
  s.bind(1, domain).bind(2, control).bind(3, control).bind(4, from_ms).bind(5, to_ms);
  while (s.step()) out.push_back(control_row_of(s));
  return out;
}

void McpQueueRow::merge(const McpQueueRow& o) {
  offered += o.offered;
  queued += o.queued;
  ran += o.ran;
  refused_rate += o.refused_rate;
  refused_full += o.refused_full;
  refused_wait += o.refused_wait;
  gone += o.gone;
  peak_waiting = std::max(peak_waiting, o.peak_waiting);
  peak_running = std::max(peak_running, o.peak_running);
  waited.merge(o.waited);
}

void Traffic::add_mcp_queue_rows(std::vector<McpQueueRow> rows) {
  if (rows.empty()) return;
  std::lock_guard lock(mcp_queue_mu_);
  for (auto& r : rows) mcp_queue_pending_.push_back(std::move(r));
}

namespace {

constexpr const char* kMcpQueueColumns =
    "t, queue, tenant, domain, cls, offered, queued, ran, refused_rate, refused_full, refused_wait, gone, peak_waiting, "
    "peak_running, waited";

McpQueueRow mcp_queue_row_of(const sqlite::Stmt& s) {
  McpQueueRow r;
  r.t = s.int64(0);
  r.queue = s.text(1);
  r.tenant = s.text(2);
  r.domain = s.text(3);
  r.cls = s.text(4);
  r.offered = s.int64(5);
  r.queued = s.int64(6);
  r.ran = s.int64(7);
  r.refused_rate = s.int64(8);
  r.refused_full = s.int64(9);
  r.refused_wait = s.int64(10);
  r.gone = s.int64(11);
  r.peak_waiting = s.int64(12);
  r.peak_running = s.int64(13);
  r.waited = Histogram::decode(s.text(14));
  return r;
}

}  // namespace

void Traffic::flush_mcp_queue() {
  std::vector<McpQueueRow> pending;
  {
    std::lock_guard lock(mcp_queue_mu_);
    pending.swap(mcp_queue_pending_);
  }
  if (pending.empty()) return;
  db_->exec("BEGIN IMMEDIATE");
  try {
    for (auto& r : pending) {
      r.t = r.t / kBucketMs * kBucketMs;
      // A bucket handed over in parts (a report's early take, a restart) adds to what is there.
      {
        sqlite::Stmt q(*db_, std::string("SELECT ") + kMcpQueueColumns +
                                 " FROM mcp_queue_5m WHERE queue = ? AND domain = ? AND cls = ? AND t = ?");
        q.bind(1, r.queue).bind(2, r.domain).bind(3, r.cls).bind(4, r.t);
        if (q.step()) {
          auto old = mcp_queue_row_of(q);
          old.merge(r);
          old.tenant = r.tenant;
          r = std::move(old);
        }
      }
      sqlite::Stmt w(*db_, std::string("INSERT OR REPLACE INTO mcp_queue_5m (") + kMcpQueueColumns +
                               ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
      w.bind(1, r.t).bind(2, r.queue).bind(3, r.tenant).bind(4, r.domain).bind(5, r.cls).bind(6, r.offered);
      w.bind(7, r.queued).bind(8, r.ran).bind(9, r.refused_rate).bind(10, r.refused_full).bind(11, r.refused_wait);
      w.bind(12, r.gone).bind(13, r.peak_waiting).bind(14, r.peak_running).bind(15, r.waited.encode());
      w.run();
    }
    db_->exec("COMMIT");
  } catch (...) {
    db_->exec("ROLLBACK");
    throw;
  }
}

std::vector<McpQueueRow> Traffic::mcp_queue_rows(const std::string& queue, const std::string& domain, int64_t from_ms,
                                                 int64_t to_ms) {
  std::lock_guard lock(mu_);
  std::vector<McpQueueRow> out;
  sqlite::Stmt s(*db_, std::string("SELECT ") + kMcpQueueColumns +
                           " FROM mcp_queue_5m WHERE queue = ? AND (? = '' OR domain = ?) AND t >= ? AND t < ? ORDER BY t");
  s.bind(1, queue).bind(2, domain).bind(3, domain).bind(4, from_ms).bind(5, to_ms);
  while (s.step()) out.push_back(mcp_queue_row_of(s));
  return out;
}


}  // namespace goober::operations
