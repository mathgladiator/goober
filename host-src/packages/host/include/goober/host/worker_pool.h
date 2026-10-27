#pragma once

#include <condition_variable>
#include <cstdint>
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

  // Queues a job, unless `max_queued` jobs are already waiting; then returns false, so the
  // caller can answer 503 rather than let a flood grow the queue without bound.
  bool post(std::function<void()> job, size_t max_queued = SIZE_MAX);
  size_t queued() const;
  // Drops the jobs still waiting, lets the running ones finish, and joins the threads.
  // Nothing posted afterwards runs. Safe to call twice.
  void shutdown();
  // Jobs waiting or running: zero means the pool is idle.
  size_t busy() const;

 private:
  void run();
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  size_t running_ = 0;
  std::vector<std::thread> threads_;
};

}  // namespace goober::host
