/*
 * miniext block I/O.
 *
 * This is the layer that would otherwise be a separate block-device
 * abstraction; folding it into the filesystem is deliberate. There is no
 * buffer cache: reads go to the device and writes land on it immediately, so
 * there is no dirty-writeback ordering to get wrong.
 *
 * One NVMe I/O queue per vCPU, so concurrent readers and writers do not
 * serialise behind each other. The per-queue lock covers only the submission
 * itself; the wait for completion happens with no lock held, which is what lets
 * requests overlap.
 */

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>

#include <osv/kernel_config.h>
#include <osv/sched.hh>

#include "drivers/nvme.hh"
#include "drivers/nvme-queue.hh"
#include "internal.hh"
#include <osv/mem/frames.hh>
#include <osv/mem/mapping.hh>

namespace miniext {

// Largest bounce transfer.
static const uint32_t BOUNCE_MAX = 128 * 1024;

// One outstanding request; the completion callback runs in the MSI-X handler,
// so it does the minimum: flag and wake.
namespace {
struct io_request {
    sched::thread_handle waiter;
    volatile bool done = false;
};

void io_complete(void *ctx, const nvme_sq_entry_t *)
{
    auto *req = static_cast<io_request *>(ctx);
    req->done = true;
    req->waiter.wake_from_kernel_or_with_irq_disabled();
}

void group_complete(void *ctx, const nvme_sq_entry_t *)
{
    static_cast<io_group *>(ctx)->drop();
}
} // namespace

void io_group::drop()
{
    if (outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (auto f = on_settled.exchange(nullptr, std::memory_order_acq_rel)) {
            f(settled_arg);
        }
        finished.store(true, std::memory_order_release);
        waiter.wake_from_kernel_or_with_irq_disabled();
    }
}

int device::open(int nvme_id)
{
    auto *drv = nvme::nvme_driver::get_nvme_device(nvme_id);
    if (!drv) {
        printf("miniext: no NVMe controller with id %d\n", nvme_id);
        return -ENODEV;
    }

    auto ns = drv->_ns_data.find(1);
    if (ns == drv->_ns_data.end()) {
        printf("miniext: NVMe controller %d has no namespace 1\n", nvme_id);
        return -ENODEV;
    }

    _lba_size = ns->second->blocksize;
    _lba_count = ns->second->blockcount;
    if (_lba_size == 0) {
        printf("miniext: NVMe namespace reports a zero LBA size\n");
        return -EINVAL;
    }
    // Address raw LBAs until the superblock tells us the real block size.
    _block_size = _lba_size;
    _lbas_per_block = 1;

    // What one command may carry: the controller's MDTS, and never more than a
    // single PRP list can address (prp1 plus 511 entries). A transfer past
    // either is rejected with "invalid field in command".
    constexpr size_t prp_limit = 511 * NVME_PAGESIZE;
    const size_t mdts = drv->max_transfer_bytes();
    _max_bytes = (mdts && mdts < prp_limit) ? mdts : prp_limit;

    _queues = queues_for(nvme_id, drv);
    if (!_queues || _queues->empty()) {
        printf("miniext: could not create any NVMe I/O queue\n");
        return -EIO;
    }
    return 0;
}

uint32_t device::max_blocks() const
{
    if (!_block_size) {
        return 1;
    }
    size_t n = _max_bytes / _block_size;
    return n ? static_cast<uint32_t>(n) : 1;
}

// The queue set for a controller, created on first use and kept for the life of
// the boot. See device::queue_set in internal.hh for why it is shared rather
// than per-open.
std::shared_ptr<device::queue_set> device::queues_for(int nvme_id,
                                                      nvme::nvme_driver *drv)
{
    static mutex sets_lock;
    static std::map<int, std::shared_ptr<queue_set>> sets;

    WITH_LOCK(sets_lock) {
        auto it = sets.find(nvme_id);
        if (it != sets.end()) {
            return it->second;
        }

        auto set = std::make_shared<queue_set>();
        unsigned ceiling = drv->max_queue_depth();
        ceiling = ceiling > 2 ? ceiling - 1 : 2;
        int depth = std::min<unsigned>(CONF_nvme_max_queue_depth, ceiling);

        // One queue per vCPU, each with its completion interrupt pinned to that
        // CPU. create_io_queue asserts on a null cpu despite its doc comment
        // saying nullptr means "current", so the CPU is always passed
        // explicitly.
        //
        // The controller may hand out fewer queues than we ask for (the driver
        // caps MSI-X vectors at min(msix_entries, ncpus + 1), one of which is
        // the admin queue). Whatever we get, pick() maps CPUs onto it, so fewer
        // queues costs throughput but never correctness.
        for (size_t i = 0; i < sched::cpus.size(); i++) {
            auto *qp = static_cast<nvme::io_queue_pair *>(
                drv->create_io_queue(depth, sched::cpus[i]));
            if (!qp) {
                break;
            }
            auto slot = std::unique_ptr<queue>(new queue());
            slot->q = qp;
            set->push_back(std::move(slot));
        }

        if (set->empty()) {
            return nullptr;
        }
        if (set->size() < sched::cpus.size()) {
            printf("miniext: %zu I/O queues for %zu vCPUs; some will share\n",
                   set->size(), sched::cpus.size());
        }
        sets.emplace(nvme_id, set);
        return set;
    }
}

// The queue for the CPU we are running on. A thread can migrate between
// picking a queue and submitting on it -- harmless, since the per-queue lock
// makes any queue safe for any submitter; the pinning is a locality hint.
device::queue &device::pick()
{
    unsigned id = sched::cpu::current()->id;
    return *(*_queues)[id % _queues->size()];
}

int device::set_block_size(uint32_t block_size)
{
    if (_lba_size == 0 || block_size % _lba_size != 0) {
        printf("miniext: fs block size %u is not a multiple of the %u-byte LBA\n",
               block_size, _lba_size);
        return -EINVAL;
    }
    _block_size = block_size;
    _lbas_per_block = block_size / _lba_size;
    return 0;
}

void device::close()
{
    // The driver owns the queues and the set outlives every device that used
    // it; dropping the reference is all there is to do.
    _queues.reset();
}

bool device::in_range(uint64_t block, uint32_t count) const
{
    if ((block + count) * static_cast<uint64_t>(_lbas_per_block) <= _lba_count) {
        return true;
    }
    printf("miniext: I/O past end of namespace (block %lu count %u)\n",
           (unsigned long)block, count);
    return false;
}

// Both submit paths cut the request into commands the controller accepts. A
// caller reads as much as one extent run holds, which is far past that.
int device::submit(void *buf, uint64_t block, uint32_t count, bool write)
{
    const uint32_t most = max_blocks();
    auto *p = static_cast<uint8_t *>(buf);
    while (count) {
        const uint32_t here = count < most ? count : most;
        int rc = submit_one(p, block, here, write);
        if (rc < 0) {
            return rc;
        }
        p += static_cast<size_t>(here) * _block_size;
        block += here;
        count -= here;
    }
    return 0;
}

int device::submit_async(void *buf, uint64_t block, uint32_t count, bool write,
                         io_group &g)
{
    const uint32_t most = max_blocks();
    auto *p = static_cast<uint8_t *>(buf);
    while (count) {
        const uint32_t here = count < most ? count : most;
        int rc = submit_one_async(p, block, here, write, g);
        if (rc < 0) {
            return rc;
        }
        p += static_cast<size_t>(here) * _block_size;
        block += here;
        count -= here;
    }
    return 0;
}

int device::submit_one(void *buf, uint64_t block, uint32_t count, bool write)
{
    if (!_queues || _queues->empty()) {
        return -ENODEV;
    }

    const uint64_t byte_off = block * static_cast<uint64_t>(_block_size);
    const uint32_t byte_len = count * _block_size;

    if (!in_range(block, count)) {
        return -EIO;
    }

    queue &qu = pick();

    io_request req;
    req.waiter.reset(*sched::thread::current());

    // submit_request returns 1 when queued and 0 when the submission queue was
    // full -- 0 means nothing was submitted. Retry, but drop the lock first so
    // the in-flight requests ahead of us can complete and free a slot.
    for (;;) {
        int rc;
        {
            SCOPE_LOCK(qu.lock);
            rc = qu.q->submit_request(1, buf, byte_off, byte_len, io_complete,
                                      &req, 0, write ? nvme::WRITE : nvme::READ);
        }
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            req.waiter.clear();
            printf("miniext: submit_request failed (%d)\n", rc);
            return -EIO;
        }
        sched::thread::yield();
    }

