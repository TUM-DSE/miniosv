/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

// DPDK's drivers/net/intel/common/rx.h, trimmed to what the iavf scalar paths use.

#ifndef _COMMON_INTEL_RX_H_
#define _COMMON_INTEL_RX_H_

#include <stddef.h>
#include <stdint.h>

#include "../iavf_plat.h"
#include "desc.h"

#define CI_RX_MAX_BURST 32

struct ci_rx_queue;
struct iavf_rxq_intr;

struct ci_rx_entry {
	rte_mbuf *mbuf; /* mbuf associated with RX descriptor. */
};

/**
 * Structure associated with each RX queue.
 */
struct ci_rx_queue {
	rte_mempool  *mp; /**< mbuf pool to populate RX ring. */
	union { /* RX ring virtual address */
		volatile union ci_rx_desc *rx_ring;
		volatile union ci_rx_flex_desc *rx_flex_ring;
	};
	volatile uint8_t *qrx_tail;   /**< register address of tail */
	struct ci_rx_entry *sw_ring; /**< address of RX software ring. */
	rte_iova_t rx_ring_phys_addr; /**< RX ring DMA address. */
	rte_mbuf *pkt_first_seg; /**< First segment of current packet. */
	rte_mbuf *pkt_last_seg; /**< Last segment of current packet. */
	/** hold packets to return to application */
	rte_mbuf *rx_stage[CI_RX_MAX_BURST * 2];
	uint16_t nb_rx_desc; /**< number of RX descriptors. */
	uint16_t rx_tail;  /**< current value of tail register. */
	uint16_t rx_nb_avail; /**< nr of staged pkts ready to ret to app */
	uint16_t nb_rx_hold; /**< number of held free RX desc. */
	uint16_t rx_next_avail; /**< idx of next staged pkt to ret to app */
	uint16_t rx_free_thresh; /**< max free RX desc to hold. */
	uint16_t rx_free_trigger; /**< triggers rx buffer allocation */
	uint16_t queue_id; /**< RX queue index. */
	uint16_t port_id;  /**< Device port identifier. */
	uint16_t reg_idx;  /**< RX queue register index. */
	uint16_t rx_buf_len; /* The packet buffer size */
	uint16_t rx_hdr_len; /* The header buffer size */
	uint16_t max_pkt_len; /* Maximum packet length */
	uint8_t crc_len;  /**< 0 if CRC stripped, 4 otherwise. */
	bool q_set; /**< indicate if rx queue has been configured */
	bool rx_deferred_start; /**< queue is not started on dev start. */
	bool fdir_enabled; /* 0 if FDIR disabled, 1 when enabled */
	uint64_t offloads; /**< Rx offloads with RTE_ETH_RX_OFFLOAD_* */
	uint32_t rxdid; /**< RX descriptor format ID. */
	/** need to alloc dummy mbuf, for wraparound when scanning hw ring */
	rte_mbuf fake_mbuf;
	struct iavf_vsi *iavf_vsi; /* the VSI this queue belongs to */
	struct iavf_dma_mem mz; /* the descriptor ring */
	/* iavf specific values */
	const struct iavf_rxq_ops *ops; /**< queue ops */
	uint8_t rel_mbufs_type; /**< type of release mbuf function */
	uint8_t rx_flags; /**< Rx VLAN tag location flags */
#define IAVF_RX_FLAGS_VLAN_TAG_LOC_L2TAG1     BIT(0)
#define IAVF_RX_FLAGS_VLAN_TAG_LOC_L2TAG2_2   BIT(1)
	/* miniosv: interrupt-mode receive, null when the application polls */
	struct iavf_rxq_intr *intr;
};

static inline uint16_t
ci_rx_reassemble_packets(rte_mbuf **rx_bufs, uint16_t nb_bufs, uint8_t *split_flags,
		rte_mbuf **pkt_first_seg, rte_mbuf **pkt_last_seg,
		const uint8_t crc_len)
{
	rte_mbuf *pkts[CI_RX_MAX_BURST] = {0}; /*finished pkts*/
	rte_mbuf *start = *pkt_first_seg;
	rte_mbuf *end = *pkt_last_seg;
	unsigned int pkt_idx, buf_idx;

	for (buf_idx = 0, pkt_idx = 0; buf_idx < nb_bufs; buf_idx++) {
		if (end) {
			/* processing a split packet */
			end->next = rx_bufs[buf_idx];
			rx_bufs[buf_idx]->data_len += crc_len;

			start->nb_segs++;
			start->pkt_len += rx_bufs[buf_idx]->data_len;
			end = end->next;

			if (!split_flags[buf_idx]) {
				/* it's the last packet of the set */
				start->hash = end->hash;
				start->ol_flags = end->ol_flags;
				/* we need to strip crc for the whole packet */
				start->pkt_len -= crc_len;
				if (end->data_len > crc_len) {
					end->data_len -= crc_len;
				} else {
					/* free up last mbuf */
					rte_mbuf *secondlast = start;

					start->nb_segs--;
					while (secondlast->next != end)
						secondlast = secondlast->next;
					secondlast->data_len -= (crc_len - end->data_len);
					secondlast->next = NULL;
					rte_pktmbuf_free_seg(end);
				}
				pkts[pkt_idx++] = start;
				start = NULL;
				end = NULL;
			}
		} else {
			/* not processing a split packet */
			if (!split_flags[buf_idx]) {
				/* not a split packet, save and skip */
				pkts[pkt_idx++] = rx_bufs[buf_idx];
				continue;
			}
			start = rx_bufs[buf_idx];
			end = start;
			rx_bufs[buf_idx]->data_len += crc_len;
			rx_bufs[buf_idx]->pkt_len += crc_len;
		}
	}

	/* save the partial packet for next time */
	*pkt_first_seg = start;
	*pkt_last_seg = end;
	memcpy(rx_bufs, pkts, pkt_idx * (sizeof(*pkts)));
	return pkt_idx;
}

#endif /* _COMMON_INTEL_RX_H_ */
