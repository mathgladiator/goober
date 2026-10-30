#include "goober/bus/bus.h"

#include <algorithm>
#include <memory>

namespace goober::bus {

Bus::Bus(int64_t seed, size_t max_topics) : next_(seed), max_topics_(max_topics) {}

int64_t Bus::publish(const std::string& topic, int64_t now_ms) {
  std::vector<std::shared_ptr<Wake>> wake;
  int64_t seq = 0;
  {
    std::lock_guard lock(mu_);
    seq = ++next_;
    auto& s = topics_[topic];
    s.seq = seq;
    s.at_ms = now_ms;
    ++s.publishes;
    if (topics_.size() > max_topics_) {
      // Forget the topic published longest ago (never this one): it reads as 0 again, which
      // only means a subscriber that hears of it later evaluates once more.
      auto oldest = topics_.end();
      for (auto it = topics_.begin(); it != topics_.end(); ++it) {
        if (it->first != topic && (oldest == topics_.end() || it->second.at_ms < oldest->second.at_ms)) oldest = it;
      }
      if (oldest != topics_.end()) topics_.erase(oldest);
    }
    auto [b, e] = by_topic_.equal_range(topic);
    for (auto it = b; it != e; ++it) {
      if (auto w = watches_.find(it->second); w != watches_.end()) wake.push_back(w->second.second);
    }
  }
  for (const auto& w : wake) (*w)(topic, seq);
  return seq;
}

int64_t Bus::seq(const std::string& topic) const {
  std::lock_guard lock(mu_);
  auto it = topics_.find(topic);
  return it == topics_.end() ? 0 : it->second.seq;
}

int64_t Bus::max_seq(const std::vector<std::string>& topics) const {
  std::lock_guard lock(mu_);
  int64_t out = 0;
  for (const auto& t : topics) {
    if (auto it = topics_.find(t); it != topics_.end()) out = std::max(out, it->second.seq);
  }
  return out;
}

uint64_t Bus::watch(const std::vector<std::string>& topics, Wake wake) {
  std::lock_guard lock(mu_);
  uint64_t id = next_watch_++;
  watches_[id] = {topics, std::make_shared<Wake>(std::move(wake))};
  for (const auto& t : topics) by_topic_.emplace(t, id);
  return id;
}

void Bus::unwatch(uint64_t id) {
  std::lock_guard lock(mu_);
  auto it = watches_.find(id);
  if (it == watches_.end()) return;
  for (const auto& t : it->second.first) {
    auto [b, e] = by_topic_.equal_range(t);
    for (auto x = b; x != e; ++x) {
      if (x->second == id) {
        by_topic_.erase(x);
        break;
      }
    }
  }
  watches_.erase(it);
}

std::vector<Bus::Topic> Bus::topics(const std::string& prefix, size_t limit) const {
  std::lock_guard lock(mu_);
  std::vector<Topic> out;
  for (auto it = topics_.lower_bound(prefix); it != topics_.end() && it->first.starts_with(prefix); ++it) {
    out.push_back(Topic{it->first, it->second.seq, it->second.at_ms, it->second.publishes, by_topic_.count(it->first)});
  }
  std::sort(out.begin(), out.end(), [](const Topic& a, const Topic& b) { return a.at_ms > b.at_ms; });
  if (out.size() > limit) out.resize(limit);
  return out;
}

size_t Bus::watchers() const {
  std::lock_guard lock(mu_);
  return watches_.size();
}

}  // namespace goober::bus