    // Waiting with no lock held is the point: other threads keep submitting on
    // this queue while we are parked here. The MSI-X handler drains completions
    // and wakes us. A completion error is fatal inside the driver (it asserts),
    // so reaching here means success.
    sched::thread::wait_until([&req] { return req.done; });
    req.waiter.clear();
    return 0;
}

// Place the command and leave. The group is what the completion finds its way
// back to, and it is held from here until the interrupt drops it.
int device::submit_one_async(void *buf, uint64_t block, uint32_t count, bool write,
                             io_group &g)
{
    if (!_queues || _queues->empty()) {
        return -ENODEV;
    }
    if (!in_range(block, count)) {
        return -EIO;
    }

    const uint64_t byte_off = block * static_cast<uint64_t>(_block_size);
    const uint32_t byte_len = count * _block_size;
    queue &qu = pick();

    g.hold();
    for (;;) {
        int rc;
        {
            SCOPE_LOCK(qu.lock);
            rc = qu.q->submit_request(1, buf, byte_off, byte_len, group_complete,
                                      &g, 0, write ? nvme::WRITE : nvme::READ);
        }
        if (rc == 1) {
            return 0;
        }
        if (rc != 0) {
            g.drop();
            printf("miniext: submit_request failed (%d)\n", rc);
            return -EIO;
        }
        // The queue is full and nothing was placed. The requests ahead of us
        // are what free a slot, so let them.
        sched::thread::yield();
    }
}

