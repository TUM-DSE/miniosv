/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024-2025 Intel Corporation
 */

// DPDK's drivers/net/intel/common/tx.h and tx_scalar.h, trimmed to the scalar path iavf uses. minidpdk mbufs carry no
// VLAN tags, TSO segment sizes or outer header lengths, so the paths that read them are gone.

#ifndef _COMMON_INTEL_TX_H_
#define _COMMON_INTEL_TX_H_

#include <stdint.h>

#include "../iavf_plat.h"

/* Common TX Descriptor QW1 Field Definitions */
#define CI_TXD_QW1_DTYPE_S      0
#define CI_TXD_QW1_DTYPE_M      (0xFUL << CI_TXD_QW1_DTYPE_S)
#define CI_TXD_QW1_CMD_S        4
#define CI_TXD_QW1_CMD_M        (0xFFFUL << CI_TXD_QW1_CMD_S)
#define CI_TXD_QW1_OFFSET_S     16
#define CI_TXD_QW1_OFFSET_M     (0x3FFFFULL << CI_TXD_QW1_OFFSET_S)
#define CI_TXD_QW1_TX_BUF_SZ_S  34
#define CI_TXD_QW1_TX_BUF_SZ_M  (0x3FFFULL << CI_TXD_QW1_TX_BUF_SZ_S)
#define CI_TXD_QW1_L2TAG1_S     48
#define CI_TXD_QW1_L2TAG1_M     (0xFFFFULL << CI_TXD_QW1_L2TAG1_S)

/* Common Descriptor Types */
#define CI_TX_DESC_DTYPE_DATA           0x0
#define CI_TX_DESC_DTYPE_CTX            0x1
#define CI_TX_DESC_DTYPE_DESC_DONE      0xF

/* Common TX Descriptor Command Flags */
#define CI_TX_DESC_CMD_EOP              0x0001
#define CI_TX_DESC_CMD_RS               0x0002
#define CI_TX_DESC_CMD_ICRC             0x0004
#define CI_TX_DESC_CMD_IL2TAG1          0x0008
#define CI_TX_DESC_CMD_DUMMY            0x0010
#define CI_TX_DESC_CMD_IIPT_IPV6        0x0020
#define CI_TX_DESC_CMD_IIPT_IPV4        0x0040
#define CI_TX_DESC_CMD_IIPT_IPV4_CSUM   0x0060
#define CI_TX_DESC_CMD_L4T_EOFT_TCP     0x0100
#define CI_TX_DESC_CMD_L4T_EOFT_SCTP    0x0200
#define CI_TX_DESC_CMD_L4T_EOFT_UDP     0x0300

/* Common TX Context Descriptor Commands */
#define CI_TX_CTX_DESC_TSO              0x01
#define CI_TX_CTX_DESC_TSYN             0x02
#define CI_TX_CTX_DESC_IL2TAG2          0x04

/* Common TX Descriptor Length Field Shifts */
#define CI_TX_DESC_LEN_MACLEN_S         0  /* 7 BITS */
#define CI_TX_DESC_LEN_IPLEN_S          7  /* 7 BITS */
#define CI_TX_DESC_LEN_L4_LEN_S         14 /* 4 BITS */

/* Common maximum data per TX descriptor */
#define CI_MAX_DATA_PER_TXD     (CI_TXD_QW1_TX_BUF_SZ_M >> CI_TXD_QW1_TX_BUF_SZ_S)

/* Checksum offload mask to identify packets requesting offload */
#define CI_TX_CKSUM_OFFLOAD_MASK (RTE_MBUF_F_TX_IP_CKSUM |		 \
				   RTE_MBUF_F_TX_L4_MASK |		 \
				   RTE_MBUF_F_TX_TCP_SEG |		 \
				   RTE_MBUF_F_TX_UDP_SEG |		 \
				   RTE_MBUF_F_TX_OUTER_IP_CKSUM |	 \
				   RTE_MBUF_F_TX_OUTER_UDP_CKSUM)

