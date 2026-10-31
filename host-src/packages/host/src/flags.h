#pragma once

// Box flags (design/machine/039.Flags.md), private to the host package: values an operator sets
// per tenant and box, kept as storage/flags/$tenant/$box.json written through the host WAL, so
// the change log shows every flip with its before and after, backups carry them, and a restart
// keeps them. A box reads them through goober_flag and can't write them.

#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "goober/json/json.h"
#include "goober/wal/wal.h"

namespace goober::host {

class FlagStore {
 public:
  explicit FlagStore(wal::HostWal& wal) : wal_(wal) {}
  // Everything set for a tenant's box: {flag: {"value", "by", "why", "at"}}.
  boost::json::object of(const std::string& tenant, const std::string& box);
  // The value set, or nullopt (the manifest's default applies).
  std::optional<std::string> value(const std::string& tenant, const std::string& box, const std::string& flag);
  bool set(const std::string& tenant, const std::string& box, const std::string& flag, const std::string& value,
           const std::string& by, const std::string& why, int64_t now_ms, std::string* error);

 private:
  std::mutex mu_;
  wal::HostWal& wal_;
  std::map<std::string, boost::json::object> cache_;  // "tenant/box"
  boost::json::object& load(const std::string& tenant, const std::string& box);  // with mu_ held
};

}  // namespace goober::host
