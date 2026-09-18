/*
 * The queue and the scheduling decision. See include/lros.hh.
 *
 * Everything here runs under one lock and never blocks: schedule() reads the
 * queue and the running set, asks the engine what each task wants, and writes
 * the next assignment of every task. Applying an assignment is the task's own
 * business, at its next iteration boundary (worker.cc).
 */

#include <osv/sched.hh>
#include <osv/mutex.h>

#include <algorithm>
#include <chrono>
#include <climits>

#include "include/lros.hh"
#include "internal.hh"

namespace lros {

mutex lock;
engine_ops ops;
cpu_mask usable_cpus;
int32_t  n_accel_units = 0;   // 0 until a plugin reports otherwise

std::vector<task *> tasks;      // every task not yet done, any state
int32_t next_task_id = 1;
int32_t n_outstanding;          // submitted and not done, for wait_all()

// The budget and what is charged against it. Read outside the lock by
// mem_status(), so the running totals are atomic; the two figures that are
// settled once, before any task exists, are not.
size_t mem_budget_bytes;
size_t weights_limit_bytes;
size_t weights_pinned_bytes;
size_t kv_paged_bytes;
std::atomic<size_t> kv_used_bytes{0};
std::atomic<size_t> kv_saved_bytes{0};

namespace {

constexpr size_t history_size = 512;
task_stats history[history_size];
size_t history_next, history_count;

cpu_mask all_cpus()
{
    const size_t n = sched::cpus.size();
    return n >= 64 ? ~cpu_mask(0) : ((cpu_mask(1) << n) - 1);
}

cpu_mask lowest_bits(cpu_mask from, int32_t n)
{
    cpu_mask out = 0;
    while (n > 0 && from) {
        const cpu_mask bit = from & -from;
        out |= bit;
        from &= ~bit;
        n--;
    }
    return out;
}

bool before(const task *a, const task *b)
{
    if (a->prio != b->prio) {
        return a->prio < b->prio;
    }
    return a->t_submit < b->t_submit;
}

int32_t best_prio(const task *t)
{
    int32_t p = INT32_MAX;
    for (request *r : t->members) {
        p = std::min(p, r->prio);
    }
    for (request *r : t->pending_members) {
        p = std::min(p, r->prio);
    }
    return p;
}

// Batching: per model, let the engine move queued requests into another
// queued task or into a running one. A queued task the engine emptied is
// dropped; its request lives on in the task it joined.
void compose_queued()
{
    if (!ops.compose) {
        return;
    }
    std::vector<int32_t> models;
    for (task *t : tasks) {
        if (t->state == task_state::queued &&
            std::find(models.begin(), models.end(), t->model) == models.end()) {
            models.push_back(t->model);
        }
    }
    for (int32_t m : models) {
        std::vector<task *> queued, running;
        for (task *t : tasks) {
            if (t->model != m) {
                continue;
            }
            if (t->state == task_state::queued) {
                queued.push_back(t);
            } else if (t->state != task_state::done) {
                running.push_back(t);
            }
        }
        if (queued.empty() || (queued.size() < 2 && running.empty())) {
            continue;
        }
        ops.compose(queued, running);
        for (task *t : queued) {
            if (t->members.empty()) {
                tasks.erase(std::find(tasks.begin(), tasks.end(), t));
                n_outstanding--;
                delete t;
            } else {
                t->prio = best_prio(t);
            }
        }
        for (task *t : running) {
            t->prio = best_prio(t);
        }
    }
}

// Ask tasks to give their contexts back until the KV charge is inside its
// share of the budget. Lock held; nothing is freed here -- each task honours
// the request on its own thread, at a boundary.
//
// `spared` is the set that was just given units: taking the context of a task
// that is about to run only means rebuilding it one iteration later, so the
// victims come from what is paused or queued behind it. Worst priority first,
// and among equals the one that has been waiting longest, which is the one
// least likely to want its context back soon.
void reclaim_kv(const std::vector<task *> &order)
{
    if (mem_budget_bytes == 0 || !ops.evict_kv) {
        return;
    }
    const mem_state m = mem_status();
    if (!m.over()) {
        return;
    }
    size_t want = m.kv_used - m.kv_budget();

    // Reverse priority order: the last task the decision reached is the one
    // with the least claim on the machine.
    for (auto it = order.rbegin(); it != order.rend() && want > 0; ++it) {
        task *t = *it;
        if (t->state == task_state::done || t->kv_charged == 0 || t->kv_evict_asked) {
            continue;
        }
        if (t->next.cpus != 0) {
            continue;   // about to run: it would only rebuild what we took
        }
        t->kv_evict_asked = true;
        want = want > t->kv_charged ? want - t->kv_charged : 0;
    }
}

} // namespace

void set_mem_budget(size_t bytes) { mem_budget_bytes = bytes; }

void set_weights_charge(size_t limit, size_t pinned)
{
    weights_limit_bytes  = limit;
    weights_pinned_bytes = pinned;
}

void set_kv_paged_charge(size_t limit) { kv_paged_bytes = limit; }

mem_state mem_status()
{
    mem_state m;
    m.budget         = mem_budget_bytes;
    m.weights_limit  = weights_limit_bytes;
    m.weights_pinned = weights_pinned_bytes;
    m.kv_used        = kv_used_bytes.load(std::memory_order_relaxed);
    m.kv_saved       = kv_saved_bytes.load(std::memory_order_relaxed);
    m.kv_paged       = kv_paged_bytes;
    return m;
}

void kv_charge(task &t, size_t bytes)
{
    kv_used_bytes.fetch_add(bytes, std::memory_order_relaxed);
    t.kv_charged += bytes;
}

void kv_uncharge(task &t)
{
    if (t.kv_charged == 0) {
        return;
    }
    kv_used_bytes.fetch_sub(t.kv_charged, std::memory_order_relaxed);
    t.kv_charged = 0;
}

void kv_saved_add(size_t bytes)
{
    kv_saved_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void kv_saved_sub(size_t bytes)
{
    size_t held = kv_saved_bytes.load(std::memory_order_relaxed);
    kv_saved_bytes.fetch_sub(std::min(held, bytes), std::memory_order_relaxed);
}

// The decision. Lock held.
void schedule()
{
    compose_queued();

    std::vector<task *> order(tasks.begin(), tasks.end());
    std::sort(order.begin(), order.end(), before);

    // Cores are handed out from scratch on every decision: by priority, then
    // by arrival, each task capped at what its phase wants. A running task
    // that loses cores keeps them until its next boundary, so two tasks can
    // overlap on a core for one iteration; the alternative, reserving the
    // running task's cores until it yields them, comes when it is measured
    // to matter.
    // Runnable tasks in priority order, so that each can be told what is
    // still behind it.
    std::vector<task *> runnable;
    for (task *t : order) {
        if (t->state != task_state::done) {
            runnable.push_back(t);
        }
    }

    const uint64_t now = now_ns();


    cpu_mask   free  = usable_cpus;
    accel_mask afree = n_accel_units >= 32 ? ~0u
                                           : (accel_mask) ((1u << n_accel_units) - 1);
    for (size_t i = 0; i < runnable.size(); i++) {
        task *t = runnable[i];

        decision d;
        d.total[(int) domain::cpu]   = __builtin_popcountll(usable_cpus);
        d.avail[(int) domain::cpu]   = __builtin_popcountll(free);
        d.total[(int) domain::accel] = n_accel_units;
        d.avail[(int) domain::accel] = __builtin_popcount(afree);
        d.n_pending = (int32_t) (runnable.size() - i - 1);
        d.worst_pending_prio = INT32_MAX;
        d.max_pending_pause_ns = 0;
        d.this_pause_ns = t->t_paused_since ? now - t->t_paused_since : 0;
        d.mem = mem_status();
        for (size_t j = i + 1; j < runnable.size(); j++) {
            task *o = runnable[j];
            d.worst_pending_prio = std::max(d.worst_pending_prio == INT32_MAX ? o->prio : d.worst_pending_prio, o->prio);
            const uint64_t paused = o->t_paused_since ? now - o->t_paused_since : 0;
            d.max_pending_pause_ns = std::max(d.max_pending_pause_ns, paused);
        }

        width w;
        if (ops.width_of) {
            w = ops.width_of(*t, d);
        } else {
            w[domain::cpu] = d.n_cores();   // absent: every core, no accelerator
        }
        t->wanted = w[domain::cpu];

        const int32_t give = std::min<int32_t>(w[domain::cpu], __builtin_popcountll(free));
        const cpu_mask decided = lowest_bits(free, give);

        // Accelerator units are handed out the same way, from the low ones up.
        // A device with no addressable partition reports capacity 1, so this
        // degenerates to "the task either holds the device or does not".
        const int32_t agive = std::min<int32_t>(w[domain::accel], __builtin_popcount(afree));
        accel_mask adecided = 0;
        for (int32_t k = 0, taken = 0; k < 32 && taken < agive; k++) {
            if (afree & (1u << k)) { adecided |= (1u << k); taken++; }
        }

        if ((decided != t->next.cpus || adecided != t->next.accels) && t->t_decided == 0) {
            t->t_decided = now_ns();   // the task learns it at its next boundary
        }
        t->next.cpus   = decided;
        t->next.accels = adecided;
        free  &= ~t->next.cpus;
        afree &= ~t->next.accels;

        if (give > 0 && t->state == task_state::queued) {
            t->state = task_state::running;
            t->current = t->next;
            worker_start(t);
        }
    }

    // Every assignment is settled, so it is known which tasks are about to run
    // and which are not; the contexts of the latter are what a tight budget
    // takes back.
    reclaim_kv(runnable);
}

void set_engine(const engine_ops &o)
{
    WITH_LOCK(lock) {
        ops = o;
    }
}

void set_cores(cpu_mask u)
{
    WITH_LOCK(lock) {
        usable_cpus = u;
    }
}

void set_accel_capacity(int32_t n_units)
{
    WITH_LOCK(lock) {
        n_accel_units = n_units < 0 ? 0 : (n_units > 32 ? 32 : n_units);
    }
}

int32_t accel_capacity()
{
    WITH_LOCK(lock) {
        return n_accel_units;
    }
}

int32_t submit(request *r)
{
    task *t = new task();
    t->model = r->model;
    t->members.push_back(r);
    t->ph = phase::prefill;
    t->prio = r->prio;
    t->state = task_state::queued;
    t->wanted = 0;
    t->engine = nullptr;
    t->t_submit = now_ns();
    t->t_first_run = t->t_done = 0;
    t->n_iterations = t->n_reassignments = 0;
    t->max_members = 1;
    t->t_decided = t->decision_lag_ns = 0;
    t->paused_ns = t->t_paused_since = 0;
    t->n_preemptions = 0;
    t->kv_charged = 0;
    t->kv_evict_asked = false;
    t->n_kv_evictions = 0;

    WITH_LOCK(lock) {
        if (usable_cpus == 0) {
            usable_cpus = all_cpus();
        }
        t->id = next_task_id++;
        tasks.push_back(t);
        n_outstanding++;
        schedule();
        return t->id;
    }
}

void submit_after(uint64_t delay_us, request *r)
{
    // A thread that sleeps for the delay and then submits on whichever core
    // it wakes on. Unpinned on purpose: the arrival lands wherever the
    // scheduler puts the wakeup, as an interrupt would.
    auto *th = sched::thread::make([delay_us, r] {
        if (delay_us > 0) {
            sched::thread::sleep(std::chrono::microseconds(delay_us));
        }
        submit(r);
    }, sched::thread::attr().detached());
    th->start();
}

void task_phase_changed(task *t)
{
    WITH_LOCK(lock) {
        schedule();
    }
}

void task_done(task *t)
{
    WITH_LOCK(lock) {
        t->state = task_state::done;
        t->t_done = now_ns();
        kv_uncharge(*t);   // the engine freed the context on its way out

        task_stats &s = history[history_next];
        s.id = t->id;
        s.n_members = t->max_members;
        s.submit_to_first_run_ns = t->t_first_run - t->t_submit;
        s.total_ns = t->t_done - t->t_submit;
        s.n_iterations = t->n_iterations;
        s.n_reassignments = t->n_reassignments;
        s.n_preemptions = t->n_preemptions;
        s.paused_ns = t->paused_ns + (t->t_paused_since ? now_ns() - t->t_paused_since : 0);
        s.decision_lag_ns = t->decision_lag_ns;
        history_next = (history_next + 1) % history_size;
        if (history_count < history_size) {
            history_count++;
        }

        tasks.erase(std::find(tasks.begin(), tasks.end(), t));
        n_outstanding--;
        schedule();
    }
    delete t;
}

void core_freed()
{
    WITH_LOCK(lock) {
        schedule();
    }
}

// Lock held. What the task runs on from now; empty means pause. This is the
// iteration boundary: requests that joined since the last one enter the
// batch here.
assignment take_assignment(task *t)
{
    if (!t->pending_members.empty()) {
        t->members.insert(t->members.end(), t->pending_members.begin(), t->pending_members.end());
        t->pending_members.clear();
    }
    t->max_members = std::max(t->max_members, (int32_t) t->members.size());
    const uint64_t now = now_ns();
    if (t->next.cpus != t->current.cpus) {
        t->n_reassignments++;
        if (t->t_decided) {
            t->decision_lag_ns += now - t->t_decided;
        }
        if (t->next.cpus == 0) {
            t->n_preemptions++;
        }
        t->current = t->next;
    }
    t->t_decided = 0;
    if (t->t_paused_since && t->current.cpus) {
        t->paused_ns += now - t->t_paused_since;
        t->t_paused_since = 0;
    } else if (!t->t_paused_since && !t->current.cpus) {
        t->t_paused_since = now;
    }
    t->state = t->current.cpus ? task_state::running : task_state::paused;
    if (t->t_first_run == 0 && t->current.cpus) {
        t->t_first_run = now_ns();
    }
    return t->current;
}

void wait_all()
{
    while (true) {
        WITH_LOCK(lock) {
            if (n_outstanding == 0) {
                return;
            }
        }
        sched::thread::sleep(std::chrono::milliseconds(1));
    }
}

occupancy get_occupancy()
{
    occupancy o = {};
    WITH_LOCK(lock) {
        o.cores_total = __builtin_popcountll(usable_cpus ? usable_cpus : all_cpus());
        cpu_mask busy = 0;
        for (task *t : tasks) {
            switch (t->state) {
            case task_state::queued:  o.tasks_queued++; break;
            case task_state::running: o.tasks_running++; busy |= t->current.cpus; break;
            case task_state::paused:  o.tasks_paused++; break;
            case task_state::done:    break;
            }
        }
        o.cores_busy = __builtin_popcountll(busy);
    }
    return o;
}

size_t finished_tasks(task_stats *out, size_t max)
{
    WITH_LOCK(lock) {
        const size_t n = std::min(history_count, max);
        const size_t start = (history_next + history_size - n) % history_size;
        for (size_t i = 0; i < n; i++) {
            out[i] = history[(start + i) % history_size];
        }
        return n;
    }
}

uint64_t now_ns()
{
    return osv::clock::uptime::now().time_since_epoch().count();
}

}
