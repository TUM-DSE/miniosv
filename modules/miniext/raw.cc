/*
 * Reading an NVMe namespace as one flat file (miniext.hh, namespace raw).
 *
 * This simply translates byte offsets into LBAs and copies the partial LBAs
 * at each end through a scratch buffer.
 *
 * Reads run at LBA granularity because that is the device's unit, so the
 * "block size" here is the LBA size: no filesystem, and nothing to align to
 * except the hardware.
 */

#include <cerrno>
#include <cstring>

#include <algorithm>

#include <osv/mem/mapping.hh>
#include <osv/mem/store.hh>

#include "drivers/nvme.hh"

#include "internal.hh"

namespace miniext {
namespace raw {

struct device {
    miniext::device dev;
    uint32_t lba_size = 0;
    uint64_t bytes = 0;
    size_t max_transfer = 0;    // what one command may carry; see store_open
};

device *open(int nvme_id, int *err)
{
    auto set = [err](int e) { if (err) { *err = e; } };

    auto *d = new device();
    int rc = d->dev.open(nvme_id);
    if (rc < 0) {
        delete d;
        set(rc);
        return nullptr;
    }
    // One LBA per "block": the device layer's block abstraction has nothing to
    // model here, so make the two the same size and let it pass reads through.
    rc = d->dev.set_block_size(d->dev.lba_size());
    if (rc < 0) {
        d->dev.close();
        delete d;
        set(rc);
        return nullptr;
    }

    d->lba_size = d->dev.lba_size();
    d->bytes = d->dev.lba_count() * static_cast<uint64_t>(d->lba_size);

    // The driver builds one PRP list page per command: prp1 plus 512 list
    // entries, so 512 pages for the page-aligned buffers the cache hands us.
    constexpr size_t prp_limit = 512 * NVME_PAGESIZE;
    auto *drv = nvme::nvme_driver::get_nvme_device(nvme_id);
    const size_t mdts = drv ? drv->max_transfer_bytes() : 0;
    d->max_transfer = (mdts && mdts < prp_limit) ? mdts : prp_limit;

    set(0);
    return d;
}

void close(device *d)
{
    if (!d) {
        return;
    }
    d->dev.close();
    delete d;
}

uint64_t size(device *d)
{
    return d ? d->bytes : 0;
}

int64_t pread(device *d, void *buf, size_t len, uint64_t offset)
{
    if (!d || !buf) {
        return -EINVAL;
    }
    if (offset >= d->bytes) {
        return 0;
    }
    if (len > d->bytes - offset) {
        len = d->bytes - offset;
    }
    if (len == 0) {
        return 0;
    }

    const uint32_t lba_size = d->lba_size;
    uint8_t *out = static_cast<uint8_t *>(buf);
    size_t moved = 0;

    while (moved < len) {
        const uint64_t pos = offset + moved;
        const uint64_t lba = pos / lba_size;
        const uint32_t in_lba = static_cast<uint32_t>(pos % lba_size);
        size_t want = len - moved;

        if (in_lba == 0 && want >= lba_size) {
            // Whole LBAs, straight into the caller's buffer.
            const uint32_t count = static_cast<uint32_t>(want / lba_size);
            int rc = d->dev.read(out + moved, lba, count);
            if (rc < 0) {
                return rc;
            }
            moved += static_cast<size_t>(count) * lba_size;
            continue;
        }

        // A partial LBA at one end or the other.
        const size_t to_lba_end = lba_size - in_lba;
        if (want > to_lba_end) {
            want = to_lba_end;
        }
        scratch s(lba_size);
        if (!s) {
            return -ENOMEM;
        }
        int rc = d->dev.read(s.data(), lba, 1);
        if (rc < 0) {
            return rc;
        }
        memcpy(out + moved, s.data() + in_lba, want);
        moved += want;
    }

    return static_cast<int64_t>(moved);
}

/* The page cache's backend, with no filesystem under it. */

namespace {

struct raw_io {
    io_group g;
    int64_t moved = 0;
    int err = 0;
};

class raw_store : public mem::store {
public:
    raw_store(device *d, size_t unit, size_t max_transfer)
        : _d(d), _unit(unit), _max_transfer(max_transfer) {}