union ci_tx_offload {
	uint64_t data;
	struct {
		uint64_t l2_len:7;        /**< L2 (MAC) Header Length. */
		uint64_t l3_len:9;        /**< L3 (IP) Header Length. */
		uint64_t l4_len:8;        /**< L4 Header Length. */
		uint64_t tso_segsz:16;    /**< TCP TSO segment size */
		uint64_t outer_l2_len:8;  /**< outer L2 Header Length */
		uint64_t outer_l3_len:16; /**< outer L3 Header Length */
	};
};

/*
 * Structure of a 16-byte Tx descriptor common across i40e, ice, iavf and idpf drivers
 */
struct ci_tx_desc {
	uint64_t buffer_addr; /* Address of descriptor's data buf */
	uint64_t cmd_type_offset_bsz;
};

/**
 * Structure associated with each descriptor of the TX ring of a TX queue.
 */
struct ci_tx_entry {
	rte_mbuf *mbuf; /* mbuf associated with TX desc, if any. */
	uint16_t next_id; /* Index of next descriptor in ring. */
	uint16_t first_id; /* Split-queue: first sw_id of packet at EOP entry. */
};

struct ci_tx_queue {
	volatile struct ci_tx_desc *ci_tx_ring; /* TX ring virtual address */
	volatile uint8_t *qtx_tail;               /* register address of tail */
	struct ci_tx_entry *sw_ring; /* virtual address of SW ring */
	/* Scalar TX path: Array tracking last_id at each RS threshold boundary */
	uint16_t *rs_last_id;
	uint16_t nb_tx_desc;           /* number of TX descriptors */
	uint16_t tx_tail; /* current value of tail register */
	/* index to last TX descriptor to have been cleaned */
	uint16_t last_desc_cleaned;
	/* Total number of TX descriptors ready to be allocated. */
	uint16_t nb_tx_free;
	/* Start freeing TX buffers if there are less free descriptors than this value. */
	uint16_t tx_free_thresh;
	/* Number of TX descriptors to use before RS bit is set. */
	uint16_t tx_rs_thresh;
	/* Scalar TX path: log2 of tx_rs_thresh for efficient bit operations */
	uint8_t log2_rs_thresh;
	uint16_t port_id;  /* Device port identifier. */
	uint16_t queue_id; /* TX queue index. */
	uint16_t reg_idx;
	uint16_t tx_next_dd;
	uint16_t tx_next_rs;
	/* NULL: free each mbuf on its own. minidpdk has no raw pool put, so it stays NULL. */
	rte_mempool *fast_free_mp;
	uint64_t offloads;
	uint64_t mbuf_errors;
	rte_iova_t tx_ring_dma;        /* TX ring DMA address */
	bool tx_deferred_start; /* don't start this queue in dev start */
	bool q_set;             /* indicate if tx queue has been configured */
	struct iavf_vsi *iavf_vsi; /* the VSI this queue belongs to */
	struct iavf_dma_mem mz; /* the descriptor ring */
	/* iavf driver specific values */
#define IAVF_TX_FLAGS_VLAN_TAG_LOC_L2TAG1 BIT(0)
#define IAVF_TX_FLAGS_VLAN_TAG_LOC_L2TAG2 BIT(1)
	uint8_t vlan_flag;
	uint8_t tc;
};

static inline void
ci_txq_release_all_mbufs(struct ci_tx_queue *txq, bool use_ctx)
{
	(void)use_ctx;
	if (unlikely(!txq || !txq->sw_ring))
		return;

	/* Free mbufs from (last_desc_cleaned + 1) to (tx_tail - 1). */
	const uint16_t start = (txq->last_desc_cleaned + 1) % txq->nb_tx_desc;
	const uint16_t nb_desc = txq->nb_tx_desc;
	const uint16_t end = txq->tx_tail;

	uint16_t i = start;
	if (end < i) {
		for (; i < nb_desc; i++)
			if (txq->sw_ring[i].mbuf != NULL)
				rte_pktmbuf_free_seg(txq->sw_ring[i].mbuf);
		i = 0;
	}
	for (; i < end; i++)
		if (txq->sw_ring[i].mbuf != NULL)
			rte_pktmbuf_free_seg(txq->sw_ring[i].mbuf);
	memset(txq->sw_ring, 0, sizeof(txq->sw_ring[0]) * nb_desc);
}

