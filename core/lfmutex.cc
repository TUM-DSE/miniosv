/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/clock.hh>
#include <lockfree/mutex.hh>
#include <osv/trace.hh>
#include <osv/sched.hh>
#include <osv/wait_record.hh>
#include <osv/export.h>

namespace lockfree {

TRACEPOINT(trace_mutex_lock, "%p", mutex *);
TRACEPOINT(trace_mutex_lock_wait, "%p", mutex *);
TRACEPOINT(trace_mutex_lock_wake, "%p", mutex *);
TRACEPOINT(trace_mutex_try_lock, "%p, success=%d", mutex *, bool);
TRACEPOINT(trace_mutex_unlock, "%p", mutex *);

namespace {

constexpr int LOCKED = 1;
// Somebody is queued: unlock() must take the slow path and wake one.
constexpr int WAITERS = 2;
// The queue's head has waited too long: unlock() hands the lock to it.
constexpr int HANDOFF = 4;

// How long a waiter competes for the lock before asking for it.
constexpr auto starvation = std::chrono::milliseconds(1);

struct mutex_waiter : wait_record {
    explicit mutex_waiter(sched::thread *t) : wait_record(t) { }
    // Set by unlock() before the wake when it hands the lock over.
    bool handed = false;
};

}

void mutex::lock_queue()
{
    sched::preempt_disable();
    while (queue_lock.exchange(1, std::memory_order_acquire)) {
        while (queue_lock.load(std::memory_order_relaxed)) {
            __builtin_ia32_pause();
        }
    }
}

void mutex::unlock_queue()
{
    queue_lock.store(0, std::memory_order_release);
    sched::preempt_enable();
}

void mutex::lock()
{
    trace_mutex_lock(this);

    sched::thread *current = sched::thread::current();

    // Unlocked, waiters or not: take it. Waking waiters compete like anybody.
    int s = state.load(std::memory_order_relaxed);
    while (!(s & LOCKED)) {
        if (state.compare_exchange_weak(s, s | LOCKED, std::memory_order_acquire)) {
            owner.store(current, std::memory_order_relaxed);
            depth = 1;
            return;
        }
    }

    if (owner.load(std::memory_order_relaxed) == current) {
        ++depth;
        return;
    }

    lock_slow(current);
}

void mutex::lock_slow(sched::thread *current)
{
    osv::clock::uptime::time_point first{};
    bool starving = false;
    while (true) {
        mutex_waiter waiter(current);
        lock_queue();
        int s = state.load(std::memory_order_relaxed);
        if (!(s & LOCKED)) {
            bool taken = state.compare_exchange_strong(s, s | LOCKED, std::memory_order_acquire);
            unlock_queue();
            if (taken) {
                break;
            }
            continue;
        }
        // Only the owner's one-step unlock can change a LOCKED state outside the queue lock, and only while no
        // WAITERS is set; failing here means the lock was just released.
        bool to_head = starving && !(s & HANDOFF);
        int want = s | WAITERS | (to_head ? HANDOFF : 0);
        if (want != s && !state.compare_exchange_strong(s, want, std::memory_order_relaxed)) {
            unlock_queue();
            continue;
        }
        if (to_head) {
            waiter.next = head;
            head = &waiter;
            if (!tail) {
                tail = &waiter;
            }
        } else {
            if (tail) {
                tail->next = &waiter;
            } else {
                head = &waiter;
            }
            tail = &waiter;
        }
        unlock_queue();

        trace_mutex_lock_wait(this);
        auto t0 = osv::clock::uptime::now();
        if (first == osv::clock::uptime::time_point{}) {
            first = t0;
        }
        waiter.wait();
        auto t1 = osv::clock::uptime::now();
        trace_mutex_lock_wake(this);
        if (waiter.handed) {
            break;
        }
        starving = t1 - first >= starvation;
    }
    owner.store(current, std::memory_order_relaxed);
    depth = 1;
}

bool mutex::try_lock()
{
    sched::thread *current = sched::thread::current();
    int s = state.load(std::memory_order_relaxed);
    while (!(s & LOCKED)) {
        if (state.compare_exchange_weak(s, s | LOCKED, std::memory_order_acquire)) {
            owner.store(current, std::memory_order_relaxed);
            depth = 1;
            trace_mutex_try_lock(this, true);
            return true;
        }
    }

    // We're implementing a recursive mutex - lock may still succeed if
    // this thread is the one holding it.
    if (owner.load(std::memory_order_relaxed) == current) {
        ++depth;
        trace_mutex_try_lock(this, true);
        return true;
    }

    trace_mutex_try_lock(this, false);
    return false;
}

void mutex::unlock()
{
    trace_mutex_unlock(this);

    // We assume unlock() is only ever called when this thread is holding
    // the lock. The following assertions don't seem to add any measurable
    // performance penalty, so we leave them in.
    assert(owner.load(std::memory_order_relaxed) == sched::thread::current());
    assert(depth!=0);
    if (--depth)
        return; // recursive mutex still locked.

    // When we return from unlock(), we will no longer be holding the lock.
    // We can't leave owner==current, otherwise a later lock() in the same
    // thread will think it's a recursive lock, while another thread may
    // have taken the lock in the meantime.
    owner.store(nullptr, std::memory_order_relaxed);

    int s = LOCKED;
    if (state.compare_exchange_strong(s, 0, std::memory_order_release)) {
        return;
    }
    unlock_slow();
}

void mutex::unlock_slow()
{
    lock_queue();
    int s = state.load(std::memory_order_relaxed);
    auto *w = static_cast<mutex_waiter *>(head);
    if (w) {
        head = w->next;
        if (!head) {
            tail = nullptr;
        }
    }
    int rest = head ? WAITERS : 0;
    if (w && (s & HANDOFF)) {
        w->handed = true;
        state.store(LOCKED | rest, std::memory_order_release);
    } else {
        state.store(rest, std::memory_order_release);
    }
    unlock_queue();
    // The waiter cannot return before this wake, so w is still valid.
    if (w) {
        w->wake();
    }
}

bool mutex::owned() const
{
    return owner.load(std::memory_order_relaxed) == sched::thread::current();
}

sched::thread *mutex::get_owner()
{
    return owner.load(std::memory_order_relaxed);
}
}

// For use in C, which can't access namespaces or methods. Note that for
// performance, we use "flatten" which basically creates a copy of each
// function.
// TODO: We can avoid this copy by make these C functions the only copy of the
// functions, and make the methods inline, calling these functions with
// "this" as a parameter. But this would be un-C++-like :(
extern "C" {
OSV_LIBSOLARIS_API __attribute__((flatten)) void lockfree_mutex_lock(void *m) {
    static_cast<lockfree::mutex *>(m)->lock();
}
OSV_LIBSOLARIS_API __attribute__ ((flatten)) void lockfree_mutex_unlock(void *m) {
    static_cast<lockfree::mutex *>(m)->unlock();
}
OSV_LIBSOLARIS_API __attribute__ ((flatten)) bool lockfree_mutex_try_lock(void *m) {
    return static_cast<lockfree::mutex *>(m)->try_lock();
}
OSV_LIBSOLARIS_API __attribute__ ((flatten)) bool lockfree_mutex_owned(void *m) {
    return static_cast<lockfree::mutex *>(m)->owned();
}
}
