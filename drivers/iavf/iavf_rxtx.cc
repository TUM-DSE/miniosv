/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017 Intel Corporation
 */

// DPDK's drivers/net/intel/iavf/iavf_rxtx.c: queue setup and the scalar Rx and Tx paths.

#include "iavf.h"
#include "iavf_ethdev.hh"
#include "iavf_rxtx.h"

static inline int
check_tx_thresh(uint16_t nb_desc, uint16_t tx_rs_thresh,
		uint16_t tx_free_thresh)
{
	/* TX descriptors will have their RS bit set after tx_rs_thresh
	 * descriptors have been used. The TX descriptor ring will be cleaned
	 * after tx_free_thresh descriptors are used or if the number of
	 * descriptors required to transmit a packet is greater than the
	 * number of free TX descriptors.
	 *
	 * The following constraints must be satisfied:
	 *  - tx_rs_thresh must be less than the size of the ring minus 2.
	 *  - tx_free_thresh must be less than the size of the ring minus 3.
	 *  - tx_rs_thresh must be less than or equal to tx_free_thresh.
	 *  - tx_rs_thresh must be a divisor of the ring size.
	 *
	 * One descriptor in the TX ring is used as a sentinel to avoid a H/W
	 * race condition, hence the maximum threshold constraints. When set
	 * to zero use default values.
	 */
	if (tx_rs_thresh >= (nb_desc - 2)) {
		PMD_INIT_LOG(ERR, "tx_rs_thresh (%u) must be less than the "
			     "number of TX descriptors (%u) minus 2",
			     tx_rs_thresh, nb_desc);
		return -EINVAL;
	}
	if (tx_free_thresh >= (nb_desc - 3)) {
		PMD_INIT_LOG(ERR, "tx_free_thresh (%u) must be less than the "
			     "number of TX descriptors (%u) minus 3.",
			     tx_free_thresh, nb_desc);
		return -EINVAL;
	}
	if (tx_rs_thresh > tx_free_thresh) {
		PMD_INIT_LOG(ERR, "tx_rs_thresh (%u) must be less than or "
			     "equal to tx_free_thresh (%u).",
			     tx_rs_thresh, tx_free_thresh);
		return -EINVAL;
	}
	if ((nb_desc % tx_rs_thresh) != 0) {
		PMD_INIT_LOG(ERR, "tx_rs_thresh (%u) must be a divisor of the "
			     "number of TX descriptors (%u).",
			     tx_rs_thresh, nb_desc);
		return -EINVAL;
	}
	if (!rte_is_power_of_2(tx_rs_thresh)) {
		PMD_INIT_LOG(ERR, "tx_rs_thresh must be a power of 2. (tx_rs_thresh=%u)",
			     tx_rs_thresh);
		return -EINVAL;
	}

	return 0;
}

static inline bool
check_rx_bulk_allow(struct ci_rx_queue *rxq)
{
	int ret = true;

	if (!(rxq->rx_free_thresh >= IAVF_RX_MAX_BURST)) {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions: "
			     "rxq->rx_free_thresh=%d, "
			     "IAVF_RX_MAX_BURST=%d",
			     rxq->rx_free_thresh, IAVF_RX_MAX_BURST);
		ret = false;
	} else if (rxq->nb_rx_desc % rxq->rx_free_thresh != 0) {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions: "
			     "rxq->nb_rx_desc=%d, "
			     "rxq->rx_free_thresh=%d",
			     rxq->nb_rx_desc, rxq->rx_free_thresh);
		ret = false;
	}
	return ret;
}

static inline void
reset_rx_queue(struct ci_rx_queue *rxq)
{
	uint16_t len;
	uint32_t i;

	if (!rxq)
		return;

	len = rxq->nb_rx_desc + IAVF_RX_MAX_BURST;

	for (i = 0; i < len * sizeof(union ci_rx_desc); i++)
		((volatile char *)rxq->rx_ring)[i] = 0;

	memset(&rxq->fake_mbuf, 0x0, sizeof(rxq->fake_mbuf));

	for (i = 0; i < IAVF_RX_MAX_BURST; i++)
		rxq->sw_ring[rxq->nb_rx_desc + i].mbuf = &rxq->fake_mbuf;

	/* for rx bulk */
	rxq->rx_nb_avail = 0;
	rxq->rx_next_avail = 0;
	rxq->rx_free_trigger = (uint16_t)(rxq->rx_free_thresh - 1);

	rxq->rx_tail = 0;
	rxq->nb_rx_hold = 0;

	/* minidpdk's free, unlike DPDK's, takes no NULL. */
	if (rxq->pkt_first_seg)
		rte_pktmbuf_free(rxq->pkt_first_seg);

	rxq->pkt_first_seg = NULL;
	rxq->pkt_last_seg = NULL;
}

static inline void
reset_tx_queue(struct ci_tx_queue *txq)
{
	struct ci_tx_entry *txe;
	uint32_t i, size;
	uint16_t prev;

	if (!txq) {
		PMD_DRV_LOG(DEBUG, "Pointer to txq is NULL");
		return;
	}

	txe = txq->sw_ring;
	size = sizeof(struct ci_tx_desc) * txq->nb_tx_desc;
	for (i = 0; i < size; i++)
		((volatile char *)txq->ci_tx_ring)[i] = 0;

	prev = (uint16_t)(txq->nb_tx_desc - 1);
	for (i = 0; i < txq->nb_tx_desc; i++) {
		txq->ci_tx_ring[i].cmd_type_offset_bsz =
			rte_cpu_to_le_64(CI_TX_DESC_DTYPE_DESC_DONE);
		txe[i].mbuf =  NULL;
		txe[prev].next_id = i;
		prev = i;
	}

	txq->tx_tail = 0;

	txq->last_desc_cleaned = txq->nb_tx_desc - 1;
	txq->nb_tx_free = txq->nb_tx_desc - 1;

	txq->tx_next_dd = txq->tx_rs_thresh - 1;
	txq->tx_next_rs = txq->tx_rs_thresh - 1;
}

static int
alloc_rxq_mbufs(struct ci_rx_queue *rxq)
{
	volatile union ci_rx_desc *rxd;
	rte_mbuf *mbuf = NULL;
	uint64_t dma_addr;
	uint16_t i, j;

	for (i = 0; i < rxq->nb_rx_desc; i++) {
		mbuf = rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(!mbuf)) {
			for (j = 0; j < i; j++) {
				rte_pktmbuf_free_seg(rxq->sw_ring[j].mbuf);
				rxq->sw_ring[j].mbuf = NULL;
			}
			PMD_DRV_LOG(ERR, "Failed to allocate mbuf for RX");
			return -ENOMEM;
		}

		/* minidpdk's pool hands out mbufs initialised, the data at the default headroom. */
		mbuf->next = NULL;
		mbuf->nb_segs = 1;

		dma_addr =
			rte_cpu_to_le_64(rte_mbuf_data_iova_default(mbuf));

		rxd = &rxq->rx_ring[i];
		rxd->read.pkt_addr = dma_addr;
		rxd->read.hdr_addr = 0;
		rxd->read.rsvd1 = 0;
		rxd->read.rsvd2 = 0;

		rxq->sw_ring[i].mbuf = mbuf;
	}

	return 0;
}

static inline void
release_rxq_mbufs(struct ci_rx_queue *rxq)
{
	uint16_t i;

	if (!rxq->sw_ring)
		return;

	for (i = 0; i < rxq->nb_rx_desc; i++) {
		if (rxq->sw_ring[i].mbuf) {
			rte_pktmbuf_free_seg(rxq->sw_ring[i].mbuf);
			rxq->sw_ring[i].mbuf = NULL;
		}
	}

	/* for rx bulk */
	if (rxq->rx_nb_avail == 0)
		return;
	for (i = 0; i < rxq->rx_nb_avail; i++) {
		rte_mbuf *mbuf;

		mbuf = rxq->rx_stage[rxq->rx_next_avail + i];
		rte_pktmbuf_free_seg(mbuf);
	}
	rxq->rx_nb_avail = 0;
}

/*
 * What a flex descriptor carries past the packet type and checksums: the RSS hash, whose offset depends on the
 * profile. DPDK also copies flow director ids and protocol-extraction metadata into dynamic mbuf fields, which
 * minidpdk mbufs do not have.
 */
static inline void
iavf_rxd_to_pkt_fields(struct ci_rx_queue *rxq, rte_mbuf *mb,
		       volatile union ci_rx_flex_desc *rxdp)
{
	uint16_t stat_err;
	uint32_t hash;

	switch (rxq->rxdid) {
	case IAVF_RXDID_LEGACY_0:
	case IAVF_RXDID_LEGACY_1:
	case IAVF_RXDID_COMMS_OVS_1: {
		volatile struct iavf_32b_rx_flex_desc_comms_ovs *desc =
				(volatile struct iavf_32b_rx_flex_desc_comms_ovs *)rxdp;
		stat_err = rte_le_to_cpu_16(desc->status_error0);
		hash = rte_le_to_cpu_32(desc->rss_hash);
		break;
	}
	default: {
		volatile struct iavf_32b_rx_flex_desc_comms *desc =
				(volatile struct iavf_32b_rx_flex_desc_comms *)rxdp;
		stat_err = rte_le_to_cpu_16(desc->status_error0);
		hash = rte_le_to_cpu_32(desc->rss_hash);
		break;
	}
	}
	if (likely(stat_err & (1 << IAVF_RX_FLEX_DESC_STATUS0_RSS_VALID_S))) {
		mb->ol_flags |= RTE_MBUF_F_RX_RSS_HASH;
		mb->hash.rss = hash;
	}
}

static void
iavf_select_rxd_to_pkt_fields_handler(struct ci_rx_queue *rxq, uint32_t rxdid)
{
	rxq->rxdid = rxdid;

	switch (rxdid) {
	case IAVF_RXDID_COMMS_OVS_1:
	case IAVF_RXDID_LEGACY_0:
	case IAVF_RXDID_LEGACY_1:
		break;
	default:
		/* update this according to the RXDID for FLEX_DESC_NONE */
		rxq->rxdid = IAVF_RXDID_COMMS_OVS_1;
		break;
	}
}

