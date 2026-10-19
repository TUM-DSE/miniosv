/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2021 Intel Corporation
 */

// The DPDK base code's OS layer, on minidpdk and OSv instead of the EAL.

#ifndef _IAVF_OSDEP_H_
#define _IAVF_OSDEP_H_

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <api/minidpdk/bit.hh>
#include <api/minidpdk/time.hh>
#include <api/minidpdk/util.hh>

#ifndef __INTEL_NET_BASE_OSDEP__
#define __INTEL_NET_BASE_OSDEP__

#define INLINE inline
#define STATIC static

// u8..s64 come from OSv.
#include <osv/types.h>

#ifndef __le16
#define __le16 uint16_t
#endif
#ifndef __le32
#define __le32 uint32_t
#endif
#ifndef __le64
#define __le64 uint64_t
#endif
#ifndef __be16
#define __be16 uint16_t
#endif
#ifndef __be32
#define __be32 uint32_t
#endif
#ifndef __be64
#define __be64 uint64_t
#endif

#define __rte_packed_begin
#define __rte_packed_end __attribute__((__packed__))
#ifndef RTE_SIZEOF_FIELD
#define RTE_SIZEOF_FIELD(type, field) (sizeof(((type *)0)->field))
#endif

#define min(a, b) RTE_MIN(a, b)
#define max(a, b) RTE_MAX(a, b)

#define FIELD_SIZEOF(t, f) RTE_SIZEOF_FIELD(t, f)
#define ARRAY_SIZE(arr) RTE_DIM(arr)

// x86 and aarch64 are little-endian.
#define rte_cpu_to_le_16(x) ((uint16_t)(x))
#define rte_cpu_to_le_32(x) ((uint32_t)(x))
#define rte_cpu_to_le_64(x) ((uint64_t)(x))
#define rte_le_to_cpu_16(x) ((uint16_t)(x))
#define rte_le_to_cpu_32(x) ((uint32_t)(x))
#define rte_le_to_cpu_64(x) ((uint64_t)(x))
#define rte_cpu_to_be_16(x) __builtin_bswap16(x)
#define rte_cpu_to_be_32(x) __builtin_bswap32(x)
#define rte_cpu_to_be_64(x) __builtin_bswap64(x)
#define rte_be_to_cpu_16(x) __builtin_bswap16(x)
#define rte_be_to_cpu_32(x) __builtin_bswap32(x)
#define rte_be_to_cpu_64(x) __builtin_bswap64(x)

#define CPU_TO_LE16(o) rte_cpu_to_le_16(o)
#define CPU_TO_LE32(s) rte_cpu_to_le_32(s)
#define CPU_TO_LE64(h) rte_cpu_to_le_64(h)
#define LE16_TO_CPU(a) rte_le_to_cpu_16(a)
#define LE32_TO_CPU(c) rte_le_to_cpu_32(c)
#define LE64_TO_CPU(k) rte_le_to_cpu_64(k)

#define CPU_TO_BE16(o) rte_cpu_to_be_16(o)
#define CPU_TO_BE32(o) rte_cpu_to_be_32(o)
#define CPU_TO_BE64(o) rte_cpu_to_be_64(o)

#define NTOHS(a) rte_be_to_cpu_16(a)
#define NTOHL(a) rte_be_to_cpu_32(a)
#define HTONS(a) rte_cpu_to_be_16(a)
#define HTONL(a) rte_cpu_to_be_32(a)

static inline uint32_t readl(volatile void *addr)
{
	return rte_read32(addr);
}

static inline void writel(uint32_t value, volatile void *addr)
{
	rte_write32(value, addr);
}

static inline void writel_relaxed(uint32_t value, volatile void *addr)
{
	rte_write32_relaxed(value, addr);
}

static inline uint64_t readq(volatile void *addr)
{
	uint64_t v = mmio_getq(addr);
	rte_rmb();
	return v;
}

static inline void writeq(uint64_t value, volatile void *addr)
{
	rte_wmb();
	mmio_setq(addr, value);
}

#define wr32(a, reg, value) writel((value), (a)->hw_addr + (reg))
#define rd32(a, reg) readl((a)->hw_addr + (reg))
#define wr64(a, reg, value) writeq((value), (a)->hw_addr + (reg))
#define rd64(a, reg) readq((a)->hw_addr + (reg))

#endif /* __INTEL_NET_BASE_OSDEP__ */

#define iavf_memset(a, b, c, d) memset((a), (b), (c))
#define iavf_memcpy(a, b, c, d) memcpy((a), (b), (c))

// The mailbox polls with these while waiting on the PF; sleeping lets the cpu run something else.
#define iavf_usec_delay(x) rte_delay_us_sleep(x)
#define iavf_msec_delay(x) iavf_usec_delay(1000 * (x))

#define IAVF_PCI_REG_WRITE(reg, value) writel(value, reg)
#define IAVF_PCI_REG_WRITE_RELAXED(reg, value) writel_relaxed(value, reg)

#define IAVF_READ_REG(hw, reg) rd32(hw, reg)
#define IAVF_WRITE_REG(hw, reg, value) wr32(hw, reg, value)

#define IAVF_WRITE_FLUSH(a) IAVF_READ_REG(a, IAVF_VFGEN_RSTAT)

extern int iavf_log_level;

#define DEBUGOUT(S, ...)                                                       \
	do {                                                                   \
		if (iavf_log_level >= 7)                                       \
			printf("iavf: " S, ##__VA_ARGS__);                     \
	} while (0)
#define DEBUGOUT2(S, ...) DEBUGOUT(S, ##__VA_ARGS__)
#define DEBUGFUNC(F) DEBUGOUT(F "\n")

#define iavf_debug(h, m, s, ...)                                               \
	do {                                                                   \
		if (((m) & (h)->debug_mask))                                   \
			DEBUGOUT("iavf %02x.%x " s, (h)->bus.device,           \
				 (h)->bus.func, ##__VA_ARGS__);                \
	} while (0)

/* memory allocation tracking */
struct __rte_packed_begin iavf_dma_mem {
	void *va;
	u64 pa;
	u32 size;
	const void *zone;
} __rte_packed_end;

struct __rte_packed_begin iavf_virt_mem {
	void *va;
	u32 size;
} __rte_packed_end;

#define iavf_allocate_dma_mem(h, m, unused, s, a) iavf_allocate_dma_mem_d(h, m, s, a)
#define iavf_free_dma_mem(h, m) iavf_free_dma_mem_d(h, m)

#define iavf_allocate_virt_mem(h, m, s) iavf_allocate_virt_mem_d(h, m, s)
#define iavf_free_virt_mem(h, m) iavf_free_virt_mem_d(h, m)

/* SW spinlock */
struct iavf_spinlock {
	rte_spinlock_t spinlock;
};

#define iavf_init_spinlock(sp) rte_spinlock_init(&(sp)->spinlock)
#define iavf_acquire_spinlock(sp) rte_spinlock_lock(&(sp)->spinlock)
#define iavf_release_spinlock(sp) rte_spinlock_unlock(&(sp)->spinlock)
#define iavf_destroy_spinlock(sp) RTE_SET_USED(sp)

#endif /* _IAVF_OSDEP_H_ */
