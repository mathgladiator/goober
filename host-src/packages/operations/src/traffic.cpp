#include "goober/operations/traffic.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <tuple>

#include "goober/sqlite/sqlite.h"

namespace goober::operations {

namespace fs = std::filesystem;

namespace {

constexpr int64_t kMinuteMs = 60 * 1000;
constexpr int64_t kHourMs = 60 * kMinuteMs;
constexpr int64_t kMinutesKeptMs = 48 * kHourMs;
constexpr int64_t kHoursKeptMs = 400 * 24 * kHourMs;
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

struct Batch {
  std::map<RowKey, Sums> rows;
  std::map<EdgeKey, EdgeSums> edges;
  std::map<ErrorKey, int64_t> errors;
};

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
  int64_t minute = key->at_ms / kMinuteMs * kMinuteMs;
  int64_t hour = key->at_ms / kHourMs * kHourMs;
  b.rows[{minute, key->tenant, key->box, key->route}].merge(s);
  b.rows[{-hour - 1, key->tenant, key->box, key->route}].merge(s);  // negative: an hour row
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
    b.errors[{minute, key->tenant, status, place, key->box, route, by, ref}] += 1;
    b.errors[{-hour - 1, key->tenant, status, place, key->box, route, by, ref}] += 1;
  }
  // A box-to-box call: "a -> b" (039).
  if (auto arrow = internal.find(" -> "); arrow != std::string::npos) {
    EdgeSums e{1, number(line, "status") >= 500 ? 1 : 0, us, 0, 0};
    std::string from = internal.substr(0, arrow), to = internal.substr(arrow + 4);
    b.edges[{minute, key->tenant, from, "box", to}].merge(e);
    b.edges[{-hour - 1, key->tenant, from, "box", to}].merge(e);
  }
}

void add_egress(Batch& b, const bj::object& line) {
  auto at = parse_utc_ms(text(line, "at"));
  if (!at) return;
  int64_t status = number(line, "status");
  EdgeSums e{1, (status == 0 || status >= 400 || line.contains("error")) ? 1 : 0, number(line, "us"),
             number(line, "bytes_out"), number(line, "bytes_in")};
  int64_t minute = *at / kMinuteMs * kMinuteMs;
  int64_t hour = *at / kHourMs * kHourMs;
  std::string tenant = text(line, "tenant"), box = text(line, "box"), service = text(line, "service");
  b.edges[{minute, tenant, box, "service", service}].merge(e);
  b.edges[{-hour - 1, tenant, box, "service", service}].merge(e);
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
  for (const char* table : {"minutes", "hours"}) {
    db_->exec(std::string("CREATE TABLE IF NOT EXISTS ") + table +
              " (t INTEGER NOT NULL, tenant TEXT NOT NULL, box TEXT NOT NULL, route TEXT NOT NULL,"
              " n INTEGER NOT NULL, n3 INTEGER NOT NULL, n4 INTEGER NOT NULL, n5 INTEGER NOT NULL,"
              " us INTEGER NOT NULL, us_max INTEGER NOT NULL, fuel INTEGER NOT NULL, sql INTEGER NOT NULL,"
              " outbox INTEGER NOT NULL, hist TEXT NOT NULL, PRIMARY KEY (t, tenant, box, route))");
  }
  for (const char* table : {"edge_minutes", "edge_hours"}) {
    db_->exec(std::string("CREATE TABLE IF NOT EXISTS ") + table +
              " (t INTEGER NOT NULL, tenant TEXT NOT NULL, from_box TEXT NOT NULL, to_kind TEXT NOT NULL,"
              " to_name TEXT NOT NULL, n INTEGER NOT NULL, errors INTEGER NOT NULL, us INTEGER NOT NULL,"
              " bytes_out INTEGER NOT NULL, bytes_in INTEGER NOT NULL,"
              " PRIMARY KEY (t, tenant, from_box, to_kind, to_name))");
  }
  for (const char* table : {"error_minutes", "error_hours"}) {
    db_->exec(std::string("CREATE TABLE IF NOT EXISTS ") + table +
              " (t INTEGER NOT NULL, tenant TEXT NOT NULL, status INTEGER NOT NULL, place TEXT NOT NULL,"
              " box TEXT NOT NULL, route TEXT NOT NULL, error_by TEXT NOT NULL, ref TEXT NOT NULL, n INTEGER NOT NULL,"
              " PRIMARY KEY (t, tenant, status, place, box, route, error_by, ref))");
  }
  db_->exec(
      "CREATE TABLE IF NOT EXISTS cursors (name TEXT PRIMARY KEY, inode INTEGER NOT NULL, offset INTEGER NOT NULL)");
}

Traffic::~Traffic() = default;

size_t Traffic::catch_up(int64_t now_ms) {
  std::lock_guard lock(mu_);
  size_t lines = tail("access", now_ms) + tail("egress", now_ms);
  if (now_ms - last_drop_ms_ > 10 * kMinuteMs) drop_old(now_ms);
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
        } else {
          add_egress(batch, v->as_object());
        }
      }
      db_->exec("BEGIN IMMEDIATE");
      try {
        for (const auto& [k, sums] : batch.rows) {
          auto [t, tenant, box, route] = k;
          const char* table = t < 0 ? "hours" : "minutes";
          int64_t when = t < 0 ? -t - 1 : t;
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
          const char* table = t < 0 ? "edge_hours" : "edge_minutes";
          int64_t when = t < 0 ? -t - 1 : t;
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
          sqlite::Stmt w(*db_, std::string("INSERT INTO ") + (t < 0 ? "error_hours" : "error_minutes") +
                                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (t, tenant, status, place, box, route, "
                                   "error_by, ref) DO UPDATE SET n = n + excluded.n");
          w.bind(1, t < 0 ? -t - 1 : t).bind(2, tenant).bind(3, status).bind(4, place).bind(5, box).bind(6, route);
          w.bind(7, by).bind(8, ref).bind(9, n);
          w.run();
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

void Traffic::drop_old(int64_t now_ms) {
  last_drop_ms_ = now_ms;
  for (auto [table, keep] : {std::pair{"minutes", kMinutesKeptMs}, std::pair{"edge_minutes", kMinutesKeptMs},
                             std::pair{"error_minutes", kMinutesKeptMs}, std::pair{"hours", kHoursKeptMs},
                             std::pair{"edge_hours", kHoursKeptMs}, std::pair{"error_hours", kHoursKeptMs}}) {
    sqlite::Stmt s(*db_, std::string("DELETE FROM ") + table + " WHERE t < ?");
    s.bind(1, now_ms - keep);
    s.run();
  }
}

struct Traffic::Window {
  const char* table;
  const char* edge_table;
  int64_t span_ms, step_ms;
  const char* error_table;
  static std::optional<Window> of(std::string_view w) {
    if (w == "1h") return Window{"minutes", "edge_minutes", kHourMs, kMinuteMs, "error_minutes"};
    if (w == "12h") return Window{"minutes", "edge_minutes", 12 * kHourMs, 10 * kMinuteMs, "error_minutes"};
    if (w == "7d") return Window{"hours", "edge_hours", 7 * 24 * kHourMs, kHourMs, "error_hours"};
    if (w == "30d") return Window{"hours", "edge_hours", 30 * 24 * kHourMs, 6 * kHourMs, "error_hours"};
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

}  // namespace goober::operations
