#pragma once

// The in-process bus (design/human/040.TheBus.md, design/machine/036.Bus.md § 3): signals, not
// data. A topic is at a sequence number; publishing moves it forward; watchers are told the new
// number and decide for themselves when to look at the data.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace goober::bus {

class Bus {
 public:
  // seed: the first sequence is seed + 1 (the host passes the time in microseconds).
  explicit Bus(int64_t seed, size_t max_topics = 100000);

  // Moves the topic forward and wakes its watchers; answers the new sequence.
  int64_t publish(const std::string& topic, int64_t now_ms);
  // The topic's sequence, or 0 if it was never published (or was forgotten).
  int64_t seq(const std::string& topic) const;
  // The highest sequence among these topics.
  int64_t max_seq(const std::vector<std::string>& topics) const;

  using Wake = std::function<void(const std::string& topic, int64_t seq)>;
  // Calls wake for every publish to any of these topics, until unwatched.
  uint64_t watch(const std::vector<std::string>& topics, Wake wake);
  void unwatch(uint64_t id);

  struct Topic {
    std::string name;
    int64_t seq = 0, at_ms = 0, publishes = 0;
    size_t watchers = 0;
  };
  // Topics whose name starts with prefix, newest publish first, at most limit.
  std::vector<Topic> topics(const std::string& prefix, size_t limit) const;
  size_t watchers() const;

 private:
  struct State {
    int64_t seq = 0, at_ms = 0, publishes = 0;
  };
  mutable std::mutex mu_;
  int64_t next_;
  size_t max_topics_;
  std::map<std::string, State> topics_;
  uint64_t next_watch_ = 1;
  std::map<uint64_t, std::pair<std::vector<std::string>, std::shared_ptr<Wake>>> watches_;
  std::multimap<std::string, uint64_t> by_topic_;
};

}  // namespace goober::bus