int
iavf_dev_rx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
		       uint16_t nb_desc, unsigned int socket_id,
		       const struct rte_eth_rxconf *rx_conf,
		       rte_mempool *mp)
{
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(dev->data.data);
	struct iavf_adapter *ad =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf =
		IAVF_DEV_PRIVATE_TO_VF(dev->data.data);
	struct iavf_vsi *vsi = &vf->vsi;
	struct ci_rx_queue *rxq;
	uint32_t ring_size;
	uint16_t len;
	uint16_t rx_free_thresh;
	uint64_t offloads;

	PMD_INIT_FUNC_TRACE();

	if (ad->closed)
		return -EIO;

	offloads = rx_conf->offloads | dev->data.dev_conf.rxmode.offloads;

	if (nb_desc % IAVF_ALIGN_RING_DESC != 0 ||
	    nb_desc > IAVF_MAX_RING_DESC ||
	    nb_desc < IAVF_MIN_RING_DESC) {
		PMD_INIT_LOG(ERR, "Number (%u) of receive descriptors is "
			     "invalid", nb_desc);
		return -EINVAL;
	}

	/* Check free threshold */
	rx_free_thresh = (rx_conf->rx_free_thresh == 0) ?
			 IAVF_DEFAULT_RX_FREE_THRESH :
			 rx_conf->rx_free_thresh;
	/* Check that ring size is > 2 * rx_free_thresh */
	if (nb_desc <= 2 * rx_free_thresh) {
		PMD_INIT_LOG(ERR, "rx ring size (%u) must be > 2 * rx_free_thresh (%u)",
			     nb_desc, rx_free_thresh);
		if (nb_desc == IAVF_MIN_RING_DESC)
			PMD_INIT_LOG(ERR, "To use the minimum ring size (%u), reduce rx_free_thresh to a lower value (recommended %u)",
				     IAVF_MIN_RING_DESC, IAVF_MIN_RING_DESC / 4);
		return -EINVAL;
	}

	/* Free memory if needed */
	if (dev->data.rx_queues[queue_idx]) {
		iavf_dev_rx_queue_release(dev, queue_idx);
		dev->data.rx_queues[queue_idx] = NULL;
	}

	/* Allocate the rx queue data structure */
	rxq = (struct ci_rx_queue *)rte_zmalloc_socket("iavf rxq",
				 sizeof(struct ci_rx_queue),
				 RTE_CACHE_LINE_SIZE,
				 socket_id);
	if (!rxq) {
		PMD_INIT_LOG(ERR, "Failed to allocate memory for "
			     "rx queue data structure");
		return -ENOMEM;
	}

	/* The flex profile without protocol extraction: packet type, checksums, RSS hash. */
	if (vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RX_FLEX_DESC)
		rxq->rxdid = IAVF_RXDID_COMMS_OVS_1;
	else
		rxq->rxdid = IAVF_RXDID_LEGACY_1;

	rxq->rx_flags = IAVF_RX_FLAGS_VLAN_TAG_LOC_L2TAG1;

	iavf_select_rxd_to_pkt_fields_handler(rxq, rxq->rxdid);

	rxq->mp = mp;
	rxq->nb_rx_desc = nb_desc;
	rxq->rx_free_thresh = rx_free_thresh;
	rxq->queue_id = queue_idx;
	rxq->port_id = dev->data.port_id;
	rxq->rx_deferred_start = false;
	rxq->rx_hdr_len = 0;
	rxq->iavf_vsi = vsi;
	rxq->offloads = offloads;

	if (dev->data.dev_conf.rxmode.offloads & RTE_ETH_RX_OFFLOAD_KEEP_CRC)
		rxq->crc_len = RTE_ETHER_CRC_LEN;
	else
		rxq->crc_len = 0;

	len = rte_pktmbuf_data_room_size(rxq->mp) - RTE_PKTMBUF_HEADROOM;
	rxq->rx_buf_len = RTE_ALIGN_FLOOR(len, (1 << IAVF_RXQ_CTX_DBUFF_SHIFT));
	rxq->rx_buf_len = RTE_MIN(rxq->rx_buf_len, IAVF_RX_MAX_DATA_BUF_SIZE);

	/* Allocate the software ring. */
	len = nb_desc + IAVF_RX_MAX_BURST;
	rxq->sw_ring = (struct ci_rx_entry *)
		rte_zmalloc_socket("iavf rx sw ring",
				   sizeof(rte_mbuf *) * len,
				   RTE_CACHE_LINE_SIZE,
				   socket_id);
	if (!rxq->sw_ring) {
		PMD_INIT_LOG(ERR, "Failed to allocate memory for SW ring");
		rte_free(rxq);
		return -ENOMEM;
	}

	/* Allocate the maximum number of RX ring hardware descriptor with
	 * a little more to support bulk allocate.
	 */
	len = IAVF_MAX_RING_DESC + IAVF_RX_MAX_BURST;
	ring_size = RTE_ALIGN(len * sizeof(union ci_rx_desc),
			      IAVF_DMA_MEM_ALIGN);
	if (iavf_allocate_dma_mem_d(hw, &rxq->mz, ring_size, IAVF_RING_BASE_ALIGN) != IAVF_SUCCESS) {
		PMD_INIT_LOG(ERR, "Failed to reserve DMA memory for RX");
		rte_free(rxq->sw_ring);
		rte_free(rxq);
		return -ENOMEM;
	}
	/* Zero all the descriptors in the ring. */
	memset(rxq->mz.va, 0, ring_size);
	rxq->rx_ring_phys_addr = rxq->mz.pa;
	rxq->rx_ring = (union ci_rx_desc *)rxq->mz.va;

	reset_rx_queue(rxq);
	rxq->q_set = true;
	dev->data.rx_queues[queue_idx] = rxq;
	rxq->qrx_tail = hw->hw_addr + IAVF_QRX_TAIL1(rxq->queue_id);

	if (check_rx_bulk_allow(rxq) == true) {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions are "
			     "satisfied. Rx Burst Bulk Alloc function will be "
			     "used on port=%d, queue=%d.",
			     rxq->port_id, rxq->queue_id);
	} else {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions are "
			     "not satisfied, Scattered Rx is requested "
			     "on port=%d, queue=%d.",
			     rxq->port_id, rxq->queue_id);
		ad->rx_bulk_alloc_allowed = false;
	}
	return 0;
}

int
iavf_dev_tx_queue_setup(struct rte_eth_dev *dev,
		       uint16_t queue_idx,
		       uint16_t nb_desc,
		       unsigned int socket_id,
		       const struct rte_eth_txconf *tx_conf)
{
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(dev->data.data);
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf =
		IAVF_DEV_PRIVATE_TO_VF(dev->data.data);
	struct iavf_vsi *vsi = &vf->vsi;
	struct ci_tx_queue *txq;
	uint32_t ring_size;
	uint16_t tx_rs_thresh, tx_free_thresh;
	uint64_t offloads;

	PMD_INIT_FUNC_TRACE();

	if (adapter->closed)
		return -EIO;

	offloads = tx_conf->offloads | dev->data.dev_conf.txmode.offloads;

	if (nb_desc % IAVF_ALIGN_RING_DESC != 0 ||
	    nb_desc > IAVF_MAX_RING_DESC ||
	    nb_desc < IAVF_MIN_RING_DESC) {
		PMD_INIT_LOG(ERR, "Number (%u) of transmit descriptors is "
			    "invalid", nb_desc);
		return -EINVAL;
	}

	/* minidpdk's txconf has no RS threshold. */
	tx_rs_thresh = DEFAULT_TX_RS_THRESH;
	tx_free_thresh = (uint16_t)((tx_conf->tx_free_thresh) ?
		tx_conf->tx_free_thresh : DEFAULT_TX_FREE_THRESH);
	if (check_tx_thresh(nb_desc, tx_rs_thresh, tx_free_thresh) != 0)
		return -EINVAL;

	/* Free memory if needed. */
	if (dev->data.tx_queues[queue_idx]) {
		iavf_dev_tx_queue_release(dev, queue_idx);
		dev->data.tx_queues[queue_idx] = NULL;
	}

	/* Allocate the TX queue data structure. */
	txq = (struct ci_tx_queue *)rte_zmalloc_socket("iavf txq",
				 sizeof(struct ci_tx_queue),
				 RTE_CACHE_LINE_SIZE,
				 socket_id);
	if (!txq) {
		PMD_INIT_LOG(ERR, "Failed to allocate memory for "
			     "tx queue structure");
		return -ENOMEM;
	}

	txq->vlan_flag = IAVF_TX_FLAGS_VLAN_TAG_LOC_L2TAG1;

	txq->nb_tx_desc = nb_desc;
	txq->tx_rs_thresh = tx_rs_thresh;
	txq->log2_rs_thresh = rte_log2_u32(tx_rs_thresh);
	txq->tx_free_thresh = tx_free_thresh;
	txq->queue_id = queue_idx;
	txq->port_id = dev->data.port_id;
	txq->fast_free_mp = NULL;
	txq->offloads = offloads;
	txq->tx_deferred_start = false;
	txq->iavf_vsi = vsi;

	/* Allocate software ring */
	txq->sw_ring = (struct ci_tx_entry *)
		rte_zmalloc_socket("iavf tx sw ring",
				   sizeof(struct ci_tx_entry) * nb_desc,
				   RTE_CACHE_LINE_SIZE,
				   socket_id);
	if (!txq->sw_ring) {
		PMD_INIT_LOG(ERR, "Failed to allocate memory for SW TX ring");
		rte_free(txq);
		return -ENOMEM;
	}

	/* Allocate RS last_id tracking array */
	uint16_t num_rs_buckets = nb_desc / tx_rs_thresh;
	txq->rs_last_id = (uint16_t *)rte_zmalloc_socket(NULL, sizeof(txq->rs_last_id[0]) * num_rs_buckets,
			RTE_CACHE_LINE_SIZE, socket_id);
	if (txq->rs_last_id == NULL) {
		PMD_INIT_LOG(ERR, "Failed to allocate memory for RS last_id array");
		rte_free(txq->sw_ring);
		rte_free(txq);
		return -ENOMEM;
	}

	/* Allocate TX hardware ring descriptors. */
	ring_size = sizeof(struct ci_tx_desc) * IAVF_MAX_RING_DESC;
	ring_size = RTE_ALIGN(ring_size, IAVF_DMA_MEM_ALIGN);
	if (iavf_allocate_dma_mem_d(hw, &txq->mz, ring_size, IAVF_RING_BASE_ALIGN) != IAVF_SUCCESS) {
		PMD_INIT_LOG(ERR, "Failed to reserve DMA memory for TX");
		rte_free(txq->rs_last_id);
		rte_free(txq->sw_ring);
		rte_free(txq);
		return -ENOMEM;
	}
	txq->tx_ring_dma = txq->mz.pa;
	txq->ci_tx_ring = (struct ci_tx_desc *)txq->mz.va;

	reset_tx_queue(txq);
	txq->q_set = true;
	dev->data.tx_queues[queue_idx] = txq;
	txq->qtx_tail = hw->hw_addr + IAVF_QTX_TAIL1(queue_idx);


	return 0;
}

