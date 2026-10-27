#include "goober/host/worker_pool.h"

#include <pthread.h>
#include <sched.h>

#include <cstdio>
#include <exception>

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

WorkerPool::~WorkerPool() { shutdown(); }

void WorkerPool::shutdown() {
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
    jobs_.clear();  // never started, so nothing of theirs is half done
  }
  cv_.notify_all();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
}

bool WorkerPool::post(std::function<void()> job, size_t max_queued) {
  {
    std::lock_guard lock(mu_);
    if (stopping_ || jobs_.size() >= max_queued) return false;
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
  return true;
}

size_t WorkerPool::busy() const {
  std::lock_guard lock(mu_);
  return jobs_.size() + running_;
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
      ++running_;
    }
    // A job that throws must not take the thread, or the process, with it.
    try {
      job();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "goober-host: a job failed: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "goober-host: a job failed\n");
    }
    std::lock_guard lock(mu_);
    --running_;
  }
}

}  // namespace goober::host