static inline void
write_txd(volatile void *txd, uint64_t qw0, uint64_t qw1)
{
	/* we use an aligned structure and cast away the volatile to allow the compiler
	 * to opportunistically optimize the two 64-bit writes as a single 128-bit write.
	 */
	struct __rte_aligned(16) txdesc {
		uint64_t qw0, qw1;
	} *txdesc = RTE_CAST_PTR(struct txdesc *, txd);
	txdesc->qw0 = rte_cpu_to_le_64(qw0);
	txdesc->qw1 = rte_cpu_to_le_64(qw1);
}

/*
 * Common transmit descriptor cleanup function for Intel drivers.
 *
 * Returns:
 *   0 on success
 *  -1 if cleanup cannot proceed (descriptors not yet processed by HW)
 */
static __rte_always_inline int
ci_tx_xmit_cleanup(struct ci_tx_queue *txq)
{
	volatile struct ci_tx_desc *txd = txq->ci_tx_ring;
	const uint16_t last_desc_cleaned = txq->last_desc_cleaned;
	const uint16_t nb_tx_desc = txq->nb_tx_desc;

	/* Calculate where the next descriptor write-back will occur */
	const uint16_t rs_idx = (last_desc_cleaned == nb_tx_desc - 1) ?
			0 :
			(last_desc_cleaned + 1) >> txq->log2_rs_thresh;
	const uint16_t dd_idx = txq->rs_last_id[rs_idx];
	const uint16_t first_to_clean = rs_idx << txq->log2_rs_thresh;

	/* Check if descriptor is done - all drivers use 0xF as done value in bits 3:0 */
	if ((txd[dd_idx].cmd_type_offset_bsz & rte_cpu_to_le_64(CI_TXD_QW1_DTYPE_M)) !=
			rte_cpu_to_le_64(CI_TX_DESC_DTYPE_DESC_DONE))
		/* Descriptor not yet processed by hardware */
		return -1;

	/* DD bit is set, descriptors are done. Now free the mbufs. nb_tx_desc is a multiple of
	 * tx_rs_thresh, so cleanup never wraps around the ring within a single burst.
	 */
	const uint16_t nb_to_clean = txq->tx_rs_thresh;
	struct ci_tx_entry *sw_ring = txq->sw_ring;

	for (uint16_t i = 0; i < nb_to_clean; i++) {
		rte_mbuf *m = sw_ring[first_to_clean + i].mbuf;
		if (m != NULL) {
			rte_pktmbuf_free_seg(m);
			sw_ring[first_to_clean + i].mbuf = NULL;
		}
	}

	/* Update the txq to reflect the last descriptor that was cleaned */
	txq->last_desc_cleaned = first_to_clean + txq->tx_rs_thresh - 1;
	txq->nb_tx_free += txq->tx_rs_thresh;

	return 0;
}

/* Common checksum enable function for Intel drivers (ice, i40e, etc.) */
static inline void
ci_txd_enable_checksum(uint64_t ol_flags,
		       uint32_t *td_cmd,
		       uint32_t *td_offset,
		       union ci_tx_offload tx_offload)
{
	/* Enable L3 checksum offloads */
	if (ol_flags & RTE_MBUF_F_TX_IP_CKSUM) {
		*td_cmd |= CI_TX_DESC_CMD_IIPT_IPV4_CSUM;
		*td_offset |= (tx_offload.l3_len >> 2) << CI_TX_DESC_LEN_IPLEN_S;
	} else if (ol_flags & RTE_MBUF_F_TX_IPV4) {
		*td_cmd |= CI_TX_DESC_CMD_IIPT_IPV4;
		*td_offset |= (tx_offload.l3_len >> 2) << CI_TX_DESC_LEN_IPLEN_S;
	} else if (ol_flags & RTE_MBUF_F_TX_IPV6) {
		*td_cmd |= CI_TX_DESC_CMD_IIPT_IPV6;
		*td_offset |= (tx_offload.l3_len >> 2) << CI_TX_DESC_LEN_IPLEN_S;
	}

	if (ol_flags & RTE_MBUF_F_TX_TCP_SEG) {
		*td_cmd |= CI_TX_DESC_CMD_L4T_EOFT_TCP;
		*td_offset |= (tx_offload.l4_len >> 2) << CI_TX_DESC_LEN_L4_LEN_S;
		return;
	}