int
iavf_dev_rx_queue_start(struct rte_eth_dev *dev, uint16_t rx_queue_id)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(dev->data.data);
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(dev->data.data);
	struct ci_rx_queue *rxq;
	int err = 0;

	if (rx_queue_id >= dev->data.nb_rx_queues)
		return -EINVAL;

	rxq = (struct ci_rx_queue *)dev->data.rx_queues[rx_queue_id];

	err = alloc_rxq_mbufs(rxq);
	if (err) {
		PMD_DRV_LOG(ERR, "Failed to allocate RX queue mbuf");
		return err;
	}

	rte_wmb();

	/* Init the RX tail register. */
	IAVF_PCI_REG_WRITE(rxq->qrx_tail, rxq->nb_rx_desc - 1);
	IAVF_WRITE_FLUSH(hw);

	/* Ready to switch the queue on */
	if (!vf->lv_enabled)
		err = iavf_switch_queue(adapter, rx_queue_id, true, true);
	else
		err = iavf_switch_queue_lv(adapter, rx_queue_id, true, true);

	if (err) {
		release_rxq_mbufs(rxq);
		PMD_DRV_LOG(ERR, "Failed to switch RX queue %u on",
			    rx_queue_id);
	} else {
		dev->data.rx_queue_state[rx_queue_id] =
			RTE_ETH_QUEUE_STATE_STARTED;
	}

	return err;
}

int
iavf_dev_tx_queue_start(struct rte_eth_dev *dev, uint16_t tx_queue_id)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(dev->data.data);
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(dev->data.data);
	struct ci_tx_queue *txq;
	int err = 0;

	if (tx_queue_id >= dev->data.nb_tx_queues)
		return -EINVAL;

	txq = (struct ci_tx_queue *)dev->data.tx_queues[tx_queue_id];

	/* Init the RX tail register. */
	IAVF_PCI_REG_WRITE(txq->qtx_tail, 0);
	IAVF_WRITE_FLUSH(hw);

	/* Ready to switch the queue on */
	if (!vf->lv_enabled)
		err = iavf_switch_queue(adapter, tx_queue_id, false, true);
	else
		err = iavf_switch_queue_lv(adapter, tx_queue_id, false, true);

	if (err)
		PMD_DRV_LOG(ERR, "Failed to switch TX queue %u on",
			    tx_queue_id);
	else
		dev->data.tx_queue_state[tx_queue_id] =
			RTE_ETH_QUEUE_STATE_STARTED;

	return err;
}

int
iavf_dev_rx_queue_stop(struct rte_eth_dev *dev, uint16_t rx_queue_id)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(dev->data.data);
	struct ci_rx_queue *rxq;
	int err;

	if (rx_queue_id >= dev->data.nb_rx_queues)
		return -EINVAL;

	if (!vf->lv_enabled)
		err = iavf_switch_queue(adapter, rx_queue_id, true, false);
	else
		err = iavf_switch_queue_lv(adapter, rx_queue_id, true, false);

	if (err) {
		PMD_DRV_LOG(ERR, "Failed to switch RX queue %u off",
			    rx_queue_id);
		return err;
	}

	rxq = (struct ci_rx_queue *)dev->data.rx_queues[rx_queue_id];
	release_rxq_mbufs(rxq);
	reset_rx_queue(rxq);
	dev->data.rx_queue_state[rx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;

	return 0;
}

int
iavf_dev_tx_queue_stop(struct rte_eth_dev *dev, uint16_t tx_queue_id)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(dev->data.data);
	struct ci_tx_queue *txq;
	int err;

	if (tx_queue_id >= dev->data.nb_tx_queues)
		return -EINVAL;

	if (!vf->lv_enabled)
		err = iavf_switch_queue(adapter, tx_queue_id, false, false);
	else
		err = iavf_switch_queue_lv(adapter, tx_queue_id, false, false);

	if (err) {
		PMD_DRV_LOG(ERR, "Failed to switch TX queue %u off",
			    tx_queue_id);
		return err;
	}

	txq = (struct ci_tx_queue *)dev->data.tx_queues[tx_queue_id];
	ci_txq_release_all_mbufs(txq, false);
	reset_tx_queue(txq);
	dev->data.tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;

	return 0;
}

void
iavf_dev_rx_queue_release(struct rte_eth_dev *dev, uint16_t qid)
{
	struct ci_rx_queue *q = (struct ci_rx_queue *)dev->data.rx_queues[qid];

	if (!q)
		return;

	release_rxq_mbufs(q);
	rte_free(q->sw_ring);
	iavf_free_dma_mem_d(IAVF_DEV_PRIVATE_TO_HW(dev->data.data), &q->mz);
	delete q->intr;
	rte_free(q);
}

void
iavf_dev_tx_queue_release(struct rte_eth_dev *dev, uint16_t qid)
{
	struct ci_tx_queue *q = (struct ci_tx_queue *)dev->data.tx_queues[qid];

	if (!q)
		return;

	ci_txq_release_all_mbufs(q, false);
	rte_free(q->sw_ring);
	rte_free(q->rs_last_id);
	iavf_free_dma_mem_d(IAVF_DEV_PRIVATE_TO_HW(dev->data.data), &q->mz);
	rte_free(q);
}

static void
iavf_reset_queues(struct rte_eth_dev *dev)
{
	struct ci_rx_queue *rxq;
	struct ci_tx_queue *txq;
	int i;

	for (i = 0; i < dev->data.nb_tx_queues; i++) {
		txq = (struct ci_tx_queue *)dev->data.tx_queues[i];
		if (!txq)
			continue;
		ci_txq_release_all_mbufs(txq, false);
		reset_tx_queue(txq);
		dev->data.tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
	}
	for (i = 0; i < dev->data.nb_rx_queues; i++) {
		rxq = (struct ci_rx_queue *)dev->data.rx_queues[i];
		if (!rxq)
			continue;
		release_rxq_mbufs(rxq);
		reset_rx_queue(rxq);
		dev->data.rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
	}
}

void
iavf_stop_queues(struct rte_eth_dev *dev)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(dev->data.data);
	int ret;

	/* adminq will be disabled when vf is resetting. */
	if (rte_atomic_load_explicit(&vf->in_reset_recovery, rte_memory_order_relaxed)) {
		iavf_reset_queues(dev);
		return;
	}

	/* Stop All queues */
	if (!vf->lv_enabled) {
		ret = iavf_disable_queues(adapter);
		if (ret)
			PMD_DRV_LOG(WARNING, "Fail to stop queues");
	} else {
		ret = iavf_disable_queues_lv(adapter);
		if (ret)
			PMD_DRV_LOG(WARNING, "Fail to stop queues for large VF");
	}

	if (ret)
		PMD_DRV_LOG(WARNING, "Fail to stop queues");

	iavf_reset_queues(dev);
}

#define IAVF_RX_FLEX_ERR0_BITS	\
	((1 << IAVF_RX_FLEX_DESC_STATUS0_HBO_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_IPE_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_L4E_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_EIPE_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_EUDPE_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_RXE_S))

/* Translate the rx descriptor status and error fields to pkt flags */
static inline uint64_t
iavf_rxd_to_pkt_flags(uint64_t qword)
{
	uint64_t flags;
	uint64_t error_bits = (qword >> IAVF_RXD_QW1_ERROR_SHIFT);

#define IAVF_RX_ERR_BITS 0x3f

	/* Check if RSS_HASH */
	flags = (((qword >> IAVF_RX_DESC_STATUS_FLTSTAT_SHIFT) &
					IAVF_RX_DESC_FLTSTAT_RSS_HASH) ==
			IAVF_RX_DESC_FLTSTAT_RSS_HASH) ? RTE_MBUF_F_RX_RSS_HASH : 0;

	/* Check if FDIR Match */
	flags |= (qword & (1 << IAVF_RX_DESC_STATUS_FLM_SHIFT) ?
				RTE_MBUF_F_RX_FDIR : 0);

	if (likely((error_bits & IAVF_RX_ERR_BITS) == 0)) {
		flags |= (RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD);
		return flags;
	}

	if (unlikely(error_bits & (1 << IAVF_RX_DESC_ERROR_IPE_SHIFT)))
		flags |= RTE_MBUF_F_RX_IP_CKSUM_BAD;
	else
		flags |= RTE_MBUF_F_RX_IP_CKSUM_GOOD;

	if (unlikely(error_bits & (1 << IAVF_RX_DESC_ERROR_L4E_SHIFT)))
		flags |= RTE_MBUF_F_RX_L4_CKSUM_BAD;
	else
		flags |= RTE_MBUF_F_RX_L4_CKSUM_GOOD;

	/* TODO: Oversize error bit is not processed here */

	return flags;
}

#define IAVF_RX_FLEX_ERR0_BITS	\
	((1 << IAVF_RX_FLEX_DESC_STATUS0_HBO_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_IPE_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_L4E_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_EIPE_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_EUDPE_S) |	\
	 (1 << IAVF_RX_FLEX_DESC_STATUS0_RXE_S))

/* Rx L3/L4 checksum */
static inline uint64_t
iavf_flex_rxd_error_to_pkt_flags(uint16_t stat_err0)
{
	uint64_t flags = 0;

	/* check if HW has decoded the packet and checksum */
	if (unlikely(!(stat_err0 & (1 << IAVF_RX_FLEX_DESC_STATUS0_L3L4P_S))))
		return 0;

	if (likely(!(stat_err0 & IAVF_RX_FLEX_ERR0_BITS))) {
		flags |= (RTE_MBUF_F_RX_IP_CKSUM_GOOD |
			RTE_MBUF_F_RX_L4_CKSUM_GOOD |
			RTE_MBUF_F_RX_OUTER_L4_CKSUM_GOOD);
		return flags;
	}

	if (unlikely(stat_err0 & (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_IPE_S)))
		flags |= RTE_MBUF_F_RX_IP_CKSUM_BAD;
	else
		flags |= RTE_MBUF_F_RX_IP_CKSUM_GOOD;

	if (unlikely(stat_err0 & (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_L4E_S)))
		flags |= RTE_MBUF_F_RX_L4_CKSUM_BAD;
	else
		flags |= RTE_MBUF_F_RX_L4_CKSUM_GOOD;

	if (unlikely(stat_err0 & (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_EIPE_S)))
		flags |= RTE_MBUF_F_RX_OUTER_IP_CKSUM_BAD;

	if (unlikely(stat_err0 & (1 << IAVF_RX_FLEX_DESC_STATUS0_XSUM_EUDPE_S)))
		flags |= RTE_MBUF_F_RX_OUTER_L4_CKSUM_BAD;
	else
		flags |= RTE_MBUF_F_RX_OUTER_L4_CKSUM_GOOD;

	return flags;
}

