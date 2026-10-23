/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/semaphore.hh>

namespace {

constexpr uint64_t one_waiter = uint64_t(1) << 32;

inline unsigned value(uint64_t s) { return static_cast<uint32_t>(s); }
inline unsigned sleepers(uint64_t s) { return s >> 32; }

}

semaphore::semaphore(unsigned val)
    : _state(val)
{
}

void semaphore::post(unsigned units)
{
    if (sleepers(_state.fetch_add(units))) {
        wake_waiters();
    }
}

// Hand the units to the oldest waiters they cover, then wake those outside
// the lock. A waiter that asked for more than what is left is skipped, so a
// smaller request behind it can still be served.
void semaphore::wake_waiters()
{
    waiter_list woken;
    WITH_LOCK(_lock) {
        auto i = _waiters.begin();
        while (i != _waiters.end()) {
            auto& wr = *i++;
            uint64_t s = _state.load();
            while (value(s) >= wr.units &&
                   !_state.compare_exchange_weak(s, s - wr.units - one_waiter)) {
            }
            if (value(s) < wr.units) {
                if (!value(s)) {
                    break;
                }
                continue;
            }
            wr.granted = true;
            _waiters.erase(_waiters.iterator_to(wr));
            woken.push_back(wr);
        }
    }
    while (!woken.empty()) {
        auto& wr = woken.front();
        woken.pop_front();
        // wr may be gone once woken.
        wr.wake();
    }
}

bool semaphore::wait(unsigned units, sched::timer* tmr)
{
    if (trywait(units)) {
        return true;
    }

    wait_record wr(sched::thread::current(), units);
    WITH_LOCK(_lock) {
        // Count ourselves as asleep before the last look at the value: a
        // post() adding units after this sees us and wakes us, one before it
        // left its units for this look.
        uint64_t s = _state.fetch_add(one_waiter) + one_waiter;
        while (value(s) >= units) {
            if (_state.compare_exchange_weak(s, s - units - one_waiter)) {
                return true;
            }
        }
        _waiters.push_back(wr);
    }

    wr.wait(tmr);
    if (wr.woken()) {
        return true;
    }

    WITH_LOCK(_lock) {
        if (!wr.granted) {
            _waiters.erase(_waiters.iterator_to(wr));
            _state.fetch_sub(one_waiter);
            return false;
        }
    }
    // A post() took the units for us just as the timer fired; it wakes us
    // next, and wr must outlive that.
    wr.wait();
    return true;
}

bool semaphore::trywait(unsigned units)
{
    uint64_t s = _state.load(std::memory_order_relaxed);
    while (value(s) >= units) {
        if (_state.compare_exchange_weak(s, s - units, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}
