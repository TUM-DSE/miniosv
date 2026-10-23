/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/condvar.h>
#include <errno.h>
#include <osv/trace.hh>
#include <osv/wait_record.hh>
#include <osv/export.h>
#include <osv/kernel_config.h>

TRACEPOINT(trace_condvar_wait, "%p", condvar *);
TRACEPOINT(trace_condvar_wake_one, "%p", condvar *);
TRACEPOINT(trace_condvar_wake_all, "%p", condvar *);

int condvar::wait(mutex* user_mutex, sched::timer* tmr)
{
    trace_condvar_wait(this);
    int ret = 0;
    wait_record wr(sched::thread::current());

    _m.lock();
    if (!_waiters_fifo.oldest) {
        _waiters_fifo.oldest = &wr;
    } else {
        _waiters_fifo.newest->next = &wr;
    }
    _waiters_fifo.newest = &wr;
    // Remember user_mutex for "wait morphing" feature. Assert our assumption
    // that concurrent waits use the same mutex.
    assert(!_user_mutex || _user_mutex == user_mutex);
    _user_mutex = user_mutex;
    // This preempt_disable() is just an optimization, to avoid context
    // switch between the two unlocks.
#if CONF_lazy_stack_invariant
    assert(arch::irq_enabled());
    assert(sched::preemptable());
#endif
#if CONF_lazy_stack
    arch::ensure_next_stack_page();
#endif
    sched::preempt_disable();
    user_mutex->unlock();
    _m.unlock();
    sched::preempt_enable();

    // Wait until either the timer expires or condition variable signaled
    wr.wait(tmr);
    if (!wr.woken()) {
        ret = ETIMEDOUT;
        // wr is still in the linked list (because of a timeout) so remove it:
        _m.lock();
        if (&wr == _waiters_fifo.oldest) {
            _waiters_fifo.oldest = wr.next;
            if (!wr.next) {
                _waiters_fifo.newest = nullptr;
            }
        } else {
            wait_record *p = _waiters_fifo.oldest;
            while (p) {
                if (&wr == p->next) {
                    p->next = p->next->next;
                    if(!p->next) {
                        _waiters_fifo.newest = p;
                    }
                    break;
                }
                p = p->next;
            }
            if (!p) {
                ret = 0;
            }
        }
        _m.unlock();
        if (!ret) {
            // wr is no longer in the queue, so either wr.wake() is
            // already done, or wake_all() has just taken the whole queue
            // and will wr.wake() soon. We can't return (and invalidate wr)
            // until it calls wr.wake().
            wr.wait();
        }
    }

    // A woken waiter takes the mutex like anybody else: handing it over
    // while we are not yet running would hold up every other thread.
    user_mutex->lock();

    return ret;
}

void condvar::wake_one()
{
    trace_condvar_wake_one(this);
    // To make wake with no waiters faster, and avoid unnecessary contention
    // in that case, first check the queue head outside the lock. If it is not
    // empty, we still need to take the lock, and re-read the head.
    if (!_waiters_fifo.oldest) {
        return;
    }

    _m.lock();
    wait_record *wr = _waiters_fifo.oldest;
    if (wr) {
        _waiters_fifo.oldest = wr->next;
        if (wr->next == nullptr) {
            _waiters_fifo.newest = nullptr;
        }
        wr->wake();
        // To help the assert() in condvar_wait(), we need to zero saved
        // user_mutex when all concurrent condvar_wait()s are done.
        if (!_waiters_fifo.oldest) {
            _user_mutex = nullptr;
        }
    }
    _m.unlock();
}

void condvar::wake_all()
{
    trace_condvar_wake_all(this);
    if (!_waiters_fifo.oldest) {
        return;
    }

    _m.lock();
    wait_record *wr = _waiters_fifo.oldest;

    // To help the assert() in condvar_wait(), we need to zero saved
    // user_mutex when all concurrent condvar_wait()s are done.
    _user_mutex = nullptr;

    _waiters_fifo.oldest = _waiters_fifo.newest = nullptr;
    _m.unlock();
    while (wr) {
        auto next_wr = wr->next; // need to save - *wr invalid after wake
        wr->wake();
        wr = next_wr;
    }
}

extern "C" OSV_LIBSOLARIS_API
int condvar_wait(condvar_t *condvar, mutex_t* user_mutex, uint64_t expiration)
{
    if (expiration) {
        return condvar->wait(user_mutex, osv::clock::uptime::time_point(
            std::chrono::nanoseconds(expiration)));
    } else {
        return condvar->wait(user_mutex);
    }
}

OSV_LIBSOLARIS_API
void condvar_wake_one(condvar_t *condvar)
{
    condvar->wake_one();
}

OSV_LIBSOLARIS_API
void condvar_wake_all(condvar_t *condvar)
{
    condvar->wake_all();
}