/* If the number of free RX descriptors is greater than the RX free
 * threshold of the queue, advance the Receive Descriptor Tail (RDT)
 * register. Update the RDT with the value of the last processed RX
 * descriptor minus 1, to guarantee that the RDT register is never
 * equal to the RDH register, which creates a "full" ring situation
 * from the hardware point of view.
 */
static inline void
iavf_update_rx_tail(struct ci_rx_queue *rxq, uint16_t nb_hold, uint16_t rx_id)
{
	nb_hold = (uint16_t)(nb_hold + rxq->nb_rx_hold);

	if (nb_hold > rxq->rx_free_thresh) {
		PMD_RX_LOG(DEBUG,
			   "port_id=%u queue_id=%u rx_tail=%u nb_hold=%u",
			   rxq->port_id, rxq->queue_id, rx_id, nb_hold);
		rx_id = (uint16_t)((rx_id == 0) ?
			(rxq->nb_rx_desc - 1) : (rx_id - 1));
		IAVF_PCI_REG_WC_WRITE(rxq->qrx_tail, rx_id);
		nb_hold = 0;
	}
	rxq->nb_rx_hold = nb_hold;
}

/* implement recv_pkts */
uint16_t
iavf_recv_pkts(void *rx_queue, rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	volatile union ci_rx_desc *rx_ring;
	volatile union ci_rx_desc *rxdp;
	struct ci_rx_queue *rxq;
	union ci_rx_desc rxd;
	struct ci_rx_entry rxe;
	rte_mbuf *rxm;
	rte_mbuf *nmb;
	uint16_t nb_rx;
	uint32_t rx_status;
	uint64_t qword1;
	uint16_t rx_packet_len;
	uint16_t rx_id, nb_hold;
	uint64_t dma_addr;
	uint64_t pkt_flags;
	const uint32_t *ptype_tbl;

	nb_rx = 0;
	nb_hold = 0;
	rxq = (struct ci_rx_queue *)rx_queue;
	rx_id = rxq->rx_tail;
	rx_ring = rxq->rx_ring;
	ptype_tbl = rxq->iavf_vsi->adapter->ptype_tbl;

	while (nb_rx < nb_pkts) {
		rxdp = &rx_ring[rx_id];
		qword1 = rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len);
		rx_status = (qword1 & IAVF_RXD_QW1_STATUS_MASK) >>
			    IAVF_RXD_QW1_STATUS_SHIFT;

		/* Check the DD bit first */
		if (!(rx_status & (1 << IAVF_RX_DESC_STATUS_DD_SHIFT)))
			break;
		IAVF_DUMP_RX_DESC(rxq, rxdp, rx_id);

		nmb = rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(!nmb)) {
			PMD_RX_LOG(DEBUG, "RX mbuf alloc failed port_id=%u "
				   "queue_id=%u", rxq->port_id, rxq->queue_id);
			break;
		}

		memcpy(&rxd, (const void *)(uintptr_t)(rxdp), sizeof(rxd));
		nb_hold++;
		rxe = rxq->sw_ring[rx_id];
		rxq->sw_ring[rx_id].mbuf = nmb;
		rx_id++;
		if (unlikely(rx_id == rxq->nb_rx_desc))
			rx_id = 0;

		/* Prefetch next mbuf */
		rte_prefetch0(rxq->sw_ring[rx_id].mbuf);

		/* When next RX descriptor is on a cache line boundary,
		 * prefetch the next 4 RX descriptors and next 8 pointers
		 * to mbufs.
		 */
		if ((rx_id & 0x3) == 0) {
			rte_prefetch0(&rx_ring[rx_id]);
			rte_prefetch0(rxq->sw_ring[rx_id].mbuf);
		}
		rxm = rxe.mbuf;
		dma_addr =
			rte_cpu_to_le_64(rte_mbuf_data_iova_default(nmb));
		rxdp->read.hdr_addr = 0;
		rxdp->read.pkt_addr = dma_addr;

		rx_packet_len = ((qword1 & IAVF_RXD_QW1_LENGTH_PBUF_MASK) >>
				IAVF_RXD_QW1_LENGTH_PBUF_SHIFT) - rxq->crc_len;

		rxm->data_offset = RTE_PKTMBUF_HEADROOM;
		rte_prefetch0(RTE_PTR_ADD(rxm->buf_addr, RTE_PKTMBUF_HEADROOM));
		rxm->nb_segs = 1;
		rxm->next = NULL;
		rxm->pkt_len = rx_packet_len;
		rxm->data_len = rx_packet_len;
		rxm->ol_flags = 0;
		pkt_flags = iavf_rxd_to_pkt_flags(qword1);
		rxm->packet_type =
			ptype_tbl[(uint8_t)((qword1 &
			IAVF_RXD_QW1_PTYPE_MASK) >> IAVF_RXD_QW1_PTYPE_SHIFT)];

		if (pkt_flags & RTE_MBUF_F_RX_RSS_HASH)
			rxm->hash.rss =
				rte_le_to_cpu_32(rxd.wb.qword0.hi_dword.rss);


		rxm->ol_flags |= pkt_flags;

		rx_pkts[nb_rx++] = rxm;
	}
	rxq->rx_tail = rx_id;

	iavf_update_rx_tail(rxq, nb_hold, rx_id);

	return nb_rx;
}

/* implement recv_pkts for flexible Rx descriptor */
uint16_t
iavf_recv_pkts_flex_rxd(void *rx_queue,
			rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	volatile union ci_rx_flex_desc *rx_ring;
	volatile union ci_rx_flex_desc *rxdp;
	struct ci_rx_queue *rxq;
	union ci_rx_flex_desc rxd;
	struct ci_rx_entry rxe;
	rte_mbuf *rxm;
	rte_mbuf *nmb;
	uint16_t nb_rx;
	uint16_t rx_stat_err0;
	uint16_t rx_packet_len;
	uint16_t rx_id, nb_hold;
	uint64_t dma_addr;
	uint64_t pkt_flags;
	const uint32_t *ptype_tbl;

	nb_rx = 0;
	nb_hold = 0;
	rxq = (struct ci_rx_queue *)rx_queue;
	rx_id = rxq->rx_tail;
	rx_ring = rxq->rx_flex_ring;
	ptype_tbl = rxq->iavf_vsi->adapter->ptype_tbl;

	while (nb_rx < nb_pkts) {
		rxdp = &rx_ring[rx_id];
		rx_stat_err0 = rte_le_to_cpu_16(rxdp->wb.status_error0);

		/* Check the DD bit first */
		if (!(rx_stat_err0 & (1 << IAVF_RX_FLEX_DESC_STATUS0_DD_S)))
			break;
		IAVF_DUMP_RX_DESC(rxq, rxdp, rx_id);

		nmb = rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(!nmb)) {
			PMD_RX_LOG(DEBUG, "RX mbuf alloc failed port_id=%u "
				   "queue_id=%u", rxq->port_id, rxq->queue_id);
			break;
		}

		memcpy(&rxd, (const void *)(uintptr_t)(rxdp), sizeof(rxd));
		nb_hold++;
		rxe = rxq->sw_ring[rx_id];
		rxq->sw_ring[rx_id].mbuf = nmb;
		rx_id++;
		if (unlikely(rx_id == rxq->nb_rx_desc))
			rx_id = 0;

		/* Prefetch next mbuf */
		rte_prefetch0(rxq->sw_ring[rx_id].mbuf);

		/* When next RX descriptor is on a cache line boundary,
		 * prefetch the next 4 RX descriptors and next 8 pointers
		 * to mbufs.
		 */
		if ((rx_id & 0x3) == 0) {
			rte_prefetch0(&rx_ring[rx_id]);
			rte_prefetch0(rxq->sw_ring[rx_id].mbuf);
		}
		rxm = rxe.mbuf;
		dma_addr =
			rte_cpu_to_le_64(rte_mbuf_data_iova_default(nmb));
		rxdp->read.hdr_addr = 0;
		rxdp->read.pkt_addr = dma_addr;

		rx_packet_len = (rte_le_to_cpu_16(rxd.wb.pkt_len) &
				IAVF_RX_FLX_DESC_PKT_LEN_M) - rxq->crc_len;

		rxm->data_offset = RTE_PKTMBUF_HEADROOM;
		rte_prefetch0(RTE_PTR_ADD(rxm->buf_addr, RTE_PKTMBUF_HEADROOM));
		rxm->nb_segs = 1;
		rxm->next = NULL;
		rxm->pkt_len = rx_packet_len;
		rxm->data_len = rx_packet_len;
		rxm->ol_flags = 0;
		rxm->packet_type = ptype_tbl[IAVF_RX_FLEX_DESC_PTYPE_M &
			rte_le_to_cpu_16(rxd.wb.ptype_flex_flags0)];
		iavf_rxd_to_pkt_fields(rxq, rxm, &rxd);
		pkt_flags = iavf_flex_rxd_error_to_pkt_flags(rx_stat_err0);


		rxm->ol_flags |= pkt_flags;

		rx_pkts[nb_rx++] = rxm;
	}
	rxq->rx_tail = rx_id;

	iavf_update_rx_tail(rxq, nb_hold, rx_id);

	return nb_rx;
}