    uint64_t size() override { return _d->bytes; }
    size_t granularity() override { return _unit; }

    mem::io *read(void *buf, uint64_t offset, size_t bytes) override
    {
        return start(buf, offset, bytes, false);
    }

    mem::io *write(const void *buf, uint64_t offset, size_t bytes) override
    {
        return start(const_cast<void *>(buf), offset, bytes, true);
    }

    bool done(mem::io *req) override
    {
        auto *r = reinterpret_cast<raw_io *>(req);
        return r && r->g.settled();
    }

    bool subscribe(mem::io *req, void (*cb)(void *), void *arg) override
    {
        auto *r = reinterpret_cast<raw_io *>(req);
        if (!r) {
            return false;
        }
        r->g.settled_arg = arg;
        r->g.on_settled.store(cb, std::memory_order_release);
        if (r->g.settled()) {
            if (auto f = r->g.on_settled.exchange(nullptr,
                                                  std::memory_order_acq_rel)) {
                f(arg);
            }
        }
        return true;
    }

    int64_t wait(mem::io *req) override
    {
        auto *r = reinterpret_cast<raw_io *>(req);
        if (!r) {
            return -EINVAL;
        }
        r->g.waiter.reset(*sched::thread::current());
        sched::thread::wait_until([r] { return r->g.settled(); });
        r->g.waiter.clear();
        int err = r->err ? r->err : r->g.error.load(std::memory_order_relaxed);
        int64_t moved = r->moved;
        if (err) {
            printf("raw_store: transfer failed err=%d moved=%ld\n", err, moved);
        }
        delete r;
        return err ? err : moved;
    }

private:
    mem::io *start(void *buf, uint64_t offset, size_t bytes, bool write);

    device *_d;
    size_t _unit;
    size_t _max_transfer;
};

mem::io *raw_store::start(void *buf, uint64_t offset, size_t bytes, bool write)
{
    auto *r = new (std::nothrow) raw_io();
    if (!r) {
        return nullptr;
    }

    if (offset >= _d->bytes) {
        bytes = 0;
    } else if (bytes > _d->bytes - offset) {
        bytes = _d->bytes - offset;
    }

    const uint32_t lba_size = _d->lba_size;
    if (bytes == 0) {
        // Nothing to submit, so the group settles on the drop below.
    } else if (offset % lba_size || bytes % lba_size) {
        // The cache faults at granularity boundaries, which are whole LBAs, so
        // anything else is a caller's mistake rather than a case to bounce.
        printf("raw_store: unaligned %s off=%lu bytes=%zu lba=%u\n",
               write ? "write" : "read", offset, bytes, lba_size);
        r->err = -EINVAL;
    } else {
        const size_t max_transfer = _max_transfer;

        size_t done = 0;
        while (done < bytes) {
            size_t piece = bytes - done;
            if (piece > max_transfer) {
                piece = max_transfer;
            }
            const uint64_t lba = (offset + done) / lba_size;
            const auto count = static_cast<uint32_t>(piece / lba_size);
            auto *at = static_cast<uint8_t *>(buf) + done;
            int rc = write ? _d->dev.write_async(at, lba, count, r->g)
                           : _d->dev.read_async(at, lba, count, r->g);
            if (rc < 0) {
                printf("raw_store: %s_async failed rc=%d lba=%lu count=%u buf=%p\n",
                       write ? "write" : "read", rc, lba, count, at);
                r->err = rc;
                break;
            }
            done += piece;
        }
        r->moved = static_cast<int64_t>(done);
    }

    r->g.drop();
    return reinterpret_cast<mem::io *>(r);
}

} // namespace

mem::store *store_open(device *d)
{
    if (!d) {
        return nullptr;
    }
    // The cache hands out memory, so its unit cannot be smaller than a page
    // however small the device's LBA is.
    size_t unit = std::max<size_t>(d->lba_size, mem::mapping::page_size);
    return new (std::nothrow) raw_store(d, unit, d->max_transfer);
}

void store_close(mem::store *s)
{
    delete s;
}

} // namespace raw
} // namespace miniext
