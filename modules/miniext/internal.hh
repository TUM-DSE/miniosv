/*
 * miniext internals shared between the translation units. Not for app use.
 */

#ifndef MINIEXT_INTERNAL_HH
#define MINIEXT_INTERNAL_HH

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <osv/condvar.h>
#include <osv/mem/frames.hh>
#include <osv/mem/phys.hh>
#include <osv/mutex.h>
#include <osv/rwlock.h>
#include <osv/sched.hh>

#include "miniext.hh"
#include "ondisk.hh"

namespace nvme { class io_queue_pair; class nvme_driver; }

namespace miniext {

// transfers in flight
struct io_group {
    std::atomic<unsigned> outstanding{1};   // the submitter's own hold
    std::atomic<int> error{0};
    sched::thread_handle waiter;

    std::atomic<void (*)(void *)> on_settled{nullptr};
    void *settled_arg = nullptr;
    std::atomic<bool> finished{false};

    void hold() { outstanding.fetch_add(1, std::memory_order_relaxed); }
    void drop();
    bool settled() const { return finished.load(std::memory_order_acquire); }
};

// --- device -------------------------------------------------------------
// Block layer folded inside the filesysstem. It owns NVMe queues (1 per vCPU)
//  and translate filesystem block numbers into byte offsets for the NVMe driver.
//
// Note the driver's submit_request() takes a BYTE offset and BYTE length
// despite its parameter names, rings the doorbell itself, and returns 1 on
// success / 0 when the submission queue was full and nothing was queued.
//
class device {
public:
    int open(int nvme_id);
    int set_block_size(uint32_t block_size);
    void close();

    // Blocking for the caller; concurrent callers proceed in parallel.
    // `block` and `count` are in filesystem blocks.
    int read(void *buf, uint64_t block, uint32_t count);
    int write(const void *buf, uint64_t block, uint32_t count);
    int flush();
    int read_async(void *buf, uint64_t block, uint32_t count, io_group &g);
    int write_async(const void *buf, uint64_t block, uint32_t count, io_group &g);

    uint64_t lba_count() const { return _lba_count; }
    uint32_t lba_size() const { return _lba_size; }
    size_t queue_count() const { return _queues ? _queues->size() : 0; }

    struct queue {
        nvme::io_queue_pair *q = nullptr;
        mutex lock;             // submission only, never held across the wait
    };

    // NVMe hardware queues are created once by the driver and shared in the kernel
    //
    // Sharing is safe for the same reason concurrent callers are: each queue
    // has its own lock, held only across submission.
    using queue_set = std::vector<std::unique_ptr<queue>>;

private:
    static std::shared_ptr<queue_set> queues_for(int nvme_id,
                                                 nvme::nvme_driver *drv);
    int submit(void *buf, uint64_t block, uint32_t count, bool write);
    int submit_async(void *buf, uint64_t block, uint32_t count, bool write, io_group &g);
    int submit_one(void *buf, uint64_t block, uint32_t count, bool write);
    int submit_one_async(void *buf, uint64_t block, uint32_t count, bool write, io_group &g);
    // Blocks one command may carry, from the controller's transfer limit.
    uint32_t max_blocks() const;
    int bounce(void *buf, uint64_t block, uint32_t count, bool write);
    bool in_range(uint64_t block, uint32_t count) const;
    queue &pick();

    std::shared_ptr<queue_set> _queues;
    uint32_t _lba_size = 0;
    uint64_t _lba_count = 0;
    uint32_t _block_size = 0;
    uint32_t _lbas_per_block = 0;
    size_t _max_bytes = 0;      // most one command may transfer
};

// --- scratch buffers ----------------------------------------------------
//
// A block-sized staging buffer, allocated and freed around a single access.
// Used to manipulate metadata smaller than a 4096-byte block.
class scratch {
public:
    explicit scratch(uint32_t size)
        : _pa(mem::frames::alloc(size, size)), _size(size)
    {
        _p = _pa ? static_cast<uint8_t *>(mem::map_phys(_pa, size)) : nullptr;
    }
    ~scratch()
    {
        if (_p) {
            mem::frames::free(_pa, _size);
        }
    }
    scratch(const scratch &) = delete;
    scratch &operator=(const scratch &) = delete;

    uint8_t *data() { return _p; }
    explicit operator bool() const { return _p != nullptr; }

private:
    mem::frames::phys_addr _pa;
    uint8_t *_p;
    uint32_t _size;
};

// --- mounted filesystem -------------------------------------------------

struct fs {
    bool mounted = false;
    std::string mount_point;

    device dev;

    superblock sb;
    std::vector<group_desc> groups;

    uint32_t block_size = 0;
    uint32_t inode_size = 0;
    uint32_t inodes_per_group = 0;
    uint32_t blocks_per_group = 0;
    uint32_t group_count = 0;
    uint64_t block_count = 0;
    uint32_t first_data_block = 0;

    // Control path only: mount/umount, path resolution, directory mutation and
    // the open-inode table. Data-path reads and writes do not take it.
    mutex lock;

    // Bitmaps + group descriptors + superblock counters, which must move
    // together. Held only while claiming or releasing blocks and inodes, never
    // across a data transfer.
    mutex alloc_lock;
};

// --- open inodes --------------------------------------------------------
//
// One entry per inode that is currently open, shared by every handle onto it,
// so a reader and a writer see the same inode rather than private stale copies.
//
// `lock` gives POSIX-level behaviour and no more: pread takes it shared so
// reads on one file run in parallel, pwrite/truncate take it exclusive so a
// read never observes a half-applied write.
struct open_inode {
    uint32_t ino = 0;
    inode in;
    rwlock lock;
    int refs = 0;