/* implement recv_scattered_pkts for flexible Rx descriptor */
uint16_t
iavf_recv_scattered_pkts_flex_rxd(void *rx_queue, rte_mbuf **rx_pkts,
				  uint16_t nb_pkts)
{
	struct ci_rx_queue *rxq = (struct ci_rx_queue *)rx_queue;
	union ci_rx_flex_desc rxd;
	struct ci_rx_entry rxe;
	rte_mbuf *first_seg = rxq->pkt_first_seg;
	rte_mbuf *last_seg = rxq->pkt_last_seg;
	rte_mbuf *nmb, *rxm;
	uint16_t rx_id = rxq->rx_tail;
	uint16_t nb_rx = 0, nb_hold = 0, rx_packet_len;
	uint16_t rx_stat_err0;
	uint64_t dma_addr;
	uint64_t pkt_flags;

	volatile union ci_rx_flex_desc *rx_ring = rxq->rx_flex_ring;
	volatile union ci_rx_flex_desc *rxdp;
	const uint32_t *ptype_tbl = rxq->iavf_vsi->adapter->ptype_tbl;

	while (nb_rx < nb_pkts) {
		rxdp = &rx_ring[rx_id];
		rx_stat_err0 = rte_le_to_cpu_16(rxdp->wb.status_error0);

		/* Check the DD bit */
		if (!(rx_stat_err0 & (1 << IAVF_RX_FLEX_DESC_STATUS0_DD_S)))
			break;
		IAVF_DUMP_RX_DESC(rxq, rxdp, rx_id);

		nmb = rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(!nmb)) {
			PMD_RX_LOG(DEBUG, "RX mbuf alloc failed port_id=%u "
				   "queue_id=%u", rxq->port_id, rxq->queue_id);
			break;
		}

		memcpy(&rxd, (const void *)(uintptr_t)(rxdp), sizeof(rxd));
		nb_hold++;
		rxe = rxq->sw_ring[rx_id];
		rxq->sw_ring[rx_id].mbuf = nmb;
		rx_id++;
		if (rx_id == rxq->nb_rx_desc)
			rx_id = 0;

		/* Prefetch next mbuf */
		rte_prefetch0(rxq->sw_ring[rx_id].mbuf);

		/* When next RX descriptor is on a cache line boundary,
		 * prefetch the next 4 RX descriptors and next 8 pointers
		 * to mbufs.
		 */
		if ((rx_id & 0x3) == 0) {
			rte_prefetch0(&rx_ring[rx_id]);
			rte_prefetch0(rxq->sw_ring[rx_id].mbuf);
		}

		rxm = rxe.mbuf;
		dma_addr =
			rte_cpu_to_le_64(rte_mbuf_data_iova_default(nmb));

		/* Set data buffer address and data length of the mbuf */
		rxdp->read.hdr_addr = 0;
		rxdp->read.pkt_addr = dma_addr;
		rx_packet_len = rte_le_to_cpu_16(rxd.wb.pkt_len) &
				IAVF_RX_FLX_DESC_PKT_LEN_M;
		rxm->data_len = rx_packet_len;
		rxm->data_offset = RTE_PKTMBUF_HEADROOM;

		/* If this is the first buffer of the received packet, set the
		 * pointer to the first mbuf of the packet and initialize its
		 * context. Otherwise, update the total length and the number
		 * of segments of the current scattered packet, and update the
		 * pointer to the last mbuf of the current packet.
		 */
		if (!first_seg) {
			first_seg = rxm;
			first_seg->nb_segs = 1;
			first_seg->pkt_len = rx_packet_len;
		} else {
			first_seg->pkt_len =
				(uint16_t)(first_seg->pkt_len +
						rx_packet_len);
			first_seg->nb_segs++;
			last_seg->next = rxm;
		}

		/* If this is not the last buffer of the received packet,
		 * update the pointer to the last mbuf of the current scattered
		 * packet and continue to parse the RX ring.
		 */
		if (!(rx_stat_err0 & (1 << IAVF_RX_FLEX_DESC_STATUS0_EOF_S))) {
			last_seg = rxm;
			continue;
		}

		/* This is the last buffer of the received packet. If the CRC
		 * is not stripped by the hardware:
		 *  - Subtract the CRC length from the total packet length.
		 *  - If the last buffer only contains the whole CRC or a part
		 *  of it, free the mbuf associated to the last buffer. If part
		 *  of the CRC is also contained in the previous mbuf, subtract
		 *  the length of that CRC part from the data length of the
		 *  previous mbuf.
		 */
		rxm->next = NULL;
		if (unlikely(rxq->crc_len > 0)) {
			first_seg->pkt_len -= RTE_ETHER_CRC_LEN;
			if (rx_packet_len <= RTE_ETHER_CRC_LEN) {
				rte_pktmbuf_free_seg(rxm);
				first_seg->nb_segs--;
				last_seg->data_len =
					(uint16_t)(last_seg->data_len -
					(RTE_ETHER_CRC_LEN - rx_packet_len));
				last_seg->next = NULL;
			} else {
				rxm->data_len = (uint16_t)(rx_packet_len -
							RTE_ETHER_CRC_LEN);
			}
		} else if (unlikely(rx_packet_len == 0)) {
			/*
			 * NIC split CRC bytes into a trailing segment which is
			 * now empty after hardware CRC stripping. Free it.
			 */
			rte_pktmbuf_free_seg(rxm);
			first_seg->nb_segs--;
			last_seg->next = NULL;
		}

		first_seg->ol_flags = 0;
		first_seg->packet_type = ptype_tbl[IAVF_RX_FLEX_DESC_PTYPE_M &
			rte_le_to_cpu_16(rxd.wb.ptype_flex_flags0)];
		iavf_rxd_to_pkt_fields(rxq, first_seg, &rxd);
		pkt_flags = iavf_flex_rxd_error_to_pkt_flags(rx_stat_err0);


		first_seg->ol_flags |= pkt_flags;

		/* Prefetch data of first segment, if configured to do so. */
		rte_prefetch0(RTE_PTR_ADD(first_seg->buf_addr,
					  first_seg->data_offset));
		rx_pkts[nb_rx++] = first_seg;
		first_seg = NULL;
	}

	/* Record index of the next RX descriptor to probe. */
	rxq->rx_tail = rx_id;
	rxq->pkt_first_seg = first_seg;
	rxq->pkt_last_seg = last_seg;

	iavf_update_rx_tail(rxq, nb_hold, rx_id);

	return nb_rx;
}

/* implement recv_scattered_pkts  */
uint16_t
iavf_recv_scattered_pkts(void *rx_queue, rte_mbuf **rx_pkts,
			uint16_t nb_pkts)
{
	struct ci_rx_queue *rxq = (struct ci_rx_queue *)rx_queue;
	union ci_rx_desc rxd;
	struct ci_rx_entry rxe;
	rte_mbuf *first_seg = rxq->pkt_first_seg;
	rte_mbuf *last_seg = rxq->pkt_last_seg;
	rte_mbuf *nmb, *rxm;
	uint16_t rx_id = rxq->rx_tail;
	uint16_t nb_rx = 0, nb_hold = 0, rx_packet_len;
	uint32_t rx_status;
	uint64_t qword1;
	uint64_t dma_addr;
	uint64_t pkt_flags;

	volatile union ci_rx_desc *rx_ring = rxq->rx_ring;
	volatile union ci_rx_desc *rxdp;
	const uint32_t *ptype_tbl = rxq->iavf_vsi->adapter->ptype_tbl;

	while (nb_rx < nb_pkts) {
		rxdp = &rx_ring[rx_id];
		qword1 = rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len);
		rx_status = (qword1 & IAVF_RXD_QW1_STATUS_MASK) >>
			    IAVF_RXD_QW1_STATUS_SHIFT;

		/* Check the DD bit */
		if (!(rx_status & (1 << IAVF_RX_DESC_STATUS_DD_SHIFT)))
			break;
		IAVF_DUMP_RX_DESC(rxq, rxdp, rx_id);

		nmb = rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(!nmb)) {
			PMD_RX_LOG(DEBUG, "RX mbuf alloc failed port_id=%u "
				   "queue_id=%u", rxq->port_id, rxq->queue_id);
			break;
		}

		memcpy(&rxd, (const void *)(uintptr_t)(rxdp), sizeof(rxd));
		nb_hold++;
		rxe = rxq->sw_ring[rx_id];
		rxq->sw_ring[rx_id].mbuf = nmb;
		rx_id++;
		if (rx_id == rxq->nb_rx_desc)
			rx_id = 0;

		/* Prefetch next mbuf */
		rte_prefetch0(rxq->sw_ring[rx_id].mbuf);

		/* When next RX descriptor is on a cache line boundary,
		 * prefetch the next 4 RX descriptors and next 8 pointers
		 * to mbufs.
		 */
		if ((rx_id & 0x3) == 0) {
			rte_prefetch0(&rx_ring[rx_id]);
			rte_prefetch0(rxq->sw_ring[rx_id].mbuf);
		}

		rxm = rxe.mbuf;
		dma_addr =
			rte_cpu_to_le_64(rte_mbuf_data_iova_default(nmb));

		/* Set data buffer address and data length of the mbuf */
		rxdp->read.hdr_addr = 0;
		rxdp->read.pkt_addr = dma_addr;
		rx_packet_len = (qword1 & IAVF_RXD_QW1_LENGTH_PBUF_MASK) >>
				 IAVF_RXD_QW1_LENGTH_PBUF_SHIFT;
		rxm->data_len = rx_packet_len;
		rxm->data_offset = RTE_PKTMBUF_HEADROOM;

		/* If this is the first buffer of the received packet, set the
		 * pointer to the first mbuf of the packet and initialize its
		 * context. Otherwise, update the total length and the number
		 * of segments of the current scattered packet, and update the
		 * pointer to the last mbuf of the current packet.
		 */
		if (!first_seg) {
			first_seg = rxm;
			first_seg->nb_segs = 1;
			first_seg->pkt_len = rx_packet_len;
		} else {
			first_seg->pkt_len =
				(uint16_t)(first_seg->pkt_len +
						rx_packet_len);
			first_seg->nb_segs++;
			last_seg->next = rxm;
		}

		/* If this is not the last buffer of the received packet,
		 * update the pointer to the last mbuf of the current scattered
		 * packet and continue to parse the RX ring.
		 */
		if (!(rx_status & (1 << IAVF_RX_DESC_STATUS_EOF_SHIFT))) {
			last_seg = rxm;
			continue;
		}

		/* This is the last buffer of the received packet. If the CRC
		 * is not stripped by the hardware:
		 *  - Subtract the CRC length from the total packet length.
		 *  - If the last buffer only contains the whole CRC or a part
		 *  of it, free the mbuf associated to the last buffer. If part
		 *  of the CRC is also contained in the previous mbuf, subtract
		 *  the length of that CRC part from the data length of the
		 *  previous mbuf.
		 */
		rxm->next = NULL;
		if (unlikely(rxq->crc_len > 0)) {
			first_seg->pkt_len -= RTE_ETHER_CRC_LEN;
			if (rx_packet_len <= RTE_ETHER_CRC_LEN) {
				rte_pktmbuf_free_seg(rxm);
				first_seg->nb_segs--;
				last_seg->data_len =
					(uint16_t)(last_seg->data_len -
					(RTE_ETHER_CRC_LEN - rx_packet_len));
				last_seg->next = NULL;
			} else
				rxm->data_len = (uint16_t)(rx_packet_len -
							RTE_ETHER_CRC_LEN);
		} else if (unlikely(rx_packet_len == 0)) {
			/*
			 * NIC split CRC bytes into a trailing segment which is
			 * now empty after hardware CRC stripping. Free it.
			 */
			rte_pktmbuf_free_seg(rxm);
			first_seg->nb_segs--;
			last_seg->next = NULL;
		}

		first_seg->ol_flags = 0;
		pkt_flags = iavf_rxd_to_pkt_flags(qword1);
		first_seg->packet_type =
			ptype_tbl[(uint8_t)((qword1 &
			IAVF_RXD_QW1_PTYPE_MASK) >> IAVF_RXD_QW1_PTYPE_SHIFT)];

		if (pkt_flags & RTE_MBUF_F_RX_RSS_HASH)
			first_seg->hash.rss =
				rte_le_to_cpu_32(rxd.wb.qword0.hi_dword.rss);


		first_seg->ol_flags |= pkt_flags;

		/* Prefetch data of first segment, if configured to do so. */
		rte_prefetch0(RTE_PTR_ADD(first_seg->buf_addr,
					  first_seg->data_offset));
		rx_pkts[nb_rx++] = first_seg;
		first_seg = NULL;
	}

	/* Record index of the next RX descriptor to probe. */
	rxq->rx_tail = rx_id;
	rxq->pkt_first_seg = first_seg;
	rxq->pkt_last_seg = last_seg;

	iavf_update_rx_tail(rxq, nb_hold, rx_id);

	return nb_rx;
}

