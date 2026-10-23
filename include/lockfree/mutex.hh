/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef LOCKFREE_MUTEX_HH
#define LOCKFREE_MUTEX_HH
// A sleeping mutex that is released on unlock, not handed over.
//
// unlock() clears the lock and wakes the oldest waiter, which then competes
// for the lock again with any thread that is running. A lock is therefore
// never owned by a thread that is not running, so a short critical section
// costs a short wait even when many threads contend for it.
//
// Competing again can starve a waiter, so a waiter that has waited for more
// than a millisecond asks for the lock to be handed over: it sets HANDOFF and
// queues at the head, and the next unlock() gives the lock directly to it.
//
// The state word carries LOCKED, WAITERS and HANDOFF. A lock() or try_lock()
// takes an unlocked state with one compare-and-swap, and an unlock() with no
// waiter releases it with one. Everything else, the queue of waiters
// included, is done under a small spin lock that is held only for a few
// instructions and never while sleeping.

#include <atomic>

// we don't want to include <sched.hh> because that includes a bunch of things
// which eventually, recursively, use mutexes.
// We also can't include <osv/wait_record.hh>, as that includes <sched.hh>.
namespace sched {
    class thread;
}
struct wait_record;

namespace lockfree {

class mutex {
protected:
    std::atomic<int> state;
    // "owner" and "depth" are only used for implementing a recursive mutex.
    // "depth" is not an atomic variable - only the lock-owning thread sets
    // and reads its own depth. "owner" is atomic - one thread doing lock()
    // needs to read the current owner possibly set by another thread - but
    // it can be accessed with relaxed memory ordering.
    unsigned int depth;
    std::atomic<sched::thread *> owner;
    // The waiters, oldest first, under queue_lock.
    wait_record *head;
    wait_record *tail;
    std::atomic<unsigned int> queue_lock;
public:
    // Note: mutex's constructor just initializes the whole structure to
    // zero, and its destructor does nothing. This is useful to know when
    // allocating a mutex in C.
    constexpr mutex() : state(0), depth(0), owner(nullptr), head(nullptr), tail(nullptr), queue_lock(0) { }
    ~mutex() { /*assert(state==0);*/ }

    void lock();
    bool try_lock();
    void unlock();

    bool owned() const;
    sched::thread *get_owner();
    // getdepth() should only be used by the thread holding the lock
    inline unsigned int getdepth() const { return depth; }

private:
    void lock_slow(sched::thread *current);
    void unlock_slow();
    void lock_queue();
    void unlock_queue();
};

}
#endif
