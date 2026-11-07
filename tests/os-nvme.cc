/*
 * The NVMe data path: enumeration, geometry, queue creation and I/O.
 *
 * Nothing else in the tree creates an I/O queue, so without this suite the
 * driver's data path is compiled and never run. One header per function of the
 * public interface, as in os-memory-primitives.cc.
 *
 * Writes go only to a device that carries no partition table. The boot image is
 * GPT, so a protective MBR or a GPT header means this is the disk we booted
 * from and the suite reads it and nothing more. Give the guest a second,
 * zeroed device (run.py --emulated-nvme) to exercise the write path.
 */

#include <osv/drivers_config.h>

#if CONF_drivers_nvme

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <stdlib.h>
#include <vector>

#include <osv/align.hh>
#include <osv/mem/mapping.hh>

#include "drivers/nvme.hh"
#include "mem-test.hh"

using namespace memtest;

namespace {

namespace map = mem::mapping;

uint32_t lcg_next(uint32_t &seed)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

// What a completion told us. The driver used to hand the callback a null
// completion entry, so a caller could not tell success from failure; no_entry
// counts that happening.
struct io_batch {
    std::atomic<unsigned> done{0};
    std::atomic<unsigned> bad_status{0};
    std::atomic<unsigned> no_entry{0};
};

void on_done(void *ctx, const nvme_cq_entry_t *cpl)
{
    auto *b = static_cast<io_batch *>(ctx);
    if (!cpl) {
        b->no_entry.fetch_add(1, std::memory_order_relaxed);
    } else if (cpl->sc != 0 || cpl->sct != 0) {
        b->bad_status.fetch_add(1, std::memory_order_relaxed);
    }
    b->done.fetch_add(1, std::memory_order_release);
}

// Drain until n completions have arrived. An interrupt-driven queue is drained
// by its ISR too; process_completions() is safe either way and is the only
// thing that makes progress on a polled queue.
bool await(nvme::io_queue_pair *q, io_batch &b, unsigned n, double secs = 10.0)
{
    auto t0 = clk::now();
    while (b.done.load(std::memory_order_acquire) < n) {
        q->process_completions(0);
        if (since(t0) > secs) {
            printf("\t  TIMEOUT: %u of %u completions after %.1fs\n",
                   b.done.load(), n, secs);
            return false;
        }
    }
    return true;
}

// A PRP entry covers a page from its own offset, so a transfer that starts
// mid-page is cut short at the page boundary. Every buffer here is page
// aligned, and page-sized multiples besides.
void *dma_buf(size_t bytes)
{
    void *p = aligned_alloc(map::page_size, align_up(bytes, map::page_size));
    assert(p);
    return p;
}

// Submit one command, draining and retrying while the queue is full.
bool submit(nvme::io_queue_pair *q, u32 nsid, void *buf, u64 off, u32 len,
            io_batch &b, nvme::NVME_COMMAND op)
{
    auto t0 = clk::now();
    for (;;) {
        int rc = q->submit_request(nsid, buf, off, len, on_done, &b, 0, op);
        if (rc == 0) {
            return true;
        }
        if (rc != -EBUSY) {
            printf("\t  submit_request failed: %d\n", rc);
            return false;
        }
        q->process_completions(0);
        if (since(t0) > 10.0) {
            printf("\t  submit_request stayed busy for 10s\n");
            return false;
        }
    }
}

// One read/write of len bytes at off, submitted, rung and awaited.
bool io_once(nvme::io_queue_pair *q, u32 nsid, void *buf, u64 off, u32 len,
             nvme::NVME_COMMAND op)
{
    io_batch b;
    if (!submit(q, nsid, buf, off, len, b, op)) {
        return false;
    }
    q->ring_sq_doorbell();
    if (!await(q, b, 1)) {
        return false;
    }
    return b.bad_status.load() == 0 && b.no_entry.load() == 0;
}

bool looks_bootable(nvme::io_queue_pair *q, u32 nsid, u32 bs)
{
    auto *buf = static_cast<unsigned char *>(dma_buf(2 * bs));
    memset(buf, 0, 2 * bs);
    bool read_ok = io_once(q, nsid, buf, 0, 2 * bs, nvme::READ);
    bool mbr = buf[510] == 0x55 && buf[511] == 0xaa;
    bool gpt = memcmp(buf + bs, "EFI PART", 8) == 0;
    free(buf);
    // Unreadable counts as bootable: refuse to write what we cannot inspect.
    return !read_ok || mbr || gpt;
}

struct device {
    nvme::nvme_driver *drv = nullptr;
    u32 nsid = 1;
    u32 block_size = 0;
    u64 block_count = 0;
    bool writable = false;
    u64 bytes() const { return block_count * block_size; }
};

std::vector<device> devices;

/* enumeration and geometry ------------------------------------------------- */

void driver_enumeration()
{
    function("nvme_driver::nvme_drives, ns_info, max_transfer_bytes, max_queue_depth");

    test("at least one controller came up, and each reports a usable namespace");
    {
        CHECK(!nvme::nvme_driver::nvme_drives.empty());
        for (auto *d : nvme::nvme_driver::nvme_drives) {
            const nvme_ns_t *ns = d->ns_info(1);
            CHECK(ns != nullptr);
            if (!ns) {
                continue;
            }
            CHECK(ns->blocksize >= 512);
            CHECK(ns->blockcount > 0);
            CHECK(d->max_transfer_bytes() >= ns->blocksize);
            CHECK(d->max_queue_depth() > 1);
            printf("    nvme%d: %lu blocks of %u B (%lu MiB), "
                   "mdts %zu KiB, qdepth %u\n",
                   d->get_id(), (unsigned long)ns->blockcount,
                   (unsigned)ns->blocksize,
                   (unsigned long)(ns->blockcount * ns->blocksize) >> 20,
                   d->max_transfer_bytes() >> 10, d->max_queue_depth());
            devices.push_back({d, 1, ns->blocksize, ns->blockcount});
        }
    }

    test("ns_info reports nothing for a namespace the controller does not have");
    {
        for (auto *d : nvme::nvme_driver::nvme_drives) {
            CHECK(d->ns_info(0xdead) == nullptr);
        }
    }
}

/* queue creation ----------------------------------------------------------- */

int queue_depth(const device &dev)
{
    u32 cap = dev.drv->max_queue_depth();
    return (int)std::min<u32>(64, cap > 2 ? cap - 1 : 2);
}

void queue_creation()
{
    function("nvme_driver::create_io_queue");

    test("an interrupt-driven queue is created and reports itself not polled");
    {
        for (auto &dev : devices) {
            auto *q = dev.drv->create_io_queue(queue_depth(dev));
            CHECK(q != nullptr);
            if (q) {
                CHECK(!q->polled());
                CHECK(!q->is_full());
                dev.drv->remove_io_user_queue(q);
            }
        }
    }

    // The default is documented as "the calling cpu", and used to trip an
    // assertion instead.
    test("a null completion cpu means the calling cpu, not an abort");
    {
        for (auto &dev : devices) {
            auto *q = dev.drv->create_io_queue(queue_depth(dev), nullptr, true);
            CHECK(q != nullptr);
            if (q) {
                dev.drv->remove_io_user_queue(q);
            }
        }
    }

    test("a polled queue is created and says so");
    {
        for (auto &dev : devices) {
            auto *q = dev.drv->create_io_queue(queue_depth(dev), nullptr, false);
            CHECK(q != nullptr);
            if (q) {
                CHECK(q->polled());
                dev.drv->remove_io_user_queue(q);
            }
        }
    }
}

/* the data path ------------------------------------------------------------ */

// Well clear of anything a boot image would use, for the devices that have one.
const u64 scratch_off = 1024 * 512;

void classify_devices()
{
    function("which devices may be written");

    test("a device carrying a partition table is read-only to this suite");
    {
        for (auto &dev : devices) {
            auto *q = dev.drv->create_io_queue(queue_depth(dev));
            CHECK(q != nullptr);
            if (!q) {
                continue;
            }
            dev.writable = !looks_bootable(q, dev.nsid, dev.block_size);
            printf("    nvme%d: %s\n", dev.drv->get_id(),
                   dev.writable ? "no partition table, writable" :
                                  "partition table or unreadable, read only");
            dev.drv->remove_io_user_queue(q);
        }
        bool any = false;
        for (auto &dev : devices) {
            any |= dev.writable;
        }
        if (!any) {
            printf("    no writable device; boot with --emulated-nvme to cover "
                   "the write path\n");
        }
    }
}

device *writable_device()
{
    for (auto &dev : devices) {
        if (dev.writable) {
            return &dev;
        }
    }
    return nullptr;
}

void data_path(bool interrupts)
{
    const char *mode = interrupts ? "interrupts" : "polling";
    device *dev = writable_device();
    if (!dev) {
        return;
    }
    auto *q = dev->drv->create_io_queue(queue_depth(*dev), nullptr, interrupts);
    CHECK(q != nullptr);
    if (!q) {
        return;
    }

    test("one block out and back, with the completion entry delivered");
    {
        const u32 len = dev->block_size;
        auto *out = static_cast<unsigned char *>(dma_buf(len));
        auto *in = static_cast<unsigned char *>(dma_buf(len));
        for (u32 i = 0; i < len; i++) {
            out[i] = (unsigned char)(i * 7 + 1);
        }
        memset(in, 0, len);

        io_batch wb;
        CHECK(submit(q, dev->nsid, out, scratch_off, len, wb, nvme::WRITE));
        q->ring_sq_doorbell();
        CHECK(await(q, wb, 1));
        CHECK(wb.no_entry.load() == 0);   // the fix: a real completion entry
        CHECK(wb.bad_status.load() == 0);

        CHECK(io_once(q, dev->nsid, in, scratch_off, len, nvme::READ));
        CHECK(memcmp(out, in, len) == 0);
        free(out);
        free(in);
    }

    test("a multi-page transfer comes back whole");
    {
        const u32 len = 8 * (u32)map::page_size;
        auto *out = static_cast<unsigned char *>(dma_buf(len));
        auto *in = static_cast<unsigned char *>(dma_buf(len));
        for (u32 i = 0; i < len; i++) {
            out[i] = (unsigned char)(i ^ (i >> 8));
        }
        memset(in, 0, len);
        CHECK(io_once(q, dev->nsid, out, scratch_off, len, nvme::WRITE));
        CHECK(io_once(q, dev->nsid, in, scratch_off, len, nvme::READ));
        CHECK(memcmp(out, in, len) == 0);
        free(out);
        free(in);
    }

    test("a batch of commands costs one doorbell write");
    {
        const unsigned n = 8;
        const u32 len = (u32)map::page_size;
        std::vector<unsigned char *> bufs;
        for (unsigned i = 0; i < n; i++) {
            auto *b = static_cast<unsigned char *>(dma_buf(len));
            memset(b, (int)(i + 1), len);
            bufs.push_back(b);
        }

        io_batch wb;
        bool placed = true;
        for (unsigned i = 0; i < n; i++) {
            placed &= submit(q, dev->nsid, bufs[i],
                             scratch_off + (u64)i * len, len, wb, nvme::WRITE);
        }
        CHECK(placed);
        // Nothing has been told to the controller yet; one write does it.
        q->ring_sq_doorbell();
        CHECK(await(q, wb, n));
        CHECK(wb.bad_status.load() == 0);

        io_batch rb;
        for (unsigned i = 0; i < n; i++) {
            memset(bufs[i], 0, len);
            placed &= submit(q, dev->nsid, bufs[i],
                             scratch_off + (u64)i * len, len, rb, nvme::READ);
        }
        CHECK(placed);
        q->ring_sq_doorbell();
        CHECK(await(q, rb, n));
        CHECK(rb.bad_status.load() == 0);
        for (unsigned i = 0; i < n; i++) {
            bool right = true;
            for (u32 k = 0; k < len; k++) {
                right &= bufs[i][k] == (unsigned char)(i + 1);
            }
            CHECK(right);
            free(bufs[i]);
        }
    }

    dev->drv->remove_io_user_queue(q);
    (void)mode;
}

void data_path_interrupts()
{
    function("io_queue_pair::submit_request, ring_sq_doorbell, process_completions "
             "(interrupt-driven)");
    if (!writable_device()) {
        test("skipped: no writable device");
        return;
    }
    data_path(true);
}

void data_path_polled()
{
    function("io_queue_pair::submit_request, ring_sq_doorbell, process_completions "
             "(polled)");
    if (!writable_device()) {
        test("skipped: no writable device");
        return;
    }
    data_path(false);
}

/* what the device can do --------------------------------------------------- */

// Keeps qd commands in flight and reports the rate. This is the denominator for
// every efficiency number later: the device's own ceiling, measured through the
// same driver the buffer manager will use.
void run_depth(nvme::io_queue_pair *q, const device &dev, const char *what,
               u32 len, unsigned qd, unsigned total, bool sequential)
{
    std::vector<unsigned char *> bufs;
    for (unsigned i = 0; i < qd; i++) {
        bufs.push_back(static_cast<unsigned char *>(dma_buf(len)));
    }
    const u64 span = dev.bytes() > len ? dev.bytes() - len : len;
    const u64 slots = span / len;

    io_batch b;
    unsigned submitted = 0;
    uint32_t seed = 12345;
    auto t0 = clk::now();
    while (submitted < total) {
        unsigned inflight = submitted - b.done.load(std::memory_order_acquire);
        if (inflight >= qd) {
            q->process_completions(0);
            continue;
        }
        u64 off = sequential ? ((u64)(submitted % slots) * len)
                             : ((u64)(lcg_next(seed) % slots) * len);
        if (!submit(q, dev.nsid, bufs[submitted % qd], off, len, b, nvme::READ)) {
            break;
        }
        q->ring_sq_doorbell();
        submitted++;
    }
    bool all = await(q, b, submitted, 60.0);
    double s = since(t0);
    CHECK(all);
    CHECK(b.bad_status.load() == 0);
    if (all && s > 0) {
        printf("    %-28s qd %3u  %7.1f MiB/s  %9.0f IOPS  %8.1f us/req\n",
               what, qd, (double)submitted * len / s / (1024 * 1024),
               submitted / s, 1e6 * s * qd / submitted);
    }
    for (auto *p : bufs) {
        free(p);
    }
}

void device_ceiling()
{
    function("device ceiling, read through the driver under test");

    if (devices.empty()) {
        test("skipped: no device");
        return;
    }
    // Reads are harmless, so this runs on the boot device if that is all there
    // is. The numbers are the same device either way.
    device &dev = devices.size() > 1 ? devices[1] : devices[0];

    test("sequential and random read rates");
    {
        auto *q = dev.drv->create_io_queue(queue_depth(dev), nullptr, false);
        CHECK(q != nullptr);
        if (q) {
            u32 big = (u32)std::min<size_t>(128 * 1024, dev.drv->max_transfer_bytes());
            run_depth(q, dev, "sequential 128 KiB", big, 32, 256, true);
            // A depth curve, not one point: a buffer manager that waits for
            // each fault runs at depth 1 per thread, so what one outstanding
            // read costs is the number that bounds it.
            for (unsigned qd : {1u, 4u, 16u, 32u}) {
                run_depth(q, dev, "random 4 KiB", (u32)map::page_size, qd,
                          qd < 4 ? 512 : 4096, false);
            }
            dev.drv->remove_io_user_queue(q);
        }
    }
}

} // namespace

int os_nvme_main()
{
    reset();
    printf("\n######## NVMe data path ########\n");

    driver_enumeration();
    queue_creation();
    classify_devices();
    data_path_interrupts();
    data_path_polled();
    device_ceiling();

    return summary("NVME");
}

#else /* !CONF_drivers_nvme */

#include <cstdio>

int os_nvme_main()
{
    printf("\n######## NVMe data path: driver not configured, skipped ########\n");
    return 0;
}

#endif