#define IAVF_LOOK_AHEAD 8
static inline int
iavf_rx_scan_hw_ring_flex_rxd(struct ci_rx_queue *rxq,
			    rte_mbuf **rx_pkts,
			    uint16_t nb_pkts)
{
	volatile union ci_rx_flex_desc *rxdp;
	struct ci_rx_entry *rxep;
	rte_mbuf *mb;
	uint16_t stat_err0;
	uint16_t pkt_len;
	int32_t s[IAVF_LOOK_AHEAD], var, nb_dd;
	int32_t i, j, nb_rx = 0;
	int32_t nb_staged = 0;
	uint64_t pkt_flags;
	const uint32_t *ptype_tbl = rxq->iavf_vsi->adapter->ptype_tbl;

	rxdp = &rxq->rx_flex_ring[rxq->rx_tail];
	rxep = &rxq->sw_ring[rxq->rx_tail];

	stat_err0 = rte_le_to_cpu_16(rxdp->wb.status_error0);

	/* Make sure there is at least 1 packet to receive */
	if (!(stat_err0 & (1 << IAVF_RX_FLEX_DESC_STATUS0_DD_S)))
		return 0;

	/* Scan LOOK_AHEAD descriptors at a time to determine which
	 * descriptors reference packets that are ready to be received.
	 */
	for (i = 0; i < IAVF_RX_MAX_BURST; i += IAVF_LOOK_AHEAD,
	     rxdp += IAVF_LOOK_AHEAD, rxep += IAVF_LOOK_AHEAD) {
		/* Read desc statuses backwards to avoid race condition */
		for (j = IAVF_LOOK_AHEAD - 1; j >= 0; j--)
			s[j] = rte_le_to_cpu_16(rxdp[j].wb.status_error0);

		/* This barrier is to order loads of different words in the descriptor */
		rte_atomic_thread_fence(rte_memory_order_acquire);

		/* Compute how many contiguous DD bits were set */
		for (j = 0, nb_dd = 0; j < IAVF_LOOK_AHEAD; j++) {
			var = s[j] & (1 << IAVF_RX_FLEX_DESC_STATUS0_DD_S);
#ifdef RTE_ARCH_ARM
			/* For Arm platforms, count only contiguous descriptors
			 * whose DD bit is set to 1. On Arm platforms, reads of
			 * descriptors can be reordered. Since the CPU may
			 * be reading the descriptors as the NIC updates them
			 * in memory, it is possbile that the DD bit for a
			 * descriptor earlier in the queue is read as not set
			 * while the DD bit for a descriptor later in the queue
			 * is read as set.
			 */
			if (var)
				nb_dd += 1;
			else
				break;
#else
			nb_dd += var;
#endif
		}

		/* Translate descriptor info to mbuf parameters */
		for (j = 0; j < nb_dd; j++) {
			IAVF_DUMP_RX_DESC(rxq, &rxdp[j],
					  rxq->rx_tail +
					  i * IAVF_LOOK_AHEAD + j);

			mb = rxep[j].mbuf;
			pkt_len = (rte_le_to_cpu_16(rxdp[j].wb.pkt_len) &
				IAVF_RX_FLX_DESC_PKT_LEN_M) - rxq->crc_len;
			mb->data_len = pkt_len;
			mb->pkt_len = pkt_len;
			mb->ol_flags = 0;

			mb->packet_type = ptype_tbl[IAVF_RX_FLEX_DESC_PTYPE_M &
				rte_le_to_cpu_16(rxdp[j].wb.ptype_flex_flags0)];
			iavf_rxd_to_pkt_fields(rxq, mb, &rxdp[j]);
			stat_err0 = rte_le_to_cpu_16(rxdp[j].wb.status_error0);
			pkt_flags = iavf_flex_rxd_error_to_pkt_flags(stat_err0);


			mb->ol_flags |= pkt_flags;

			/* Put up to nb_pkts directly into buffers */
			if ((i + j) < nb_pkts) {
				rx_pkts[i + j] = rxep[j].mbuf;
				nb_rx++;
			} else {
				/* Stage excess pkts received */
				rxq->rx_stage[nb_staged] = rxep[j].mbuf;
				nb_staged++;
			}
		}

		if (nb_dd != IAVF_LOOK_AHEAD)
			break;
	}

	/* Update rxq->rx_nb_avail to reflect number of staged pkts */
	rxq->rx_nb_avail = nb_staged;

	/* Clear software ring entries */
	for (i = 0; i < (nb_rx + nb_staged); i++)
		rxq->sw_ring[rxq->rx_tail + i].mbuf = NULL;

	return nb_rx;
}

static inline int
iavf_rx_scan_hw_ring(struct ci_rx_queue *rxq, rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	volatile union ci_rx_desc *rxdp;
	struct ci_rx_entry *rxep;
	rte_mbuf *mb;
	uint16_t pkt_len;
	uint64_t qword1;
	uint32_t rx_status;
	int32_t s[IAVF_LOOK_AHEAD], var, nb_dd;
	int32_t i, j, nb_rx = 0;
	int32_t nb_staged = 0;
	uint64_t pkt_flags;
	const uint32_t *ptype_tbl = rxq->iavf_vsi->adapter->ptype_tbl;

	rxdp = &rxq->rx_ring[rxq->rx_tail];
	rxep = &rxq->sw_ring[rxq->rx_tail];

	qword1 = rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len);
	rx_status = (qword1 & IAVF_RXD_QW1_STATUS_MASK) >>
		    IAVF_RXD_QW1_STATUS_SHIFT;

	/* Make sure there is at least 1 packet to receive */
	if (!(rx_status & (1 << IAVF_RX_DESC_STATUS_DD_SHIFT)))
		return 0;

	/* Scan LOOK_AHEAD descriptors at a time to determine which
	 * descriptors reference packets that are ready to be received.
	 */
	for (i = 0; i < IAVF_RX_MAX_BURST; i += IAVF_LOOK_AHEAD,
	     rxdp += IAVF_LOOK_AHEAD, rxep += IAVF_LOOK_AHEAD) {
		/* Read desc statuses backwards to avoid race condition */
		for (j = IAVF_LOOK_AHEAD - 1; j >= 0; j--) {
			qword1 = rte_le_to_cpu_64(
				rxdp[j].wb.qword1.status_error_len);
			s[j] = (qword1 & IAVF_RXD_QW1_STATUS_MASK) >>
			       IAVF_RXD_QW1_STATUS_SHIFT;
		}

		/* This barrier is to order loads of different words in the descriptor */
		rte_atomic_thread_fence(rte_memory_order_acquire);

		/* Compute how many contiguous DD bits were set */
		for (j = 0, nb_dd = 0; j < IAVF_LOOK_AHEAD; j++) {
			var = s[j] & (1 << IAVF_RX_DESC_STATUS_DD_SHIFT);
#ifdef RTE_ARCH_ARM
			/* For Arm platforms, count only contiguous descriptors
			 * whose DD bit is set to 1. On Arm platforms, reads of
			 * descriptors can be reordered. Since the CPU may
			 * be reading the descriptors as the NIC updates them
			 * in memory, it is possbile that the DD bit for a
			 * descriptor earlier in the queue is read as not set
			 * while the DD bit for a descriptor later in the queue
			 * is read as set.
			 */
			if (var)
				nb_dd += 1;
			else
				break;
#else
			nb_dd += var;
#endif
		}

		/* Translate descriptor info to mbuf parameters */
		for (j = 0; j < nb_dd; j++) {
			IAVF_DUMP_RX_DESC(rxq, &rxdp[j],
					 rxq->rx_tail + i * IAVF_LOOK_AHEAD + j);

			mb = rxep[j].mbuf;
			qword1 = rte_le_to_cpu_64
					(rxdp[j].wb.qword1.status_error_len);
			pkt_len = ((qword1 & IAVF_RXD_QW1_LENGTH_PBUF_MASK) >>
				  IAVF_RXD_QW1_LENGTH_PBUF_SHIFT) - rxq->crc_len;
			mb->data_len = pkt_len;
			mb->pkt_len = pkt_len;
			mb->ol_flags = 0;
			pkt_flags = iavf_rxd_to_pkt_flags(qword1);
			mb->packet_type =
				ptype_tbl[(uint8_t)((qword1 &
				IAVF_RXD_QW1_PTYPE_MASK) >>
				IAVF_RXD_QW1_PTYPE_SHIFT)];

			if (pkt_flags & RTE_MBUF_F_RX_RSS_HASH)
				mb->hash.rss = rte_le_to_cpu_32(
					rxdp[j].wb.qword0.hi_dword.rss);


			mb->ol_flags |= pkt_flags;

			/* Put up to nb_pkts directly into buffers */
			if ((i + j) < nb_pkts) {
				rx_pkts[i + j] = rxep[j].mbuf;
				nb_rx++;
			} else { /* Stage excess pkts received */
				rxq->rx_stage[nb_staged] = rxep[j].mbuf;
				nb_staged++;
			}
		}

		if (nb_dd != IAVF_LOOK_AHEAD)
			break;
	}

	/* Update rxq->rx_nb_avail to reflect number of staged pkts */
	rxq->rx_nb_avail = nb_staged;

	/* Clear software ring entries */
	for (i = 0; i < (nb_rx + nb_staged); i++)
		rxq->sw_ring[rxq->rx_tail + i].mbuf = NULL;

	return nb_rx;
}

