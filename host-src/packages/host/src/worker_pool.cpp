#include "goober/host/worker_pool.h"

#include <pthread.h>
#include <sched.h>

namespace goober::host {

WorkerPool::WorkerPool(int threads, bool pin) {
  unsigned cores = std::thread::hardware_concurrency();
  for (int i = 0; i < threads; ++i) {
    threads_.emplace_back([this] { run(); });
    if (pin && cores > 0) {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(unsigned(i) % cores, &set);
      pthread_setaffinity_np(threads_.back().native_handle(), sizeof(set), &set);
    }
  }
}

WorkerPool::~WorkerPool() {
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) t.join();
}

void WorkerPool::post(std::function<void()> job) {
  {
    std::lock_guard lock(mu_);
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

size_t WorkerPool::queued() const {
  std::lock_guard lock(mu_);
  return jobs_.size();
}

void WorkerPool::run() {
  while (true) {
    std::function<void()> job;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}

}  // namespace goober::host
