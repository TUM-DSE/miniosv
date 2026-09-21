/*
 * Workers: threads that exist to run one task's iteration on one core.
 *
 * A task has one main worker for its whole life. At every iteration boundary
 * it takes the assignment the scheduler last decided, moves itself onto one
 * of those cores if it is not on one already, and runs the engine's iterate()
 * there. The helper workers for the other cores of the assignment exist only
 * inside run_on_workers(): created for the call, pinned, joined at its end.
 */

#include <osv/sched.hh>

#include "include/lros.hh"
#include "internal.hh"

namespace lros {

namespace {

worker_stats wstats;

// The task whose iteration this thread is running. Set around iterate() so
// that a compute library called from inside it lands on the task's cores
// without the engine having to thread the assignment through.
__thread task *tls_current_task;

sched::cpu *cpu_of_bit(cpu_mask bit)
{
    return sched::cpus[__builtin_ctzll(bit)];
}

// Give this task's inference context back, if the scheduler has asked for it
// and the engine can. Called between iterations and while paused, never inside
// one: the context is being freed under the thread that would otherwise be
// computing in it.
void honour_kv_eviction(task *t)
{
    bool asked = false;
    WITH_LOCK(lock) {
        asked = t->kv_evict_asked && t->kv_charged > 0;
        t->kv_evict_asked = false;
    }
    if (!asked || !ops.evict_kv) {
        return;
    }
    size_t saved = 0;
    const size_t freed = ops.evict_kv(*t, &saved);
    if (freed == 0) {
        return;   // the engine kept it; it will be asked again
    }
    WITH_LOCK(lock) {
        kv_uncharge(*t);
        t->n_kv_evictions++;
    }
    kv_saved_add(saved);
}

// A paused task waits here until the scheduler gives it cores again, giving
// its context back in the meantime if asked to.
void wait_for_cores(task *t)
{
    while (true) {
        honour_kv_eviction(t);
        WITH_LOCK(lock) {
            while (!t->next.cpus && !t->kv_evict_asked) {
                decided.wait(lock);
            }
            if (t->next.cpus) {
                return;
            }
        }
    }
}

void main_loop(task *t)
{
    while (true) {
        honour_kv_eviction(t);

        assignment a;
        uint32_t rt = 0;
        WITH_LOCK(lock) {
            a = take_assignment(t);
            rt = t->rt_prio;
        }
        sched::thread *self = sched::thread::current();
        if (self->realtime_priority() != rt) {
            self->set_realtime_priority(rt);
        }
        if (a.cpus == 0) {
            wait_for_cores(t);
            continue;
        }

        // The main worker runs on the lowest core of the assignment.
        sched::cpu *home = cpu_of_bit(a.cpus & -a.cpus);
        if (sched::cpu::current() != home) {
            sched::thread::pin(home);
        }

        tls_current_task = t;
        const iter_result r = ops.iterate(*t, a);
        tls_current_task = nullptr;
        t->n_iterations++;

        switch (r) {
        case iter_result::running:
            continue;
        case iter_result::phase_changed:
            task_phase_changed(t);
            continue;
        case iter_result::done:
        case iter_result::failed:
            task_done(t);     // frees t
            core_freed();
            return;
        }
    }
}

struct helper_arg {
    void (*fn)(void *, int32_t, int32_t);
    void *arg;
    int32_t worker, n;
};

} // namespace

void worker_start(task *t)
{
    sched::cpu *home = cpu_of_bit(t->current.cpus & -t->current.cpus);
    const uint64_t t0 = now_ns();
    auto *th = sched::thread::make([t] { main_loop(t); },
                                   sched::thread::attr().pin(home).detached());
    th->set_realtime_priority(t->rt_prio);
    th->start();
    wstats.n_created++;
    wstats.create_ns_total += now_ns() - t0;
}

void run_parallel(cpu_mask cpus, int32_t n, void (*fn)(void *, int32_t, int32_t), void *arg)
{
    // A caller may name more cores than the machine has (~0 for "all"); every
    // bit below is dereferenced as sched::cpus[i], so clamp here rather than
    // trusting the mask.
    const size_t n_online = sched::cpus.size();
    cpus &= n_online >= 64 ? ~cpu_mask(0) : ((cpu_mask(1) << n_online) - 1);

    const int32_t n_cores = __builtin_popcountll(cpus);
    if (n <= 0) {
        n = n_cores;
    }
    if (n <= 1 || n_cores == 0) {
        fn(arg, 0, n > 1 ? n : 1);
        return;
    }

    // Worker 0 is the caller, and it has to be on the mask like every other
    // worker. Left where it happened to be, it can land on a core that a
    // helper is pinned to, and then the two of them share one core while the
    // engine's barrier spins: every barrier costs a preemption instead of a
    // few hundred cycles, which at a small graph is most of the run time.
    sched::thread *self = sched::thread::current();
    const bool self_was_pinned = self->pinned();
    sched::cpu *self_prev_cpu = self->tcpu();
    sched::cpu *home = cpu_of_bit(cpus & -cpus);
    if (!self_was_pinned || self_prev_cpu != home) {
        sched::thread::pin(home);
    }

    // One helper per extra worker, placed round-robin over the cores of the
    // mask: n may exceed the cores when the engine asks for more workers than
    // it was given, and two workers on one core is better than a missing one,
    // which would hang ggml's barrier.
    std::vector<sched::thread *> helpers;
    std::vector<helper_arg> args(n);
    helpers.reserve(n - 1);
    const uint64_t t0 = now_ns();
    cpu_mask rest = cpus & ~(cpus & -cpus);
    for (int32_t i = 1; i < n; i++) {
        if (rest == 0) {
            rest = cpus;
        }
        const cpu_mask bit = rest & -rest;
        rest &= ~bit;
        args[i] = { fn, arg, i, n };
        helper_arg *ha = &args[i];
        auto *th = sched::thread::make([ha] { ha->fn(ha->arg, ha->worker, ha->n); },
                                       sched::thread::attr().pin(cpu_of_bit(bit)));
        th->set_realtime_priority(self->realtime_priority());   // the caller's task's
        th->start();
        helpers.push_back(th);
    }
    wstats.n_created += n - 1;
    wstats.create_ns_total += now_ns() - t0;

    fn(arg, 0, n);

    const uint64_t t1 = now_ns();
    for (sched::thread *th : helpers) {
        th->join();
        delete th;
    }
    wstats.join_ns_total += now_ns() - t1;

    // Give the caller back the placement it had, core included: nothing moves
    // an unpinned thread off the core it was borrowed onto.
    if (self_prev_cpu != home) {
        sched::thread::pin(self_prev_cpu);
    }
    if (!self_was_pinned) {
        self->unpin();
    }
}

void run_on_workers(task &t, void (*fn)(void *, int32_t, int32_t), void *arg)
{
    run_parallel(t.current.cpus, t.current.n_cpus(), fn, arg);
}

task *current_task()
{
    return tls_current_task;
}

void run_workers_here(int32_t n, void (*fn)(void *, int32_t, int32_t), void *arg, cpu_mask fallback)
{
    task *t = tls_current_task;
    run_parallel(t ? t->current.cpus : fallback, n, fn, arg);
}

worker_stats get_worker_stats()
{
    return wstats;
}

}
