#pragma once

#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lf {

// Small work-stealing pool for a fixed batch of tasks.
//
// Tasks are dealt round-robin into one deque per worker. A worker takes from the
// front of its own deque; when that runs dry it steals from the back of someone
// else's. Nothing is added once run() starts, so a worker can quit as soon as
// one full pass over all deques comes up empty.
class StealPool {
 public:
  struct alignas(64) WorkerStats {  // own cache line: no false sharing on the counters
    size_t ran = 0;
    size_t stolen = 0;
  };

  explicit StealPool(int threads) : n_(threads < 1 ? 1 : threads), stats_(n_) {}

  int threads() const { return n_; }
  const std::vector<WorkerStats>& stats() const { return stats_; }

  // Calls fn(task, worker) for every task id in `order`. Seeding order matters:
  // pass the biggest tasks first so they start early.
  void run(const std::vector<size_t>& order, const std::function<void(size_t, int)>& fn) {
    if (n_ == 1) {
      for (size_t t : order) {
        fn(t, 0);
        stats_[0].ran++;
      }
      return;
    }
    std::vector<std::unique_ptr<Deque>> dq;
    for (int i = 0; i < n_; i++) dq.push_back(std::make_unique<Deque>());
    for (size_t i = 0; i < order.size(); i++) dq[i % n_]->q.push_back(order[i]);

    std::mutex err_mu;
    std::exception_ptr err;

    auto worker = [&](int w) {
      for (;;) {
        size_t task = 0;
        bool got = false;
        {
          std::lock_guard<std::mutex> lk(dq[w]->mu);
          if (!dq[w]->q.empty()) {
            task = dq[w]->q.front();
            dq[w]->q.pop_front();
            got = true;
          }
        }
        for (int k = 1; k < n_ && !got; k++) {
          Deque& victim = *dq[(w + k) % n_];
          std::lock_guard<std::mutex> lk(victim.mu);
          if (!victim.q.empty()) {
            task = victim.q.back();
            victim.q.pop_back();
            got = true;
            stats_[w].stolen++;
          }
        }
        if (!got) return;
        try {
          fn(task, w);
        } catch (...) {
          std::lock_guard<std::mutex> lk(err_mu);
          if (!err) err = std::current_exception();
        }
        stats_[w].ran++;
      }
    };

    std::vector<std::thread> th;
    for (int w = 1; w < n_; w++) th.emplace_back(worker, w);
    worker(0);  // the calling thread works too
    for (auto& t : th) t.join();
    if (err) std::rethrow_exception(err);
  }

 private:
  struct Deque {
    std::mutex mu;
    std::deque<size_t> q;
  };
  int n_;
  std::vector<WorkerStats> stats_;
};

}  // namespace lf
