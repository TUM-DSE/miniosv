/*
 * Types shared by the memory layers.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_TYPES_HH
#define OSV_MEM_TYPES_HH

#include <stddef.h>
#include <stdint.h>

namespace mem {
namespace frames {

// A physical address. Not a pointer: physical memory is not addressable until it is mapped.
using phys_addr = uint64_t;

// Nothing is allocated at physical 0, used to return "no memory" from alloc() and claim_run().
constexpr phys_addr no_memory = 0;

}

// A half-open range of virtual addresses.
struct range {
    uintptr_t start = 0;
    uintptr_t end = 0;

    size_t size() const { return end - start; }
    bool empty() const { return end <= start; }
    bool contains(uintptr_t a) const { return a >= start && a < end; }
    bool contains(const range &r) const { return r.start >= start && r.end <= end; }
    bool intersects(const range &r) const { return r.start < end && start < r.end; }
    range clamp(const range &r) const {
        return {r.start > start ? r.start : start, r.end < end ? r.end : end};
    }
};

// Access permissions. The values match the hardware-facing permission bits.
enum {
    perm_none = 0,
    perm_read = 1,
    perm_write = 2,
    perm_exec = 4,
    perm_rw = perm_read | perm_write,
    perm_rwx = perm_read | perm_write | perm_exec,
};

// How the hardware may reorder, cache and combine accesses to a mapping.
// aarch64 distinguishes normal from dev. x64 adds wc, write-combining, for a
// device BAR that is written in bursts and never read back (ENA's LLQ): the
// hypervisor leaves such a BAR uncached, which makes every store a serialised
// transaction unless the guest asks for combining itself.
enum class mattr {
    normal,
    dev,
    wc,
};

}

#endif
