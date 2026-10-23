/*
 * Invalidation, and the epoch counter that lets a client detach without one.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <atomic>

#include <osv/align.hh>
#include <osv/sched.hh>
#include <osv/mem/mapping.hh>

namespace mem {
namespace mapping {

// Global flushes started, and global flushes finished. Two counters, because
// what a client needs to know is that one began after its entries were cleared,
// which a count of completions alone cannot say.
static std::atomic<uint64_t> begun, done;

uint64_t flush_epoch()
{
    // The caller has just cleared entries. They must be visible to every
    // page-table walker before a flush another cpu begins from here on is
    // counted as having covered them.
    pte_barrier();
    return begun.load(std::memory_order_seq_cst);
}

void flush_local(range r)
{
    if (r.size() > flush_batch * page_size) {
        tlb_flush_local();
        return;
    }
    for (uintptr_t va = r.start; va < r.end; va += page_size) {
        tlb_flush_page(va);
    }
}

// Naming each address in turn, on every cpu. Past flush_batch pages it is a
// full flush instead, and only then does the epoch move.
void flush_range(range r)
{
    size_t pages = align_up(r.size(), page_size) / page_size;
    if (pages > flush_batch) {
        flush_all();
        return;
    }
    uintptr_t va[flush_batch];
    uintptr_t a = align_down(r.start, page_size);
    for (size_t i = 0; i < pages; i++, a += page_size) {
        va[i] = a;
    }
    tlb_flush_pages_all(va, pages);
}

void flush_all()
{
    begun.fetch_add(1, std::memory_order_seq_cst);
    tlb_flush_all();
    done.fetch_add(1, std::memory_order_seq_cst);
}

bool flushed_since(uint64_t epoch)
{
    // More flushes have finished than had started when the epoch was taken,
    // so one of them started after. Flushes overlap, which is why finishing
    // later is not enough on its own.
    return done.load(std::memory_order_seq_cst) > epoch;
}

void pending_invalidation::add(uintptr_t)
{
    if (count == flush_batch) {
        all = true;
        return;
    }
    count++;
}

// One global flush covers every clear that came before it began, so callers
// that pile up behind a flush in progress need at most one more between them.
// Serialized here rather than in the shootdown itself so the check happens
// after the wait: a caller that finds a newer flush finished by the time it
// holds the lock has nothing left to do.
static mutex coalesce_mutex;

// Read by the tpch driver: flushes asked for, and how many a newer one covered.
std::atomic<uint64_t> flushes_asked, flushes_coalesced;

static void flush_all_since(uint64_t epoch)
{
    flushes_asked.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<mutex> guard(coalesce_mutex);
    if (flushed_since(epoch)) {
        flushes_coalesced.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    flush_all();
}

void pending_invalidation::invalidate()
{
    // Always a full flush, never the address list: the cost is the round trip
    // to every cpu, not the entries, and only a full flush can stand in for
    // the ones queued behind it.
    if ((count || all) && !flushed_since(epoch)) {
        flush_all_since(epoch);
    }
    count = 0;
    all = false;
    epoch = never_flushed;
}

}
}