static inline uint16_t
iavf_rx_fill_from_stage(struct ci_rx_queue *rxq,
		       rte_mbuf **rx_pkts,
		       uint16_t nb_pkts)
{
	uint16_t i;
	rte_mbuf **stage = &rxq->rx_stage[rxq->rx_next_avail];

	nb_pkts = (uint16_t)RTE_MIN(nb_pkts, rxq->rx_nb_avail);

	for (i = 0; i < nb_pkts; i++)
		rx_pkts[i] = stage[i];

	rxq->rx_nb_avail = (uint16_t)(rxq->rx_nb_avail - nb_pkts);
	rxq->rx_next_avail = (uint16_t)(rxq->rx_next_avail + nb_pkts);

	return nb_pkts;
}

static inline int
iavf_rx_alloc_bufs(struct ci_rx_queue *rxq)
{
	volatile union ci_rx_desc *rxdp;
	struct ci_rx_entry *rxep;
	rte_mbuf *mb;
	uint16_t alloc_idx, i;
	uint64_t dma_addr;
	int diag;

	/* Allocate buffers in bulk */
	alloc_idx = (uint16_t)(rxq->rx_free_trigger -
				(rxq->rx_free_thresh - 1));
	rxep = &rxq->sw_ring[alloc_idx];
	diag = rte_mbuf_raw_alloc_bulk(rxq->mp, (rte_mbuf **)rxep,
				    rxq->rx_free_thresh);
	if (unlikely(diag != 0)) {
		PMD_RX_LOG(ERR, "Failed to get mbufs in bulk");
		return -ENOMEM;
	}

	rxdp = &rxq->rx_ring[alloc_idx];
	for (i = 0; i < rxq->rx_free_thresh; i++) {
		if (likely(i < (rxq->rx_free_thresh - 1)))
			/* Prefetch next mbuf */
			rte_prefetch0(rxep[i + 1].mbuf);

		mb = rxep[i].mbuf;
		mb->next = NULL;
		mb->data_offset = RTE_PKTMBUF_HEADROOM;
		mb->nb_segs = 1;
		dma_addr = rte_cpu_to_le_64(rte_mbuf_data_iova_default(mb));
		rxdp[i].read.hdr_addr = 0;
		rxdp[i].read.pkt_addr = dma_addr;
	}

	/* Update rx tail register */
	rte_wmb();
	IAVF_PCI_REG_WC_WRITE_RELAXED(rxq->qrx_tail, rxq->rx_free_trigger);

	rxq->rx_free_trigger =
		(uint16_t)(rxq->rx_free_trigger + rxq->rx_free_thresh);
	if (rxq->rx_free_trigger >= rxq->nb_rx_desc)
		rxq->rx_free_trigger = (uint16_t)(rxq->rx_free_thresh - 1);

	return 0;
}

static inline uint16_t
rx_recv_pkts(void *rx_queue, rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	struct ci_rx_queue *rxq = (struct ci_rx_queue *)rx_queue;
	uint16_t nb_rx = 0;

	if (!nb_pkts)
		return 0;

	if (rxq->rx_nb_avail)
		return iavf_rx_fill_from_stage(rxq, rx_pkts, nb_pkts);

	if (rxq->rxdid >= IAVF_RXDID_FLEX_NIC && rxq->rxdid <= IAVF_RXDID_LAST)
		nb_rx = (uint16_t)iavf_rx_scan_hw_ring_flex_rxd(rxq, rx_pkts, nb_pkts);
	else
		nb_rx = (uint16_t)iavf_rx_scan_hw_ring(rxq, rx_pkts, nb_pkts);

	rxq->rx_next_avail = 0;
	rxq->rx_tail = (uint16_t)(rxq->rx_tail + nb_rx + rxq->rx_nb_avail);

	if (rxq->rx_tail > rxq->rx_free_trigger) {
		if (iavf_rx_alloc_bufs(rxq) != 0) {
			uint16_t i, j, nb_staged;

			/* TODO: count rx_mbuf_alloc_failed here */

			nb_staged = rxq->rx_nb_avail;
			rxq->rx_nb_avail = 0;

			rxq->rx_tail = (uint16_t)(rxq->rx_tail - (nb_rx + nb_staged));
			for (i = 0, j = rxq->rx_tail; i < nb_rx; i++, j++) {
				rxq->sw_ring[j].mbuf = rx_pkts[i];
				rx_pkts[i] = NULL;
			}
			for (i = 0, j = rxq->rx_tail + nb_rx; i < nb_staged; i++, j++) {
				rxq->sw_ring[j].mbuf = rxq->rx_stage[i];
				rx_pkts[i] = NULL;
			}

			return 0;
		}
	}

	if (rxq->rx_tail >= rxq->nb_rx_desc)
		rxq->rx_tail = 0;

	PMD_RX_LOG(DEBUG, "port_id=%u queue_id=%u rx_tail=%u, nb_rx=%u",
		   rxq->port_id, rxq->queue_id,
		   rxq->rx_tail, nb_rx);

	return nb_rx;
}

static uint16_t
iavf_recv_pkts_bulk_alloc(void *rx_queue,
			 rte_mbuf **rx_pkts,
			 uint16_t nb_pkts)
{
	uint16_t nb_rx = 0, n, count;

	if (unlikely(nb_pkts == 0))
		return 0;

	if (likely(nb_pkts <= IAVF_RX_MAX_BURST))
		return rx_recv_pkts(rx_queue, rx_pkts, nb_pkts);

	while (nb_pkts) {
		n = RTE_MIN(nb_pkts, IAVF_RX_MAX_BURST);
		count = rx_recv_pkts(rx_queue, &rx_pkts[nb_rx], n);
		nb_rx = (uint16_t)(nb_rx + count);
		nb_pkts = (uint16_t)(nb_pkts - count);
		if (count < n)
			break;
	}

	return nb_rx;
}

/* Check if the context descriptor is needed for TX offloading */
/*
 * A context descriptor carries TSO, tunnel, LLDP or second-VLAN-tag parameters. minidpdk mbufs have none of them,
 * so a packet never needs one; checksum offloads live in the data descriptor.
 */
static uint16_t
iavf_get_context_desc(uint64_t ol_flags, const rte_mbuf *mbuf,
		      const union ci_tx_offload *tx_offload,
		      const struct ci_tx_queue *txq,
		      uint64_t *qw0, uint64_t *qw1)
{
	(void)ol_flags;
	(void)mbuf;
	(void)tx_offload;
	(void)txq;
	(void)qw0;
	(void)qw1;
	return 0;
}

/*
 * What DPDK's iavf_prep_pkts does through rte_net_intel_cksum_prepare: the NIC sums the L4 header and payload into
 * the checksum field it finds there, so that field has to start out as the pseudo-header sum. ENA computes the whole
 * thing itself, so minidpdk applications leave it zero.
 */
static inline void
iavf_tx_cksum_prepare(rte_mbuf *m)
{
	const uint64_t l4 = m->ol_flags & RTE_MBUF_F_TX_L4_MASK;

	if (l4 != RTE_MBUF_F_TX_TCP_CKSUM && l4 != RTE_MBUF_F_TX_UDP_CKSUM)
		return;
	if (m->data_len < m->l2_len + m->l3_len + (l4 == RTE_MBUF_F_TX_TCP_CKSUM ? 18 : 8))
		return;

	uint8_t *l3 = rte_pktmbuf_mtod_offset(m, uint8_t *, m->l2_len);
	uint8_t *l4h = l3 + m->l3_len;
	uint16_t *field = (uint16_t *)(l4h + (l4 == RTE_MBUF_F_TX_TCP_CKSUM ? 16 : 6));

	if (m->ol_flags & RTE_MBUF_F_TX_IPV4) {
		struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)l3;
		if (m->ol_flags & RTE_MBUF_F_TX_IP_CKSUM)
			ip->hdr_checksum = 0;
		*field = rte_ipv4_phdr_cksum(ip, m->ol_flags);
	} else if (m->ol_flags & RTE_MBUF_F_TX_IPV6) {
		/* src and dst (32 bytes from offset 8), L4 length (payload length), next header */
		uint32_t sum = _raw_cksum(l3 + 8, 32, 0);
		uint16_t plen;
		memcpy(&plen, l3 + 4, sizeof(plen));
		sum += plen;
		sum += rte_cpu_to_be_16((uint16_t)l3[6]);
		*field = _raw_cksum_reduce(sum);
	}
}

/* TX function */
uint16_t
iavf_xmit_pkts(void *tx_queue, rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	struct ci_tx_queue *txq = (struct ci_tx_queue *)tx_queue;

	for (uint16_t i = 0; i < nb_pkts; i++)
		iavf_tx_cksum_prepare(tx_pkts[i]);
	return ci_xmit_pkts(txq, tx_pkts, nb_pkts, iavf_get_context_desc);
}

/*
 * DPDK picks among its scalar and vector paths with ci_rx_path_select; with the scalar paths only, the choice is
 * scattered or not, bulk allocation or not, and the descriptor format every queue negotiated.
 */
