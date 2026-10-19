/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019-2021 Intel Corporation
 */

// DMA and plain memory for the base code: physically contiguous frames mapped once, as the ENA driver does.

#include <cstdlib>

#include <osv/mem/frames.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/phys.hh>

#include "iavf_prototype.h"
#include "iavf_type.h"

int iavf_log_level = 4;

enum iavf_status iavf_allocate_dma_mem_d(struct iavf_hw *hw, struct iavf_dma_mem *mem, u64 size,
					 u32 alignment)
{
	(void)hw;
	if (!mem)
		return IAVF_ERR_PARAM;
	size_t align = alignment > mem::mapping::page_size ? alignment : mem::mapping::page_size;
	auto pa = mem::frames::alloc(size, align);
	if (pa == mem::frames::no_memory)
		return IAVF_ERR_NO_MEMORY;
	mem->va = mem::map_phys(pa, size);
	// The descriptor rings poll a done bit; stale bytes would read as completions.
	memset(mem->va, 0, size);
	mem->pa = pa;
	mem->size = size;
	mem->zone = nullptr;
	return IAVF_SUCCESS;
}

enum iavf_status iavf_free_dma_mem_d(struct iavf_hw *hw, struct iavf_dma_mem *mem)
{
	(void)hw;
	if (!mem)
		return IAVF_ERR_PARAM;
	if (mem->va)
		mem::frames::free(mem->pa, mem->size);
	mem->va = nullptr;
	mem->pa = 0;
	return IAVF_SUCCESS;
}

enum iavf_status iavf_allocate_virt_mem_d(struct iavf_hw *hw, struct iavf_virt_mem *mem, u32 size)
{
	(void)hw;
	if (!mem)
		return IAVF_ERR_PARAM;
	mem->size = size;
	mem->va = calloc(1, size);
	return mem->va ? IAVF_SUCCESS : IAVF_ERR_NO_MEMORY;
}

enum iavf_status iavf_free_virt_mem_d(struct iavf_hw *hw, struct iavf_virt_mem *mem)
{
	(void)hw;
	if (!mem)
		return IAVF_ERR_PARAM;
	free(mem->va);
	mem->va = nullptr;
	return IAVF_SUCCESS;
}