	if (ol_flags & RTE_MBUF_F_TX_UDP_SEG) {
		*td_cmd |= CI_TX_DESC_CMD_L4T_EOFT_UDP;
		*td_offset |= (tx_offload.l4_len >> 2) << CI_TX_DESC_LEN_L4_LEN_S;
		return;
	}

	/* Enable L4 checksum offloads */
	switch (ol_flags & RTE_MBUF_F_TX_L4_MASK) {
	case RTE_MBUF_F_TX_TCP_CKSUM:
		*td_cmd |= CI_TX_DESC_CMD_L4T_EOFT_TCP;
		*td_offset |= (sizeof(struct rte_tcp_hdr) >> 2) << CI_TX_DESC_LEN_L4_LEN_S;
		break;
	case RTE_MBUF_F_TX_SCTP_CKSUM:
		*td_cmd |= CI_TX_DESC_CMD_L4T_EOFT_SCTP;
		*td_offset |= (sizeof(struct rte_sctp_hdr) >> 2) << CI_TX_DESC_LEN_L4_LEN_S;
		break;
	case RTE_MBUF_F_TX_UDP_CKSUM:
		*td_cmd |= CI_TX_DESC_CMD_L4T_EOFT_UDP;
		*td_offset |= (sizeof(struct rte_udp_hdr_size) >> 2) << CI_TX_DESC_LEN_L4_LEN_S;
		break;
	default:
		break;
	}
}

typedef uint16_t (*ci_get_ctx_desc_fn)(uint64_t ol_flags, const rte_mbuf *mbuf,
		const union ci_tx_offload *tx_offload, const struct ci_tx_queue *txq,
		uint64_t *qw0, uint64_t *qw1);

