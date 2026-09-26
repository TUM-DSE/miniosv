#pragma once

#include <atomic>
#include <cstring>
#include <minidpdk/util.hh>
#include <osv/sched.hh>
#include <processor.hh>
#include <vector>

// Test-and-set spinlock on the free stack. mininet gives every queue its own
// pool and one pinned worker, so nothing contends here today and the lock is
// one uncontended exchange per push or pop; it is what keeps a pool shared
// across cpus, which minidpdk allows, correct. The critical section is a few
// nanoseconds, so it spins: a sleeping mutex would starve the polling loops,
// which never voluntarily hit a scheduling point.
struct pool_spinlock {
  std::atomic<bool> flag{false};
  void lock() {
    while (flag.exchange(true, std::memory_order_acquire)) {
      while (flag.load(std::memory_order_relaxed)) {
        processor::spin_hint();
      }
    }
  }
  void unlock() { flag.store(false, std::memory_order_release); }
};
struct pool_spin_guard {
  pool_spinlock &l;
  explicit pool_spin_guard(pool_spinlock &l_) : l(l_) { l.lock(); }
  ~pool_spin_guard() { l.unlock(); }
};

struct stack {
  std::vector<void *> objs;
  size_t head = 0;
  pool_spinlock lock;

  stack(size_t size) : objs(size) {}

  static stack *create(size_t size) { return new stack(size); }

  static void destroy(stack *s) { delete s; }

  unsigned int push(void *const *obj_table, unsigned int n) {
    pool_spin_guard g(lock);
    if (unlikely(objs.size() - head < n))
      return 0;
    std::memcpy(&objs[head], obj_table, n * sizeof(void *));
    head += n;
    return n;
  }

  unsigned int pop(void **obj_table, unsigned int n) {
    pool_spin_guard g(lock);
    if (unlikely(head < n))
      return 0;
    for (unsigned i = 0; i < n; ++i)
      obj_table[n - i - 1] = objs[head - n + i];
    head -= n;
    return n;
  }
};
