#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace goober::host {

// The box threads: a fixed pool that runs jobs off the network threads.
// With `pin`, thread i is pinned to core i modulo the core count.
class WorkerPool {
 public:
  WorkerPool(int threads, bool pin);
  ~WorkerPool();
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  void post(std::function<void()> job);
  size_t queued() const;

 private:
  void run();
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  std::vector<std::thread> threads_;
};

}  // namespace goober::host