static inline uint16_t
ci_xmit_pkts(struct ci_tx_queue *txq,
	     rte_mbuf **tx_pkts,
	     uint16_t nb_pkts,
	     ci_get_ctx_desc_fn get_ctx_desc)
{
	volatile struct ci_tx_desc *ci_tx_ring;
	volatile struct ci_tx_desc *txd;
	struct ci_tx_entry *sw_ring;
	struct ci_tx_entry *txe, *txn;
	rte_mbuf *tx_pkt;
	rte_mbuf *m_seg;
	uint16_t tx_id;
	uint16_t nb_tx;
	uint16_t nb_used;
	uint16_t nb_ctx;
	uint32_t td_cmd = 0;
	uint32_t td_offset = 0;
	uint16_t tx_last;
	uint16_t slen;
	uint64_t buf_dma_addr;
	uint64_t ol_flags;
	union ci_tx_offload tx_offload = {0};

	sw_ring = txq->sw_ring;
	ci_tx_ring = txq->ci_tx_ring;
	tx_id = txq->tx_tail;
	txe = &sw_ring[tx_id];

	/* Check if the descriptor ring needs to be cleaned. */
	if (txq->nb_tx_free < txq->tx_free_thresh)
		(void)ci_tx_xmit_cleanup(txq);

	for (nb_tx = 0; nb_tx < nb_pkts; nb_tx++) {
		uint64_t cd_qw0 = 0, cd_qw1 = 0;
		uint16_t pkt_rs_idx;
		tx_pkt = *tx_pkts++;

		ol_flags = tx_pkt->ol_flags;
		td_cmd = CI_TX_DESC_CMD_ICRC;
		td_offset = (tx_pkt->l2_len >> 1) << CI_TX_DESC_LEN_MACLEN_S;

		tx_offload.l2_len = tx_pkt->l2_len;
		tx_offload.l3_len = tx_pkt->l3_len;
		tx_offload.l4_len = tx_pkt->l4_len;

		/* Calculate the number of context descriptors needed. */
		nb_ctx = get_ctx_desc(ol_flags, tx_pkt, &tx_offload, txq, &cd_qw0, &cd_qw1);

		nb_used = (uint16_t)(tx_pkt->nb_segs + nb_ctx);
		tx_last = (uint16_t)(tx_id + nb_used - 1);

		/* Circular ring */
		if (tx_last >= txq->nb_tx_desc)
			tx_last = (uint16_t)(tx_last - txq->nb_tx_desc);

		/* Track the RS threshold bucket at packet start */
		pkt_rs_idx = (uint16_t)(tx_id >> txq->log2_rs_thresh);

		if (nb_used > txq->nb_tx_free) {
			if (ci_tx_xmit_cleanup(txq) != 0) {
				if (nb_tx == 0)
					return 0;
				goto end_of_tx;
			}
			if (unlikely(nb_used > txq->tx_rs_thresh)) {
				while (nb_used > txq->nb_tx_free) {
					if (ci_tx_xmit_cleanup(txq) != 0) {
						if (nb_tx == 0)
							return 0;
						goto end_of_tx;
					}
				}
			}
		}

		/* Enable checksum offloading */
		if (ol_flags & CI_TX_CKSUM_OFFLOAD_MASK)
			ci_txd_enable_checksum(ol_flags, &td_cmd,
						&td_offset, tx_offload);

		/* special case for single descriptor packet */
		if (nb_used == 1) {
			txd = &ci_tx_ring[tx_id];
			tx_id = txe->next_id;

			txe->mbuf = tx_pkt;
			/* Setup TX Descriptor */
			td_cmd |= CI_TX_DESC_CMD_EOP;
			const uint64_t cmd_type_offset_bsz = CI_TX_DESC_DTYPE_DATA |
				((uint64_t)td_cmd << CI_TXD_QW1_CMD_S) |
				((uint64_t)td_offset << CI_TXD_QW1_OFFSET_S) |
				((uint64_t)tx_pkt->data_len << CI_TXD_QW1_TX_BUF_SZ_S);
			write_txd(txd, rte_mbuf_data_iova(tx_pkt), cmd_type_offset_bsz);

			txe = &sw_ring[tx_id];
			goto end_pkt;
		}

		if (nb_ctx) {
			/* Setup TX context descriptor if required */
			uint64_t *ctx_txd = RTE_CAST_PTR(uint64_t *, &ci_tx_ring[tx_id]);

			txn = &sw_ring[txe->next_id];
			txe->mbuf = NULL;

			write_txd(ctx_txd, cd_qw0, cd_qw1);

			tx_id = txe->next_id;
			txe = txn;
		}

		m_seg = tx_pkt;

		do {
			txd = &ci_tx_ring[tx_id];
			txn = &sw_ring[txe->next_id];

			rte_mbuf *next_seg = m_seg->next;
			slen = m_seg->data_len;
			buf_dma_addr = rte_mbuf_data_iova(m_seg);

			/* fill the last descriptor with End of Packet (EOP) bit */
			if (next_seg == NULL)
				td_cmd |= CI_TX_DESC_CMD_EOP;

			const uint64_t cmd_type_offset_bsz = CI_TX_DESC_DTYPE_DATA |
				((uint64_t)td_cmd << CI_TXD_QW1_CMD_S) |
				((uint64_t)td_offset << CI_TXD_QW1_OFFSET_S) |
				((uint64_t)slen << CI_TXD_QW1_TX_BUF_SZ_S);
			write_txd(txd, buf_dma_addr, cmd_type_offset_bsz);
			txe->mbuf = m_seg;

			tx_id = txe->next_id;
			txe = txn;
			m_seg = next_seg;
		} while (m_seg);
end_pkt:
		txq->nb_tx_free = (uint16_t)(txq->nb_tx_free - nb_used);

		/* Set the RS bit on the last descriptor of a packet that crosses into a new
		 * RS threshold bucket, and record it for the bucket being left.
		 */
		uint16_t next_rs_idx = ((tx_last + 1) >> txq->log2_rs_thresh);

		if (next_rs_idx != pkt_rs_idx) {
			txd->cmd_type_offset_bsz |=
					rte_cpu_to_le_64(CI_TX_DESC_CMD_RS << CI_TXD_QW1_CMD_S);
			txq->rs_last_id[pkt_rs_idx] = tx_last;
		}
	}
end_of_tx:
	/* update Tail register */
	rte_write32_wc(tx_id, txq->qtx_tail);
	txq->tx_tail = tx_id;

	return nb_tx;
}

#endif /* _COMMON_INTEL_TX_H_ */
