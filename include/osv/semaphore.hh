/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef SEMAPHORE_HH_
#define SEMAPHORE_HH_

#include <atomic>
#include <osv/spinlock.h>
#include <boost/intrusive/list.hpp>
#include <osv/sched.hh>
#include <osv/wait_record.hh>

// A counting semaphore whose post() and wait() touch one atomic word unless
// somebody has to sleep. The word holds the value in its low half and the
// number of sleeping waiters in its high half, so a post() learns from the
// same atomic add that made its units visible whether anybody needs waking;
// with nobody asleep it does not touch the semaphore again.
class semaphore {
public:
    explicit semaphore(unsigned val);
    virtual ~semaphore() {}
    virtual void post(unsigned units = 1);

    bool wait(unsigned units = 1, sched::timer* tmr = nullptr);
    bool trywait(unsigned units = 1);
protected:
    std::atomic<uint64_t> _state;
    // The sleeping waiters, oldest first.
    spinlock_t _lock;

    struct wait_record : boost::intrusive::list_base_hook<>, waiter {
        explicit wait_record(sched::thread* t, unsigned units) : waiter(t), units(units) {}
        unsigned units;
        // Set by post(), under _lock, once it has taken the units for us.
        bool granted = false;
    };
    using waiter_list = boost::intrusive::list<wait_record,
                          boost::intrusive::base_hook<wait_record>,
                          boost::intrusive::constant_time_size<false>>;
    waiter_list _waiters;

    void wake_waiters();
};

#endif /* SEMAPHORE_HH_ */