    /*
     * Reads that have left the lock but not the device.
     *
     * A read lock cannot be held from aread() to await(): a caller batching
     * several transfers would hold it several times over, and a writer
     * arriving between two of them blocks the second -- the lock stops
     * granting reads as soon as a writer is waiting. So a read holds it only
     * while it decides where the blocks are, and counts itself here for as
     * long as a transfer is on its way to them.
     *
     * A writer waits for that count to reach zero. It is holding the lock by
     * then, so nothing new can join.
     */
    mutex io_lock;
    condvar io_idle;
    unsigned inflight = 0;
};

// One more transfer under way, and one fewer. Called with the read lock held
// and with nothing held, in that order.
void oi_io_begin(open_inode *oi);
void oi_io_end(open_inode *oi);

// Wait for the transfers already under way. With the write lock held.
void oi_io_drain(open_inode *oi);

// An open handle: which inode, and what it was opened for.
struct file {
    open_inode *oi;
    int flags;
};

open_inode *oi_get(fs *f, uint32_t ino, const inode *in);
void oi_put(fs *f, open_inode *oi);

fs *get_fs();

// --- metadata writeback -------------------------------------------------
//
// Metadata writebacks are persisted before they returns. There is no cache
// and no journal, so an interrupted sequence can leave the filesystem
// inconsistent -- the order things are written in is the only ordering
// guarantee there is. Allocate-then-link, and unlink-then-free.

int sb_write(fs *f);
int gd_write(fs *f, uint32_t group);

// --- allocators ---------------------------------------------------------
//
// Bitmap based. `goal` is a hint: allocation starts scanning at that block's
// group so a growing file stays contiguous, which is what keeps extent counts
// low enough for the depth limit below.

int block_alloc(fs *f, uint64_t goal, uint32_t want, uint64_t *out, uint32_t *got);
int block_free(fs *f, uint64_t block, uint32_t count);
int inode_alloc(fs *f, bool is_dir, uint32_t *out);
int inode_free(fs *f, uint32_t ino, bool was_dir);

// --- inode --------------------------------------------------------------

int inode_read(fs *f, uint32_t ino, inode *out);
int inode_write(fs *f, uint32_t ino, const inode *in);
void inode_init(fs *f, inode *in, uint16_t mode);
uint64_t inode_size(const inode *in);
void inode_set_size(inode *in, uint64_t size);
void inode_mark_deleted(inode *in);
bool inode_is_dir(const inode *in);
bool inode_is_reg(const inode *in);

// i_blocks counts 512-byte units, and includes extent-tree blocks.
void inode_add_blocks(fs *f, inode *in, int64_t delta_fs_blocks);

// --- extents ------------------------------------------------------------
//
// Map file block `fblock` to a physical block. Sets *phys to 0 for a hole (an
// unallocated range or an uninitialised extent), which reads as zeros.
// *run is the number of consecutive blocks that share the mapping, so callers
// can coalesce I/O.
// --- extent tree block cache --------------------------------------------
//
// See etcache.cc. read() is a read-through: it fills dst either from the cache
// or from the device. Every write of an extent tree block must invalidate it.
namespace etcache {
struct stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t evictions = 0;
    unsigned held = 0;
    size_t bytes = 0;
};
void configure(unsigned blocks, uint32_t block_size);
void teardown();
unsigned capacity();
int read(fs *f, uint64_t block, uint8_t *dst);
void invalidate(uint64_t block);
stats report();
}

int extent_lookup(fs *f, const inode *in, uint32_t fblock,
                  uint64_t *phys, uint32_t *run);

// Ensure file block `fblock` is backed, allocating up to `want` consecutive
// blocks if it is not. *phys/*run describe the resulting mapping.
int extent_map_write(fs *f, uint32_t ino, inode *in, uint32_t fblock,
                     uint32_t want, uint64_t *phys, uint32_t *run);

// Free every block from file block `from` onwards and trim the tree.
int extent_truncate(fs *f, uint32_t ino, inode *in, uint32_t from);

// --- directories --------------------------------------------------------

// Find `name` (not NUL-terminated, `name_len` bytes) in directory inode `dir`.
// Returns the inode number, or 0 if absent.
int dir_lookup(fs *f, const inode *dir, const char *name, size_t name_len,
               uint32_t *out_ino);

int dir_iterate(fs *f, const inode *dir,
                const std::function<bool(const char *, size_t, uint32_t, uint8_t)> &cb);

// Link `ino` into directory `dir` under `name`, growing the directory by a
// block if no existing record has room.
int dir_add(fs *f, uint32_t dir_ino, inode *dir, const char *name,
            size_t name_len, uint32_t ino, uint8_t file_type);

// Unlink `name`, merging its record into the preceding one.
int dir_remove(fs *f, uint32_t dir_ino, inode *dir, const char *name,
               size_t name_len);

// Split a path into its parent directory and final component.
int path_split(fs *f, const char *path, uint32_t *parent_ino, inode *parent,
               const char **name, size_t *name_len);

// --- path resolution ----------------------------------------------------

// Resolve an absolute path (including the mount point prefix) to an inode.
int path_resolve(fs *f, const char *path, uint32_t *out_ino, inode *out);

// Read `len` bytes at `offset` from an inode's data.
int64_t inode_pread(fs *f, const inode *in, void *buf, size_t len, uint64_t offset);

// Write `len` bytes at `offset`, allocating blocks as needed and extending
// i_size. The inode is written back before this returns.
int64_t inode_pwrite(fs *f, uint32_t ino, inode *in, const void *buf, size_t len,
                     uint64_t offset);

int inode_truncate(fs *f, uint32_t ino, inode *in, uint64_t new_size);

} // namespace miniext

#endif /* MINIEXT_INTERNAL_HH */
