// What the host grows on its own disk, and the budget for each (design/machine/045.Limits.md).

#include <chrono>
#include <cmath>
#include <iostream>
#include <set>

#include <sys/stat.h>

#include "backups.h"
#include "disk_usage.h"
#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/operations/traffic.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

int64_t bytes_under(const fs::path& root) {
  int64_t total = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2) && !it->is_symlink(e2)) total += int64_t(it->file_size(e2));
  }
  return total;
}

std::string mib(int64_t bytes) { return std::to_string((bytes + (1 << 20) - 1) >> 20) + " MiB"; }

}  // namespace

int64_t bytes_on_disk(const fs::path& root) {
  int64_t total = 0;
  std::set<std::pair<uint64_t, uint64_t>> seen;  // (device, inode) of files with more than one name
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2) || it->is_symlink(e2)) continue;
    if (it->hard_link_count(e2) > 1) {
      struct stat st{};
      if (::stat(it->path().c_str(), &st) == 0 && !seen.insert({uint64_t(st.st_dev), uint64_t(st.st_ino)}).second) continue;
    }
    total += int64_t(it->file_size(e2));
  }
  return total;
}

int64_t archive_only_bytes(const fs::path& archive) {
  int64_t total = 0;
  std::error_code ec;
  for (auto it = fs::directory_iterator(archive, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    std::error_code e2;
    if (it->path().extension() != ".log" || !it->is_regular_file(e2)) continue;
    if (it->hard_link_count(e2) > 1) continue;  // its numbered twin still holds these bytes
    total += int64_t(it->file_size(e2));
  }
  return total;
}

std::string Host::free_space_refused(int64_t bytes, const std::string& what) const {
  auto cfg = config_.get();
  std::error_code ec;
  auto space = fs::space(cfg->storage, ec);
  if (ec) return "";
  int64_t free = int64_t(space.available);
  if (free - bytes >= cfg->disk.free_bytes) return "";
  return what + " needs " + mib(bytes) + " and the disk has " + mib(free) + " free; it must leave limits.free-bytes (" +
         mib(cfg->disk.free_bytes) + ") free. Free space on the disk, or lower limits.free-bytes in host.json";
}

std::string Host::copies_refused(const std::vector<fs::path>& files, const std::string& what) const {
  auto cfg = config_.get();
  int64_t total = 0;
  for (const auto& f : files) {
    std::error_code ec;
    auto n = fs::file_size(f, ec);
    if (!ec) total += int64_t(n);
  }
  if (total > cfg->disk.try_bytes) {
    return what + " would copy " + mib(total) + " of databases, past limits.try-bytes (" + mib(cfg->disk.try_bytes) +
           "): a try runs on copies of every database the box writes. Raise limits.try-bytes in host.json (and reload) "
           "if the disk has room";
  }
  return free_space_refused(total, what);
}

int Host::sweep_tmp(int64_t now_ms) {
  auto cfg = config_.get();
  fs::path tmp = cfg->storage / "tmp";
  auto now = fs::file_time_type::clock::now();  // file times are on the file clock, not now_ms
  (void)now_ms;
  auto old = [&](const fs::path& p) {
    std::error_code ec;
    auto when = fs::last_write_time(p, ec);
    if (ec) return false;
    return std::chrono::duration_cast<std::chrono::hours>(now - when).count() >= cfg->disk.tmp_hours;
  };
  int gone = 0;
  std::error_code ec;
  for (auto it = fs::directory_iterator(tmp, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    std::string name = it->path().filename().string();
    if (name == "jobs") continue;  // a running dry run's copies; its run removes them
    std::error_code e2;
    if (name == "tries" || name == "upgrades") {
      // One directory per try: those a crash or a hang left.
      for (auto in = fs::directory_iterator(it->path(), e2); !e2 && in != fs::directory_iterator(); in.increment(e2)) {
        if (!old(in->path())) continue;
        std::error_code e3;
        if (fs::remove_all(in->path(), e3) > 0) ++gone;
      }
      continue;
    }
    if (!old(it->path())) continue;
    if (fs::remove_all(it->path(), e2) > 0) ++gone;
  }
  if (gone > 0) std::cerr << "goober-host: tmp: removed " << gone << " item(s) older than limits.tmp-hours\n";
  return gone;
}

bj::object Host::limits_report(bool name_tenants) {
  auto cfg = config_.get();
  const auto& d = cfg->disk;
  bj::object out;
  // What nears or passes a budget. Operators read `near`; /goober/ready is public, so its words
  // are ratios only (`near_public`): no bytes, no counts, no tenant or box.
  bj::array near, near_public;
  auto warn = [&](const std::string& text, const std::string& public_text) {
    near.push_back(bj::value(text));
    near_public.push_back(bj::value(public_text));
  };
  auto pct = [](int64_t used, int64_t limit) { return std::to_string(limit > 0 ? used * 100 / limit : 0) + "%"; };
  auto add = [&](const std::string& name, int64_t used, int64_t limit, const char* key, bj::object more = {}) {
    more["used_bytes"] = used;
    more["limit_bytes"] = limit;
    more["share"] = limit > 0 ? std::round(double(used) * 1000 / double(limit)) / 1000 : 0.0;
    more["key"] = std::string("limits.") + key;
    if (limit > 0 && used * 10 >= limit * 8) {
      more["warn"] = true;
      std::string text = name + " at " + pct(used, limit) + " of limits." + key;
      warn(text, text);
    }
    out[name] = std::move(more);
  };
  add("tries", bytes_under(cfg->storage / "tmp" / "tries") + bytes_under(cfg->storage / "tmp" / "upgrades") +
                   bytes_under(cfg->storage / "tmp" / "jobs"),
      d.try_bytes, "try-bytes", bj::object{{"note", "copies of the tries running now; each try alone is held to the limit"}});
  add("exports", bytes_under(cfg->storage / "exports"), d.exports_bytes, "exports-bytes");
  // The size the traffic loop's last catch_up measured: never waits on the store.
  add("traffic", traffic_ ? traffic_->size_bytes() : 0, d.traffic_bytes, "traffic-bytes",
      bj::object{{"dropped_hours", traffic_ ? traffic_->budget_drops() : 0}});
  // Snapshots are a person's to delete: the fullest tenant is reported, and every tenant near.
  {
    int64_t worst = 0;
    std::string worst_tenant;
    int64_t worst_files = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(cfg->storage / "snapshots", ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
      if (!it->is_directory()) continue;
      int64_t bytes = 0, files = 0;
      std::error_code e2;
      for (auto in = fs::recursive_directory_iterator(it->path(), e2); !e2 && in != fs::recursive_directory_iterator();
           in.increment(e2)) {
        std::error_code e3;
        if (in->is_regular_file(e3) && in->path().extension() == ".db") {
          bytes += int64_t(in->file_size(e3));
          ++files;
        }
      }
      std::string tenant = it->path().filename().string();
      if (files * 10 >= d.snapshot_files * 8) {
        warn("tenant " + tenant + "'s snapshots: " + std::to_string(files) + " of limits.snapshot-files (" +
                 std::to_string(d.snapshot_files) + ")",
             "a tenant's snapshots at " + pct(files, d.snapshot_files) + " of limits.snapshot-files");
      }
      if (bytes >= worst) {
        worst = bytes;
        worst_tenant = tenant;
        worst_files = files;
      }
    }
    add("snapshots", worst, d.snapshot_bytes, "snapshot-bytes",
        bj::object{{"tenant", worst_tenant}, {"files", worst_files}, {"files_limit", d.snapshot_files},
                   {"note", "per tenant: the fullest; past the limit db_snapshot refuses and a person deletes old ones"}});
  }
  {
    // How fast they grow (032 § 10): the projection in 7 days, and a log far past its usual day,
    // as the traffic loop last computed them (readiness never computes them).
    bj::object more{{"note", "rotated files leave after 7 days; the budget is reported, not yet enforced"}};
    auto v = log_velocity_report("7d", "", true);
    if (const auto* disk = v.if_contains("disk"); disk && disk->is_object()) {
      for (const char* k : {"per_day_bytes", "projected_bytes", "archive_queued_bytes", "projected_share"}) {
        if (const auto* x = disk->as_object().if_contains(k)) more[k] = *x;
      }
    }
    if (const auto* ws = v.if_contains("warnings"); ws && ws->is_array()) {
      for (const auto& w : ws->as_array()) {
        const auto& o = w.as_object();
        std::string log(o.at("log").as_string());
        std::string pub;
        if (o.at("kind") == "grew") {
          // A box's log is named only for operators.
          pub = (log.starts_with("boxes/") ? std::string("a box's log") : log) + " grew " + json::dump(o.at("growth")) +
                " times its usual day (log_velocity says more)";
        } else {
          double share = more.contains("projected_share") ? more.at("projected_share").to_number<double>() : 0;
          pub = "at today's rate the logs would reach " + std::to_string(int64_t(share * 100)) +
                "% of limits.logs-bytes within 7 days (log_velocity says more)";
        }
        warn(std::string(o.at("text").as_string()), pub);
      }
    }
    add("logs", bytes_on_disk(cfg->storage / "logs"), d.logs_bytes, "logs-bytes", std::move(more));
  }
  // What backups put in the bucket today (019 § 9): past the budget, log archives, the source
  // mirror, and increments wait for the next day.
  if (backups_ && cfg->backup.enabled() && !cfg->sandbox && d.upload_bytes_per_day > 0) {
    try {
      add("uploads", backups_->uploaded_today(base::now_unix_ms()), d.upload_bytes_per_day, "upload-bytes-per-day",
          bj::object{{"note", "today (UTC) in the backup bucket; past it, only the nightly, attachments, and log archives "
                              "waiting a week go up until tomorrow"}});
    } catch (const std::exception&) {
    }
  }
  if (heat_) {
    add("studies", heat_->bytes(), d.studies_bytes, "studies-bytes",
        bj::object{{"note", "heat study reports, kept 30 days; past the budget the oldest finished one goes"}});
  }
  add("skills", skills_.bytes(), d.skills_bytes, "skills-bytes",
      bj::object{{"note", "skills and playbooks, kept until an operator deletes one; past the budget skill_put refuses to grow them"}});
  add("tmp", bytes_under(cfg->storage / "tmp"), d.try_bytes * 2, "try-bytes",
      bj::object{{"note", "swept after limits.tmp-hours; budgeted as twice limits.try-bytes"}});
  // /goober/ready is public: each budget as its share and whether it warns, and the warnings as
  // ratios. Local copies (no budget) and the disk (its own check) are for operators.
  if (!name_tenants) {
    bj::object pub;
    for (const auto& kv : out) {
      const auto& row = kv.value().as_object();
      bj::object r{{"share", row.at("share")}};
      if (row.contains("warn")) r["warn"] = true;
      pub[kv.key()] = std::move(r);
    }
    if (!near_public.empty()) pub["near"] = std::move(near_public);
    return pub;
  }
  // The nightly copies setup's timer makes beside storage (046 A12): told, not budgeted. The
  // host doesn't own them; `goober-host backup` keeps the newest 3 (S3 on) or 14 (off).
  {
    fs::path dir = local_copies_dir(cfg->storage);
    auto copies = local_copies(dir);
    int keep = local_copies_to_keep(cfg->backup.enabled());
    out["local-copies"] = bj::object{
        {"used_bytes", copies.bytes},
        {"copies", copies.copies},
        {"keep", keep},
        {"dir", dir.string()},
        {"note", "nightly copies of storage beside it (backups/YYYY-MM-DD/); goober-host backup keeps the newest " +
                     std::to_string(keep) + (cfg->backup.enabled() ? " (S3 backups on)" : " (S3 backups off)") +
                     "; no budget: the host doesn't own them, but they share the disk the disk check reads"}};
  }
  std::error_code ec;
  auto space = fs::space(cfg->storage, ec);
  if (!ec) {
    out["disk"] = bj::object{{"free_bytes", int64_t(space.available)}, {"total_bytes", int64_t(space.capacity)},
                             {"free_limit_bytes", d.free_bytes}, {"key", "limits.free-bytes"}};
  }
  if (!near.empty()) out["near"] = std::move(near);
  return out;
}

}  // namespace goober::host
