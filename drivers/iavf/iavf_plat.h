/* SPDX-License-Identifier: BSD-3-Clause */

// What the DPDK iavf PMD takes from the EAL, ethdev and mbuf libraries, on minidpdk and OSv. The base code has its own
// layer, base/iavf_osdep.h, which this includes.

#ifndef IAVF_PLAT_H
#define IAVF_PLAT_H

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <api/minidpdk/bit.hh>
#include <api/minidpdk/defs.hh>
#include <api/minidpdk/dev.hh>
#include <api/minidpdk/mem.hh>
#include <api/minidpdk/net.hh>
#include <api/minidpdk/rss.hh>
#include <api/minidpdk/time.hh>
#include <api/minidpdk/util.hh>

#include "base/iavf_osdep.h"

/* The base code's min()/max() macros would rewrite std::min and std::max in every header included after this. */
#undef min
#undef max

/* minidpdk's prefetch takes no volatile pointer, and descriptor rings are volatile. */
#undef rte_prefetch0
#define rte_prefetch0(a) __builtin_prefetch((const void *)(uintptr_t)(a), 0, 0)
#define __rte_cold __attribute__((cold))

static inline int rte_spinlock_trylock(rte_spinlock_t *lock)
{
	irq_lock.lock();
	if (np_spin_trylock(lock))
		return 1;
	irq_lock.unlock();
	return 0;
}

typedef uint64_t rte_iova_t;
typedef uint16_t rte_le16_t;
typedef uint32_t rte_le32_t;
typedef uint64_t rte_le64_t;

#define RTE_ATOMIC(type) std::atomic<type>
#define rte_memory_order_relaxed std::memory_order_relaxed
#define rte_memory_order_acquire std::memory_order_acquire
#define rte_memory_order_release std::memory_order_release
#define rte_memory_order_seq_cst std::memory_order_seq_cst
#define rte_atomic_load_explicit(p, o) (p)->load(o)
#define rte_atomic_store_explicit(p, v, o) (p)->store(v, o)
#define rte_atomic_fetch_add_explicit(p, v, o) (p)->fetch_add(v, o)
#define rte_atomic_fetch_sub_explicit(p, v, o) (p)->fetch_sub(v, o)
#define rte_atomic_compare_exchange_strong_explicit(p, e, d, s, f) (p)->compare_exchange_strong(*(e), d, s, f)
#define rte_atomic_thread_fence(o) std::atomic_thread_fence(o)

#define __rte_aligned(a) __attribute__((__aligned__(a)))
#define RTE_CAST_PTR(type, ptr) ((type)(uintptr_t)(ptr))
#define RTE_ALIGN_FLOOR(val, align) ((val) & ~((decltype(val))((align) - 1)))
#define RTE_ALIGN_CEIL(val, align) RTE_ALIGN_FLOOR((val) + ((decltype(val))(align) - 1), align)
#define RTE_ALIGN(val, align) RTE_ALIGN_CEIL(val, align)
#define RTE_LEN2MASK(ln, tp) ((tp)((uint64_t)-1 >> (sizeof(uint64_t) * CHAR_BIT - (ln))))
#define rte_popcount32(x) __builtin_popcount(x)
#define RTE_PTR_ADD(ptr, x) ((void *)((uintptr_t)(ptr) + (x)))
static inline uint32_t rte_log2_u32(uint32_t v)
{
	return v ? 31 - __builtin_clz(v) : 0;
}

