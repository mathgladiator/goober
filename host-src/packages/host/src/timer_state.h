#pragma once

// The scheduler's state (scheduler.cpp), private to the host package.

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/sqlite/sqlite.h"

namespace goober::host {

struct Host::TimerState {
  std::mutex mu;
  std::map<std::string, int64_t> next_due;     // tenant|timer|row -> when it's next due
  std::map<std::string, std::string> plan;     // what each next_due was computed from: schedule and zone
  std::map<std::string, int64_t> retry_after;  // a failed fire's backoff
  std::set<std::string> running;               // fires in flight: the next is skipped (045)
  struct Rows {
    int64_t read_at = 0;
    std::vector<boost::json::object> rows;
  };
  std::map<std::string, Rows> rows;                                // tenant|source
  std::map<std::string, std::unique_ptr<sqlite::Db>> progress;     // tenant -> timers.db
  bool inline_fires = false;
};

}  // namespace goober::host