int device::read_async(void *buf, uint64_t block, uint32_t count, io_group &g)
{
    if (!mem::mapping::is_contiguous(buf, static_cast<size_t>(count) * _block_size)) {
        return bounce(buf, block, count, false);
    }
    return submit_async(buf, block, count, false, g);
}

int device::write_async(const void *buf, uint64_t block, uint32_t count, io_group &g)
{
    if (!mem::mapping::is_contiguous(buf, static_cast<size_t>(count) * _block_size)) {
        return bounce(const_cast<void *>(buf), block, count, true);
    }
    return submit_async(const_cast<void *>(buf), block, count, true, g);
}

// Use a bounce buffer when the caller hands us a buffer outside the linear map
int device::bounce(void *buf, uint64_t block, uint32_t count, bool write)
{
    const uint32_t per_pass = BOUNCE_MAX / _block_size;
    scratch tmp(per_pass * _block_size);
    if (!tmp) {
        return -ENOMEM;
    }

    auto *p = static_cast<uint8_t *>(buf);
    while (count) {
        const uint32_t here = count < per_pass ? count : per_pass;
        const size_t bytes = static_cast<size_t>(here) * _block_size;

        if (write) {
            memcpy(tmp.data(), p, bytes);
        }
        int rc = submit(tmp.data(), block, here, write);
        if (rc < 0) {
            return rc;
        }
        if (!write) {
            memcpy(p, tmp.data(), bytes);
        }

        p += bytes;
        block += here;
        count -= here;
    }
    return 0;
}

int device::read(void *buf, uint64_t block, uint32_t count)
{
    if (!mem::mapping::is_contiguous(buf, static_cast<size_t>(count) * _block_size)) {
        return bounce(buf, block, count, false);
    }
    return submit(buf, block, count, false);
}

int device::write(const void *buf, uint64_t block, uint32_t count)
{
    if (!mem::mapping::is_contiguous(buf, static_cast<size_t>(count) * _block_size)) {
        return bounce(const_cast<void *>(buf), block, count, true);
    }
    return submit(const_cast<void *>(buf), block, count, true);
}

int device::flush()
{
    if (!_queues || _queues->empty()) {
        return -ENODEV;
    }

    // A flush commits the controller's volatile write cache for the whole
    // namespace, so issuing it on one queue covers writes submitted on all of
    // them -- but only those that have already completed, which is why callers
    // must have collected their writes before calling this.
    queue &qu = pick();

    io_request req;
    req.waiter.reset(*sched::thread::current());

    // submit_request asserts nlb >= 1 before it looks at the opcode, so a FLUSH
    // still has to present a non-zero length even though submit_flush_cmd()
    // ignores the address and length entirely. Hand it one block.
    for (;;) {
        int rc;
        {
            SCOPE_LOCK(qu.lock);
            rc = qu.q->submit_request(1, nullptr, 0, _block_size, io_complete,
                                      &req, 0, nvme::FLUSH);
        }
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            req.waiter.clear();
            return -EIO;
        }
        sched::thread::yield();
    }

    sched::thread::wait_until([&req] { return req.done; });
    req.waiter.clear();
    return 0;
}

} // namespace miniext
