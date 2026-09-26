/*
 * The inference task abstraction.
 *
 * A request arrives on some core, that core wraps it in a task, queues it and
 * calls the scheduler. The scheduler runs only on events (arrival, completion,
 * phase change, a core freed) and hands each task an assignment: the cores it
 * may use from its next iteration on. A task runs iteration after iteration
 * on its assignment until it finishes or the assignment changes, so a change
 * takes effect at an iteration boundary, unless set_preempt_in_iteration()
 * lets a better priority take cores inside one.
 *
 * The engine supplies the policies (how to batch, how many cores a phase
 * wants) and the mechanism of one iteration; the OS supplies the queue, the
 * decision, and the workers: threads that exist only to run one task's
 * iteration on one core, created and destroyed as assignments change.
 *
 * Built into the image with conf_lros=1. Uses sched::thread as it is.
 */

#ifndef LROS_HH
#define LROS_HH

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lros {

typedef uint64_t cpu_mask;     // bit i = sched::cpus[i]
typedef uint32_t accel_mask;   // bit i = accelerator unit i

// A resource domain is a set of interchangeable execution units with a
// capacity. The scheduler hands out integers per domain; what an integer buys
// is the engine's policy, from its calibration, because the devices disagree:
//
//   cpu    A76x4: k cores multiply throughput up to a knee that moves with
//          batch (B=1 saturates at 2 workers, B=4 scales to 4).
//   accel  RK3588 NPU: 3 independently addressable cores, k of them really is
//          ~k times the work (2.81x on 3), and RKNN_NPU_CORE_0_1_2 does not
//          exist for matmul, so partitioning is the only way to use them all.
//          Orin GPU: no addressable partition at all -- one prefill saturates
//          the device (1309/1703/1584/1350 t/s across batch), and the core
//          mask is a no-op in the CUDA plugin.
//
// So the GPU is not a special case here, it is a domain whose speedup curve is
// flat: its policy asks for one unit and never more. Capacity comes from the
// plugin, not from a constant.
enum class domain : uint8_t { cpu = 0, accel = 1 };
static const int32_t n_domains = 2;

enum class phase : uint8_t { prefill, decode };

// What the application asked for, as far as the OS needs it. The engine owns
// the prompt, the sampler and the KV cache.
struct request {
    int32_t id;
    int32_t prio;        // lower runs first
    int32_t model;       // registry id of the base model
    int32_t n_prompt;    // tokens, known at arrival
    int32_t max_tokens;  // generation limit, 0 for none
    void   *engine;      // the engine's per-request state
};

// The units, per domain, a task may use from its next iteration on.
struct assignment {
    cpu_mask   cpus   = 0;
    accel_mask accels = 0;
    int32_t    n_cpus()   const { return __builtin_popcountll(cpus); }
    int32_t    n_accels() const { return __builtin_popcount(accels); }
    int32_t    n(domain d) const {
        return d == domain::cpu ? n_cpus() : n_accels();
    }
};

enum class task_state : uint8_t {
    queued,     // waiting for cores
    running,    // has a main worker
    paused,     // assignment shrank to nothing; KV in place, resumes on cores
    done,
};

// One or more requests over one model, executed as one computation. The
// scheduler creates a task per arriving request and may compose several into
// one through the engine's policy.
struct task {
    int32_t              id;
    int32_t              model;
    std::vector<request *> members;          // in the current batch
    std::vector<request *> pending_members;  // joined; in the batch from the next boundary
    phase                ph;
    int32_t              prio;         // best of the members
    task_state           state;
    assignment           current;      // what the task runs on now
    assignment           next;         // what the scheduler last decided
    int32_t              wanted;       // cores the engine asked for in this phase
    void                *engine;       // the engine's per-task state (context, batch)

    // Accounting for the evaluation, ns of the OS clock.
    uint64_t t_submit, t_first_run, t_done;
    uint32_t n_iterations, n_reassignments;
    int32_t  max_members;   // largest batch this task ever ran

    // Preemption accounting. A decision changes `next`; the task applies it
    // at its next iteration boundary, so the delay between the two is what
    // preemption actually costs, and it is bounded by one iteration.
    bool     on_device;          // blocked in an accelerator submission
    uint32_t rt_prio;            // real-time priority of its workers, 0 for none
    uint64_t t_decided;          // when `next` last changed, 0 if applied
    uint64_t decision_lag_ns;    // summed decision -> boundary
    uint64_t paused_ns;          // summed time held at zero cores
    uint64_t t_paused_since;     // 0 when running
    uint32_t n_preemptions;      // assignments that took every core away

    // Memory. `kv_charged` is what this task's context costs while it exists;
    // the scheduler sets `kv_evict_asked` when it wants that back, and the
    // task's own thread honours it at an iteration boundary, so a context is
    // never taken from under a running computation.
    size_t   kv_charged;
    bool     kv_evict_asked;
    uint32_t n_kv_evictions;
};

// One figure for the machine's memory, and what is charged against it.
//
// Two things are: the page cache that serves the model weights, and the
// inference contexts, whose KV caches and compute buffers come and go with the
// tasks. The weights are charged their *limit* rather than their residency,
// because the cache is entitled to grow to it at any fault. `pinned` is the
// part of that allowance no pressure can recover -- the prefix the weight
// policy holds -- and is recorded so that a budget can be read as "what is
// left to move" rather than "what is in use".
struct mem_state {
    size_t budget = 0;          // 0: unbounded, and nothing below is enforced
    size_t weights_limit = 0;   // what the weight cache may hold
    size_t weights_pinned = 0;  // of which this much is never reclaimable
    size_t kv_used = 0;         // contexts that exist now
    size_t kv_saved = 0;        // serialised KV of contexts that were given back
    size_t kv_paged = 0;        // the KV page cache's allowance, when KV is paged

    // What the contexts may hold: the budget less the weights' allowance,
    // less the KV cache's own allowance when the KV is paged, and less what
    // evicted contexts are still keeping.
    size_t kv_budget() const
    {
        if (budget == 0) { return SIZE_MAX; }
        const size_t taken = weights_limit + kv_saved + kv_paged;
        return budget > taken ? budget - taken : 0;
    }
    bool over() const { return budget != 0 && kv_used > kv_budget(); }
};

// The total. 0 leaves memory unmanaged, which is what a run with no --mem
// budget does.
void set_mem_budget(size_t bytes);

// What the weight cache was allowed, and how much of it its policy pins.
// Told to the OS by whoever made the mapping, since the page cache's limit is
// settled before any task exists.
void set_weights_charge(size_t limit, size_t pinned);

// What the KV page cache was allowed, when the KV caches are paged rather
// than held whole (app/llama.cpp/miniosv/kvpage.hpp). Charged like the
// weights' allowance and for the same reason: the cache is entitled to grow
// to it, and a context's KV then costs the machine nothing beyond it, because
// its pages come and go against that one figure. A task's charge is its
// compute buffers alone, and a context need never be evicted to free KV.
void set_kv_paged_charge(size_t limit);

mem_state mem_status();

// What the scheduler knows when it asks a task how wide it should be. Tasks
// are asked in priority order, so `n_pending` is what is still behind this
// one and would get whatever it leaves.
struct decision {
    int32_t  total[n_domains];     // usable units in each domain
    int32_t  avail[n_domains];     // not yet handed out when this task is asked
    int32_t  n_pending;            // runnable tasks after this one, in priority order
    int32_t  worst_pending_prio;   // their worst priority, INT32_MAX if none
    uint64_t max_pending_pause_ns; // longest any of them has been held at zero units
    uint64_t this_pause_ns;        // how long *this* task has had nothing

    // Memory as it stands when the decision is taken, so that a policy can
    // decline to start work it has nowhere to put.
    mem_state mem;

    // The CPU domain reads often enough to be worth naming.
    int32_t n_cores() const { return total[(int) domain::cpu]; }
    int32_t n_free()  const { return avail[(int) domain::cpu]; }
};

// What the engine asks for, per domain. Zero in a domain means "none of it":
// a CPU-only task leaves accel at 0, an accelerator-resident phase may leave
// cpu at 1 for the thread that drives submission.
struct width {
    int32_t n[n_domains];
    width() { for (int32_t i = 0; i < n_domains; i++) { n[i] = 0; } }
    int32_t & operator[](domain d)       { return n[(int) d]; }
    int32_t   operator[](domain d) const { return n[(int) d]; }
};

// The result of one iteration, as the engine reports it.
enum class iter_result : uint8_t {
    running,        // more iterations in the same phase
    phase_changed,  // task.ph was updated by the engine; re-decide
    done,           // every member finished
    failed,
};

// What the engine registers. compose and width are called from inside the
// scheduling decision, with its lock held: they must be short and must not
// call back into lros. iterate and finished run on the task's main worker.
struct engine_ops {
    // Batching: which requests compute together. Called per model with its
    // queued and running tasks. The engine moves members of queued tasks
    // into another queued task's members, or into a running task's
    // pending_members (they enter its batch at its next boundary), under
    // llama.cpp's rules: a free sequence, KV room, the pass within n_batch.
    // A queued task left without members is dropped. Absent: no batching.
    void (*compose)(std::vector<task *> &queued, std::vector<task *> &running);

    // Units, per domain, this task should take in its current phase, given
    // what else is waiting. Absent: every CPU, no accelerator.
    //
    // This is where the floor left for lower-priority work lives, and it is
    // the engine's to decide because it depends on the requests: a task with
    // a short prompt can reasonably take every core and be gone, while one
    // with a long prompt would hold them long enough to starve everything
    // behind it. The OS supplies the context and enforces the answer at the
    // next boundary; it does not second-guess the number.
    //
    // It answers per domain for the same reason: only the engine's
    // calibration knows that a second NPU core nearly doubles the work while
    // a second GPU "unit" buys nothing.
    width (*width_of)(const task &t, const decision &d);

    // Run one iteration on the calling worker (worker 0) and the n - 1
    // helper workers the OS created for it; the engine sees them as workers
    // 0..n-1 through run_on_workers(). Must update t.ph on a phase change.
    iter_result (*iterate)(task &t, const assignment &a);

    // A member finished; the engine releases its state.
    void (*finished)(task &t, request *r);

    // Give back the memory of this task's inference context.
    //
    // The engine keeps what it must -- the live sequences, serialised -- frees
    // the context and returns the bytes released, along with the bytes it is
    // still holding for the sequences it saved. The task keeps its members and
    // its place in the queue: its next iterate() builds a context again and
    // restores them, which is the path iterate() already takes when it has no
    // context yet.
    //
    // Runs on the task's own thread, between iterations. Absent: contexts are
    // never reclaimed and the budget only reports.
    size_t (*evict_kv)(task &t, size_t *saved_bytes);
};

void set_engine(const engine_ops &ops);

// Which cores inference may use. Default: all of them.
void set_cores(cpu_mask usable);

// How many accelerator units the scheduler may hand out. Default 0: no
// accelerator, so width_of's accel answer is always clamped away and nothing
// changes for a CPU-only image. This is the plugin's answer, not a constant:
// 3 for the RK3588 NPU, 1 for CUDA, which has no addressable partition.
void set_accel_capacity(int32_t n_units);
int32_t accel_capacity();

// Off (the default): a core another task runs on is handed over at that
// task's next iteration boundary. On: a task takes cores from a worse-priority
// one at once. Its workers run there at a real-time priority, so the kernel
// stops the other task's threads where they are, mid-iteration, and they carry
// on when the cores are free again.
void set_preempt_in_iteration(bool on);

// Arrival: called on whatever core the caller runs on. Queues the request as
// a task and takes a scheduling decision. Returns the task id.
int32_t submit(request *r);

// Arms a timer so that submit(r) runs delay_us from now, on whichever core
// the timer fires on. The stand-in for a network interrupt. A delay rather
// than an absolute time, so that the caller need not share a clock with us.
void submit_after(uint64_t delay_us, request *r);

// Blocks the caller until every submitted task is done.
void wait_all();

// Inside iterate(): run fn(worker, n_workers) on every worker of the current
// assignment, the caller being worker 0, and return when all have finished.
// This is what a ported ggml graph compute is built on.
void run_on_workers(task &t, void (*fn)(void *arg, int32_t worker, int32_t n_workers), void *arg);

// The same over an explicit set of cores, for a caller with no task yet: n
// workers placed round-robin over cpus, the caller being worker 0. n larger
// than the number of cores puts several workers on a core rather than
// leaving one out, which would hang a barrier.
void run_parallel(cpu_mask cpus, int32_t n, void (*fn)(void *arg, int32_t worker, int32_t n_workers), void *arg);

// What a compute library called from inside iterate() should use: n workers
// over the assignment of the task this thread is running, falling back to
// `fallback` when the caller is not a task's worker (model loading, warmup).
// This is the entry a ggml parallel runner is wired to.
void run_workers_here(int32_t n, void (*fn)(void *arg, int32_t worker, int32_t n_workers), void *arg,
                      cpu_mask fallback);

// The task this thread is running an iteration of, or nullptr.
task *current_task();

// Around a blocking accelerator submission from inside iterate(). While the
// calling task waits on the device its cores are not counted as busy, so
// another task may be started on them; no-ops outside a task.
void device_enter();
void device_leave();

// What a task's inference context costs, told by the engine when it makes one
// and when it gives it back. Charged against the KV share of the budget.
void kv_charge(task &t, size_t bytes);
void kv_uncharge(task &t);

// What evicted contexts are still holding, in serialised form. The engine
// reports the difference as sequences are saved and restored.
void kv_saved_add(size_t bytes);
void kv_saved_sub(size_t bytes);

// What is in use right now.
struct occupancy {
    int32_t cores_total, cores_busy;
    int32_t tasks_queued, tasks_running, tasks_paused;
};
occupancy get_occupancy();

// Per-task record kept after completion, for the report.
struct task_stats {
    int32_t  id;
    int32_t  n_members;      // largest batch it ran
    uint64_t submit_to_first_run_ns;
    uint64_t total_ns;
    uint32_t n_iterations;
    uint32_t n_reassignments;
    uint32_t n_preemptions;
    uint64_t paused_ns;
    uint64_t decision_lag_ns;   // total; divide by n_reassignments for the mean
};
size_t finished_tasks(task_stats *out, size_t max);

// Worker lifecycle cost, accumulated: create+pin+start, and join.
struct worker_stats {
    uint64_t n_created;
    uint64_t create_ns_total;
    uint64_t join_ns_total;
};
worker_stats get_worker_stats();

}

#endif /* LROS_HH */