/* Logging: the level a message needs, against iavf_log_level (base/iavf_osdep.cc). */
#define IAVF_LOG_ERR 3
#define IAVF_LOG_WARNING 4
#define IAVF_LOG_NOTICE 5
#define IAVF_LOG_INFO 6
#define IAVF_LOG_DEBUG 7
#define IAVF_LOG(level, fmt, ...)                                              \
	do {                                                                   \
		if (IAVF_LOG_##level <= iavf_log_level)                         \
			printf("iavf: " fmt "\n", ##__VA_ARGS__);              \
	} while (0)
#define PMD_INIT_LOG(level, ...) IAVF_LOG(level, __VA_ARGS__)
#define PMD_DRV_LOG(level, ...) IAVF_LOG(level, __VA_ARGS__)
#define PMD_RX_LOG(level, ...) IAVF_LOG(level, __VA_ARGS__)
#define PMD_TX_LOG(level, ...) IAVF_LOG(level, __VA_ARGS__)
#define PMD_INIT_FUNC_TRACE() do { } while (0)

/* Plain memory; nothing the device reads goes through here. */
static inline void *rte_zmalloc(const char *, size_t size, unsigned)
{
	return calloc(1, size);
}
static inline void *rte_zmalloc_socket(const char *, size_t size, unsigned, int)
{
	return calloc(1, size);
}
static inline void *rte_malloc(const char *, size_t size, unsigned)
{
	return malloc(size);
}

/* Doorbells. The tail registers are mapped uncached, so these are plain stores. */
static inline void rte_write32_wc(uint32_t value, volatile void *addr)
{
	rte_wmb();
	mmio_setl(addr, value);
}
static inline void rte_write32_wc_relaxed(uint32_t value, volatile void *addr)
{
	mmio_setl(addr, value);
}

#define IAVF_PCI_REG_WC_WRITE(reg, value) rte_write32_wc((value), reg)
#define IAVF_PCI_REG_WC_WRITE_RELAXED(reg, value) rte_write32_wc_relaxed((value), reg)

/* Ethernet */
#define RTE_ETHER_MTU 1500
#define RTE_ETHER_MIN_MTU 68
#define RTE_ETHER_MAX_LEN 1518
#define RTE_VLAN_HLEN 4
#define RTE_ETHER_TYPE_VLAN 0x8100
#define RTE_ETHER_TYPE_QINQ 0x88A8

static inline bool rte_is_zero_ether_addr(const struct rte_ether_addr *ea)
{
	for (auto b : ea->addr)
		if (b)
			return false;
	return true;
}
static inline bool rte_is_multicast_ether_addr(const struct rte_ether_addr *ea)
{
	return ea->addr[0] & 0x01;
}
static inline bool rte_is_valid_assigned_ether_addr(const struct rte_ether_addr *ea)
{
	return !rte_is_multicast_ether_addr(ea) && !rte_is_zero_ether_addr(ea);
}
static inline bool rte_is_same_ether_addr(const struct rte_ether_addr *a, const struct rte_ether_addr *b)
{
	return a->addr == b->addr;
}

/* Header sizes the Tx checksum offload programs into descriptors. */
struct rte_tcp_hdr {
	uint8_t bytes[20];
};
struct rte_udp_hdr_size {
	uint8_t bytes[8];
};
struct rte_sctp_hdr {
	uint8_t bytes[12];
};

/*
 * mbufs. minidpdk's iova already points at the data, past the headroom, and a freshly allocated mbuf is fully
 * initialised by its pool.
 */
#define RTE_PKTMBUF_HEADROOM minidpdk::mem_pool::kDefaultHeadroom
#define rte_mbuf_data_iova(m) ((rte_iova_t)(m)->iova)
#define rte_mbuf_data_iova_default(m) ((rte_iova_t)(m)->iova)

static inline rte_mbuf *rte_mbuf_raw_alloc(rte_mempool *mp)
{
	return rte_pktmbuf_alloc(mp);
}
static inline int rte_mbuf_raw_alloc_bulk(rte_mempool *mp, rte_mbuf **mbufs, unsigned n)
{
	return rte_pktmbuf_alloc_bulk(mp, mbufs, n);
}
#define rte_mempool_get_bulk(mp, objs, n) rte_mbuf_raw_alloc_bulk(mp, (rte_mbuf **)(objs), n)

/* One segment only: minidpdk's free walks the chain, and the Tx ring holds each segment in its own slot. */
static inline void rte_pktmbuf_free_seg(rte_mbuf *m)
{
	m->next = nullptr;
	m->nb_segs = 1;
	rte_pktmbuf_free(m);
}
static inline void rte_mbuf_raw_free_bulk(rte_mempool *, rte_mbuf **mbufs, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		rte_pktmbuf_free_seg(mbufs[i]);
}
static inline void rte_mempool_put(rte_mempool *, rte_mbuf *m)
{
	rte_pktmbuf_free_seg(m);
}
static inline uint16_t rte_pktmbuf_data_room_size(rte_mempool *mp)
{
	return mp->get_data_size() + RTE_PKTMBUF_HEADROOM;
}

#endif /* IAVF_PLAT_H */