void
iavf_set_rx_function(struct rte_eth_dev *dev)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	iavf_eth_dev *edev = iavf_edev(dev);
	bool use_flex = true;
	int i;

	for (i = 0; i < dev->data.nb_rx_queues; i++) {
		struct ci_rx_queue *rxq = (struct ci_rx_queue *)dev->data.rx_queues[i];
		if (rxq->rxdid <= IAVF_RXDID_LEGACY_1) {
			PMD_DRV_LOG(NOTICE, "request RXDID[%d] in Queue[%d] is legacy, "
				"set rx_pkt_burst as legacy for all queues", rxq->rxdid, i);
			use_flex = false;
		} else if (!(vf->supported_rxdid & RTE_BIT64(rxq->rxdid))) {
			PMD_DRV_LOG(NOTICE, "request RXDID[%d] in Queue[%d] is not supported, "
				"set rx_pkt_burst as legacy for all queues", rxq->rxdid, i);
			use_flex = false;
		}
	}

	if (adapter->scattered_rx) {
		adapter->rx_func_type = use_flex ? IAVF_RX_SCATTERED_FLEX_RXD : IAVF_RX_SCATTERED;
		edev->rx_fn = use_flex ? iavf_recv_scattered_pkts_flex_rxd : iavf_recv_scattered_pkts;
	} else if (adapter->rx_bulk_alloc_allowed) {
		adapter->rx_func_type = use_flex ? IAVF_RX_BULK_ALLOC_FLEX_RXD : IAVF_RX_BULK_ALLOC;
		edev->rx_fn = iavf_recv_pkts_bulk_alloc;
	} else {
		adapter->rx_func_type = use_flex ? IAVF_RX_FLEX_RXD : IAVF_RX_DEFAULT;
		edev->rx_fn = use_flex ? iavf_recv_pkts_flex_rxd : iavf_recv_pkts;
	}
	PMD_DRV_LOG(INFO, "Rx: %s%s%s", adapter->scattered_rx ? "scattered " : "",
		    !adapter->scattered_rx && adapter->rx_bulk_alloc_allowed ? "bulk-alloc " : "",
		    use_flex ? "flex descriptors" : "legacy descriptors");
}

void
iavf_set_tx_function(struct rte_eth_dev *dev)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);

	adapter->tx_func_type = IAVF_TX_DEFAULT;
	iavf_edev(dev)->tx_fn = iavf_xmit_pkts;
}

/* Get the number of used descriptors of a rx queue */
int
iavf_dev_rxq_count(void *rx_queue)
{
#define IAVF_RXQ_SCAN_INTERVAL 4
	volatile union ci_rx_desc *rxdp;
	struct ci_rx_queue *rxq;
	uint16_t desc = 0;

	rxq = (struct ci_rx_queue *)rx_queue;
	rxdp = &rxq->rx_ring[rxq->rx_tail];

	while ((desc < rxq->nb_rx_desc) &&
	       ((rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len) &
		 IAVF_RXD_QW1_STATUS_MASK) >> IAVF_RXD_QW1_STATUS_SHIFT) &
	       (1 << IAVF_RX_DESC_STATUS_DD_SHIFT)) {
		/* Check the DD bit of a rx descriptor of each 4 in a group,
		 * to avoid checking too frequently and downgrading performance
		 * too much.
		 */
		desc += IAVF_RXQ_SCAN_INTERVAL;
		rxdp += IAVF_RXQ_SCAN_INTERVAL;
		if (rxq->rx_tail + desc >= rxq->nb_rx_desc)
			rxdp = &(rxq->rx_ring[rxq->rx_tail +
					desc - rxq->nb_rx_desc]);
	}

	return desc;
}

static inline uint32_t
iavf_get_default_ptype(uint16_t ptype)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-designator"
	alignas(RTE_CACHE_LINE_SIZE) static const uint32_t ptype_tbl[IAVF_MAX_PKT_TYPE] = {
		/* L2 types */
		/* [0] reserved */
		[1] = RTE_PTYPE_L2_ETHER,
		[2] = RTE_PTYPE_L2_ETHER_TIMESYNC,
		/* [3] - [5] reserved */
		[6] = RTE_PTYPE_L2_ETHER_LLDP,
		/* [7] - [10] reserved */
		[11] = RTE_PTYPE_L2_ETHER_ARP,
		/* [12] - [21] reserved */

		/* Non tunneled IPv4 */
		[22] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_L4_FRAG,
		[23] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_L4_NONFRAG,
		[24] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_L4_UDP,
		/* [25] reserved */
		[26] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_L4_TCP,
		[27] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_L4_SCTP,
		[28] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_L4_ICMP,

		/* IPv4 --> IPv4 */
		[29] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_FRAG,
		[30] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_NONFRAG,
		[31] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_UDP,
		/* [32] reserved */
		[33] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_TCP,
		[34] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_SCTP,
		[35] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_ICMP,

		/* IPv4 --> IPv6 */
		[36] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_FRAG,
		[37] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_NONFRAG,
		[38] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_UDP,
		/* [39] reserved */
		[40] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_TCP,
		[41] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_SCTP,
		[42] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_ICMP,

		/* IPv4 --> GRE/Teredo/VXLAN */
		[43] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT,

		/* IPv4 --> GRE/Teredo/VXLAN --> IPv4 */
		[44] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_FRAG,
		[45] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_NONFRAG,
		[46] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_UDP,
		/* [47] reserved */
		[48] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_TCP,
		[49] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_SCTP,
		[50] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_ICMP,

		/* IPv4 --> GRE/Teredo/VXLAN --> IPv6 */
		[51] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_FRAG,
		[52] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_NONFRAG,
		[53] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_UDP,
		/* [54] reserved */
		[55] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_TCP,
		[56] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_SCTP,
		[57] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_ICMP,

		/* IPv4 --> GRE/Teredo/VXLAN --> MAC */
		[58] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER,

		/* IPv4 --> GRE/Teredo/VXLAN --> MAC --> IPv4 */
		[59] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_FRAG,
		[60] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_NONFRAG,
		[61] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_UDP,
		/* [62] reserved */
		[63] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_TCP,
		[64] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_SCTP,
		[65] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_ICMP,

		/* IPv4 --> GRE/Teredo/VXLAN --> MAC --> IPv6 */
		[66] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_FRAG,
		[67] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_NONFRAG,
		[68] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_UDP,
		/* [69] reserved */
		[70] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_TCP,
		[71] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_SCTP,
		[72] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
		       RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_ICMP,
		/* [73] - [87] reserved */

		/* Non tunneled IPv6 */
		[88] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_L4_FRAG,
		[89] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_L4_NONFRAG,
		[90] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_L4_UDP,
		/* [91] reserved */
		[92] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_L4_TCP,
		[93] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_L4_SCTP,
		[94] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_L4_ICMP,

		/* IPv6 --> IPv4 */
		[95] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_FRAG,
		[96] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_NONFRAG,
		[97] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_UDP,
		/* [98] reserved */
		[99] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
		       RTE_PTYPE_TUNNEL_IP |
		       RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
		       RTE_PTYPE_INNER_L4_TCP,
		[100] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_IP |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_SCTP,
		[101] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_IP |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,

		/* IPv6 --> IPv6 */
		[102] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_IP |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_FRAG,
		[103] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_IP |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_NONFRAG,
		[104] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_IP |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_UDP,
		/* [105] reserved */
		[106] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_IP |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_TCP,
		[107] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_IP |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_SCTP,
		[108] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_IP |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,

		/* IPv6 --> GRE/Teredo/VXLAN */
		[109] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT,

		/* IPv6 --> GRE/Teredo/VXLAN --> IPv4 */
		[110] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_FRAG,
		[111] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_NONFRAG,
		[112] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_UDP,
		/* [113] reserved */
		[114] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_TCP,
		[115] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_SCTP,
		[116] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,

		/* IPv6 --> GRE/Teredo/VXLAN --> IPv6 */
		[117] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_FRAG,
		[118] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_NONFRAG,
		[119] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_UDP,
		/* [120] reserved */
		[121] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_TCP,
		[122] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_SCTP,
		[123] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,

		/* IPv6 --> GRE/Teredo/VXLAN --> MAC */
		[124] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER,

		/* IPv6 --> GRE/Teredo/VXLAN --> MAC --> IPv4 */
		[125] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_FRAG,
		[126] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_NONFRAG,
		[127] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_UDP,
		/* [128] reserved */
		[129] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_TCP,
		[130] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_SCTP,
		[131] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,

		/* IPv6 --> GRE/Teredo/VXLAN --> MAC --> IPv6 */
		[132] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_FRAG,
		[133] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_NONFRAG,
		[134] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_UDP,
		/* [135] reserved */
		[136] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_TCP,
		[137] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_SCTP,
		[138] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GRENAT | RTE_PTYPE_INNER_L2_ETHER |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,
		/* [139] - [299] reserved */

		/* PPPoE */
		[300] = RTE_PTYPE_L2_ETHER_PPPOE,
		[301] = RTE_PTYPE_L2_ETHER_PPPOE,

		/* PPPoE --> IPv4 */
		[302] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_FRAG,
		[303] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_NONFRAG,
		[304] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[305] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_TCP,
		[306] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_SCTP,
		[307] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_ICMP,

		/* PPPoE --> IPv6 */
		[308] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_FRAG,
		[309] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_NONFRAG,
		[310] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[311] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_TCP,
		[312] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_SCTP,
		[313] = RTE_PTYPE_L2_ETHER_PPPOE |
			RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_ICMP,
		/* [314] - [324] reserved */

		/* IPv4/IPv6 --> GTPC/GTPU */
		[325] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPC,
		[326] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPC,
		[327] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPC,
		[328] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPC,
		[329] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU,
		[330] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU,

		/* IPv4 --> GTPU --> IPv4 */
		[331] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_FRAG,
		[332] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_NONFRAG,
		[333] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_UDP,
		[334] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_TCP,
		[335] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,

		/* IPv6 --> GTPU --> IPv4 */
		[336] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_FRAG,
		[337] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_NONFRAG,
		[338] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_UDP,
		[339] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_TCP,
		[340] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,

		/* IPv4 --> GTPU --> IPv6 */
		[341] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_FRAG,
		[342] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_NONFRAG,
		[343] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_UDP,
		[344] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_TCP,
		[345] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,

		/* IPv6 --> GTPU --> IPv6 */
		[346] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_FRAG,
		[347] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_NONFRAG,
		[348] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_UDP,
		[349] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_TCP,
		[350] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_TUNNEL_GTPU |
			RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_INNER_L4_ICMP,

		/* IPv4 --> UDP ECPRI */
		[372] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[373] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[374] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[375] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[376] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[377] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[378] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[379] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[380] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[381] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,

		/* IPV6 --> UDP ECPRI */
		[382] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[383] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[384] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[385] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[386] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[387] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[388] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[389] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[390] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		[391] = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV6_EXT_UNKNOWN |
			RTE_PTYPE_L4_UDP,
		/* All others reserved */
	};
#pragma clang diagnostic pop

	return ptype_tbl[ptype];
}

void __rte_cold
iavf_set_default_ptype_table(struct rte_eth_dev *dev)
{
	struct iavf_adapter *ad =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	int i;

	for (i = 0; i < IAVF_MAX_PKT_TYPE; i++)
		ad->ptype_tbl[i] = iavf_get_default_ptype(i);
}
