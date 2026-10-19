/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017 Intel Corporation
 */

// DPDK's drivers/net/intel/iavf/iavf_ethdev.c on minidpdk: configure, start/stop, RSS, stats, the misc vector and
// device init, plus the OSv glue (probe/attach, MSI-X, the management thread, interrupt-mode receive).

#include <osv/aligned_new.hh>
#include <osv/msi.hh>
#include <osv/sched.hh>

#include "iavf.h"
#include "iavf_ethdev.hh"
#include "iavf_rxtx.h"

#define RTE_ETHER_MIN_LEN 64

static void
iavf_config_rss_hf(struct iavf_adapter *adapter, uint64_t rss_hf)
{
	/* hena bit -> rss_hf type; C++ has no array designators. */
	uint64_t map_hena_rss[IAVF_FILTER_PCTYPE_L2_PAYLOAD + 1] = {};
	/* IPv4 */
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_UNICAST_IPV4_UDP] = RTE_ETH_RSS_NONFRAG_IPV4_UDP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_MULTICAST_IPV4_UDP] = RTE_ETH_RSS_NONFRAG_IPV4_UDP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV4_UDP] = RTE_ETH_RSS_NONFRAG_IPV4_UDP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV4_TCP_SYN_NO_ACK] = RTE_ETH_RSS_NONFRAG_IPV4_TCP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV4_TCP] = RTE_ETH_RSS_NONFRAG_IPV4_TCP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV4_SCTP] = RTE_ETH_RSS_NONFRAG_IPV4_SCTP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV4_OTHER] = RTE_ETH_RSS_NONFRAG_IPV4_OTHER;
	map_hena_rss[IAVF_FILTER_PCTYPE_FRAG_IPV4] = RTE_ETH_RSS_FRAG_IPV4;
	/* IPv6 */
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_UNICAST_IPV6_UDP] = RTE_ETH_RSS_NONFRAG_IPV6_UDP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_MULTICAST_IPV6_UDP] = RTE_ETH_RSS_NONFRAG_IPV6_UDP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV6_UDP] = RTE_ETH_RSS_NONFRAG_IPV6_UDP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV6_TCP_SYN_NO_ACK] = RTE_ETH_RSS_NONFRAG_IPV6_TCP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV6_TCP] = RTE_ETH_RSS_NONFRAG_IPV6_TCP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV6_SCTP] = RTE_ETH_RSS_NONFRAG_IPV6_SCTP;
	map_hena_rss[IAVF_FILTER_PCTYPE_NONF_IPV6_OTHER] = RTE_ETH_RSS_NONFRAG_IPV6_OTHER;
	map_hena_rss[IAVF_FILTER_PCTYPE_FRAG_IPV6] = RTE_ETH_RSS_FRAG_IPV6;
	/* L2 Payload */
	map_hena_rss[IAVF_FILTER_PCTYPE_L2_PAYLOAD] = RTE_ETH_RSS_L2_PAYLOAD;

	const uint64_t ipv4_rss = RTE_ETH_RSS_NONFRAG_IPV4_UDP |
				  RTE_ETH_RSS_NONFRAG_IPV4_TCP |
				  RTE_ETH_RSS_NONFRAG_IPV4_SCTP |
				  RTE_ETH_RSS_NONFRAG_IPV4_OTHER |
				  RTE_ETH_RSS_FRAG_IPV4;

	const uint64_t ipv6_rss = RTE_ETH_RSS_NONFRAG_IPV6_UDP |
				  RTE_ETH_RSS_NONFRAG_IPV6_TCP |
				  RTE_ETH_RSS_NONFRAG_IPV6_SCTP |
				  RTE_ETH_RSS_NONFRAG_IPV6_OTHER |
				  RTE_ETH_RSS_FRAG_IPV6;

	struct iavf_info *vf =  IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint64_t caps = 0, hena = 0, valid_rss_hf = 0;
	uint32_t i;
	int ret;

	ret = iavf_get_hena_caps(adapter, &caps);
	if (ret) {
		/* RSS offload type configuration is not a necessary feature for VF. */
		PMD_DRV_LOG(WARNING, "fail to get RSS offload type caps, ret: %d", ret);
		return;
	}

	/* RTE_ETH_RSS_IPV4 and RTE_ETH_RSS_IPV6 generalise all other IPv4 and IPv6 RSS types. */
	if (rss_hf & RTE_ETH_RSS_IPV4)
		rss_hf |= ipv4_rss;

	if (rss_hf & RTE_ETH_RSS_IPV6)
		rss_hf |= ipv6_rss;

	static_assert(RTE_DIM(map_hena_rss) <= sizeof(uint64_t) * CHAR_BIT, "hena is 64 bits");

	for (i = 0; i < RTE_DIM(map_hena_rss); i++) {
		uint64_t bit = 1ULL << i;

		if ((caps & bit) && (map_hena_rss[i] & rss_hf)) {
			valid_rss_hf |= map_hena_rss[i];
			hena |= bit;
		}
	}

	ret = iavf_set_hena(adapter, hena);
	if (ret) {
		PMD_DRV_LOG(WARNING, "fail to set RSS offload types, ret: %d", ret);
		return;
	}

	if (valid_rss_hf & ipv4_rss)
		valid_rss_hf |= rss_hf & RTE_ETH_RSS_IPV4;

	if (valid_rss_hf & ipv6_rss)
		valid_rss_hf |= rss_hf & RTE_ETH_RSS_IPV6;

	if (rss_hf & ~valid_rss_hf)
		PMD_DRV_LOG(WARNING, "Unsupported rss_hf 0x%" PRIx64,
			    rss_hf & ~valid_rss_hf);

	vf->rss_hf = valid_rss_hf;
}

/* A Toeplitz key nobody can predict across boots; DPDK uses rte_rand(). */
static uint8_t
iavf_rand_byte(void)
{
	static uint64_t x = processor::ticks() | 1;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return (uint8_t)x;
}

static int
iavf_init_rss(struct iavf_adapter *adapter)
{
	struct iavf_info *vf =  IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct rte_eth_rss_conf *rss_conf;
	uint16_t i, j, nb_q;
	int ret;

	rss_conf = &adapter->dev_data->dev_conf.rx_adv_conf.rss_conf;
	nb_q = RTE_MIN(adapter->dev_data->nb_rx_queues,
		       vf->max_rss_qregion);

	if (!(vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RSS_PF)) {
		PMD_DRV_LOG(DEBUG, "RSS is not supported");
		return -ENOTSUP;
	}

	/* configure RSS key */
	if (!rss_conf->rss_key) {
		/* Calculate the default hash key */
		for (i = 0; i < vf->vf_res->rss_key_size; i++)
			vf->rss_key[i] = iavf_rand_byte();
	} else
		memcpy(vf->rss_key, rss_conf->rss_key,
			   RTE_MIN(rss_conf->rss_key_len,
				   vf->vf_res->rss_key_size));

	/* init RSS LUT table */
	for (i = 0, j = 0; i < vf->vf_res->rss_lut_size; i++, j++) {
		if (j >= nb_q)
			j = 0;
		vf->rss_lut[i] = j;
	}
	/* send virtchnl ops to configure RSS */
	ret = iavf_configure_rss_lut(adapter);
	if (ret)
		return ret;
	ret = iavf_configure_rss_key(adapter);
	if (ret)
		return ret;

	/*
	 * DPDK programs advanced RSS through its rte_flow hash engine (iavf_hash.c), which is not ported; the PF
	 * takes the hena bitmap whether or not it offers advanced RSS.
	 */
	iavf_config_rss_hf(adapter, rss_conf->rss_hf);

	return 0;
}

static int iavf_reinit(struct rte_eth_dev *dev);

static int
iavf_queues_req_reset(struct rte_eth_dev *dev, uint16_t num)
{
	struct iavf_adapter *ad =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf =  IAVF_DEV_PRIVATE_TO_VF(ad);
	int ret;

	ret = iavf_request_queues(dev, num);
	if (ret) {
		PMD_DRV_LOG(ERR, "request queues from PF failed");
		return ret;
	}
	PMD_DRV_LOG(INFO, "change queue pairs from %u to %u",
			vf->vsi_res->num_queue_pairs, num);

	ret = iavf_reinit(dev);
	if (ret) {
		PMD_DRV_LOG(ERR, "vf reset failed");
		return ret;
	}

	return 0;
}

static int
iavf_dev_configure(struct rte_eth_dev *dev)
{
	struct iavf_adapter *ad =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf =  IAVF_DEV_PRIVATE_TO_VF(ad);
	uint16_t num_queue_pairs = RTE_MAX(dev->data.nb_rx_queues,
		dev->data.nb_tx_queues);
	int ret;

	if (ad->closed)
		return -EIO;

	ad->rx_bulk_alloc_allowed = true;

	ad->tx_func_type = IAVF_TX_DEFAULT;

	if (dev->data.dev_conf.rxmode.mq_mode & RTE_ETH_MQ_RX_RSS_FLAG)
		dev->data.dev_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_RSS_HASH;

	/* Large VF setting */
	if (num_queue_pairs > IAVF_MAX_NUM_QUEUES_DFLT) {
		if (!(vf->vf_res->vf_cap_flags &
				VIRTCHNL_VF_LARGE_NUM_QPAIRS)) {
			PMD_DRV_LOG(ERR, "large VF is not supported");
			return -1;
		}

		if (num_queue_pairs > IAVF_MAX_NUM_QUEUES_LV) {
			PMD_DRV_LOG(ERR, "queue pairs number cannot be larger than %u",
				IAVF_MAX_NUM_QUEUES_LV);
			return -1;
		}

		ret = iavf_queues_req_reset(dev, num_queue_pairs);
		if (ret)
			return ret;

		ret = iavf_get_max_rss_queue_region(ad);
		if (ret) {
			PMD_INIT_LOG(ERR, "get max rss queue region failed");
			return ret;
		}

		vf->lv_enabled = true;
	} else {
		/* Check if large VF is already enabled. If so, disable and
		 * release redundant queue resource.
		 * Or check if enough queue pairs. If not, request them from PF.
		 */
		if (vf->lv_enabled ||
		    num_queue_pairs > vf->vsi_res->num_queue_pairs) {
			ret = iavf_queues_req_reset(dev, num_queue_pairs);
			if (ret)
				return ret;

			vf->lv_enabled = false;
		}
		/* if large VF is not required, use default rss queue region */
		vf->max_rss_qregion = IAVF_MAX_NUM_QUEUES_DFLT;
	}

	if (vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RSS_PF) {
		if (iavf_init_rss(ad) != 0) {
			PMD_DRV_LOG(ERR, "configure rss failed");
			return -1;
		}
	}

	return 0;
}

static int
iavf_init_rxq(struct rte_eth_dev *dev, struct ci_rx_queue *rxq)
{
	struct iavf_adapter *ad = IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(ad);
	uint16_t buf_size, max_pkt_len;
	uint32_t frame_size = ad->mtu + IAVF_ETH_OVERHEAD;

	buf_size = rte_pktmbuf_data_room_size(rxq->mp) - RTE_PKTMBUF_HEADROOM;

	/* Calculate the maximum packet length allowed */
	max_pkt_len = RTE_MIN((uint32_t)
			rxq->rx_buf_len * IAVF_MAX_CHAINED_RX_BUFFERS,
			frame_size);

	/* Check if maximum packet length is set correctly.  */
	if (max_pkt_len <= RTE_ETHER_MIN_LEN ||
	    max_pkt_len > IAVF_FRAME_SIZE_MAX) {
		PMD_DRV_LOG(ERR, "maximum packet length must be "
			    "larger than %u and smaller than %u",
			    (uint32_t)IAVF_ETH_MAX_LEN,
			    (uint32_t)IAVF_FRAME_SIZE_MAX);
		return -EINVAL;
	}

	rxq->max_pkt_len = max_pkt_len;
	if ((dev->data.dev_conf.rxmode.offloads & RTE_ETH_RX_OFFLOAD_SCATTER) ||
	    rxq->max_pkt_len > buf_size) {
		ad->scattered_rx = true;
	}
	IAVF_PCI_REG_WRITE(rxq->qrx_tail, rxq->nb_rx_desc - 1);
	IAVF_WRITE_FLUSH(hw);

	return 0;
}

static int
iavf_init_queues(struct rte_eth_dev *dev)
{
	struct ci_rx_queue **rxq = (struct ci_rx_queue **)dev->data.rx_queues.data();
	int i, ret = IAVF_SUCCESS;

	for (i = 0; i < dev->data.nb_rx_queues; i++) {
		if (!rxq[i] || !rxq[i]->q_set)
			continue;
		ret = iavf_init_rxq(dev, rxq[i]);
		if (ret != IAVF_SUCCESS)
			break;
	}
	/* set rx/tx function to scatter/single-segment according to parameters */
	iavf_set_rx_function(dev);
	iavf_set_tx_function(dev);

	return ret;
}

/*
 * DPDK maps the queues to vectors through an rte_intr_handle; here the vectors come from OSv's MSI-X allocator and
 * vector 0 already belongs to the mailbox (iavf_attach).
 */
static int iavf_config_rx_queues_irqs(struct rte_eth_dev *dev)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	iavf_eth_dev *edev = iavf_edev(dev);
	struct iavf_qv_map *qv_map;
	uint16_t interval, i;
	int vec;

	rte_free(vf->qv_map);
	qv_map = (struct iavf_qv_map *)rte_zmalloc("qv_map",
		dev->data.nb_rx_queues * sizeof(struct iavf_qv_map), 0);
	if (!qv_map) {
		PMD_DRV_LOG(ERR, "Failed to allocate %d queue-vector map",
				dev->data.nb_rx_queues);
		return -1;
	}

	/* Vectors left past vector 0: what the VF has, and what the guest can route. */
	uint16_t spare = RTE_MIN((uint16_t)(vf->vf_res->max_vectors - 1),
				 (uint16_t)(edev->pdev->msix_get_num_entries() - 1));

	if (!dev->data.dev_conf.intr_conf.rxq || spare == 0) {
		/* Rx interrupt disabled, Map interrupt only for writeback */
		vf->nb_msix = 1;
		if (vf->vf_res->vf_cap_flags &
		    VIRTCHNL_VF_OFFLOAD_WB_ON_ITR) {
			/* If WB_ON_ITR supports, enable it */
			vf->msix_base = IAVF_RX_VEC_START;
			/* Set the ITR for index zero, to 2us to make sure that
			 * we leave time for aggregation to occur, but don't
			 * increase latency dramatically.
			 */
			IAVF_WRITE_REG(hw,
				       IAVF_VFINT_DYN_CTLN1(vf->msix_base - 1),
				       (0 << IAVF_VFINT_DYN_CTLN1_ITR_INDX_SHIFT) |
				       IAVF_VFINT_DYN_CTLN1_WB_ON_ITR_MASK |
				       (2UL << IAVF_VFINT_DYN_CTLN1_INTERVAL_SHIFT));
		} else {
			/* If no WB_ON_ITR offload flags, need to set
			 * interrupt for descriptor write back.
			 */
			vf->msix_base = IAVF_MISC_VEC_ID;

			/* set ITR to default */
			interval = iavf_calc_itr_interval(
					IAVF_QUEUE_ITR_INTERVAL_DEFAULT);
			IAVF_WRITE_REG(hw, IAVF_VFINT_DYN_CTL01,
				       IAVF_VFINT_DYN_CTL01_INTENA_MASK |
				       (IAVF_ITR_INDEX_DEFAULT <<
					IAVF_VFINT_DYN_CTL01_ITR_INDX_SHIFT) |
				       (interval <<
					IAVF_VFINT_DYN_CTL01_INTERVAL_SHIFT));
		}
		IAVF_WRITE_FLUSH(hw);
		/* map all queues to the same interrupt */
		for (i = 0; i < dev->data.nb_rx_queues; i++) {
			qv_map[i].queue_id = i;
			qv_map[i].vector_id = vf->msix_base;
		}
		vf->qv_map = qv_map;
	} else {
		/* Rx interrupts on their own vectors, from 1 */
		vf->nb_msix = RTE_MIN(dev->data.nb_rx_queues, spare);
		vf->msix_base = IAVF_RX_VEC_START;
		vec = IAVF_RX_VEC_START;
		for (i = 0; i < dev->data.nb_rx_queues; i++) {
			qv_map[i].queue_id = i;
			qv_map[i].vector_id = vec++;
			if (vec >= vf->nb_msix + IAVF_RX_VEC_START)
				vec = IAVF_RX_VEC_START;
		}
		vf->qv_map = qv_map;
		PMD_DRV_LOG(INFO, "%u vectors are mapping to %u Rx queues",
			    vf->nb_msix, dev->data.nb_rx_queues);
	}

	if (!vf->lv_enabled) {
		if (iavf_config_irq_map(adapter)) {
			PMD_DRV_LOG(ERR, "config interrupt mapping failed");
			goto config_irq_map_err;
		}
	} else {
		if (iavf_config_irq_map_lv(adapter, dev->data.nb_rx_queues)) {
			PMD_DRV_LOG(ERR, "config interrupt mapping for large VF failed");
			goto config_irq_map_err;
		}
	}
	return 0;

config_irq_map_err:
	rte_free(vf->qv_map);
	vf->qv_map = NULL;

	return -1;
}

static int
iavf_start_queues(struct rte_eth_dev *dev)
{
	struct ci_rx_queue *rxq;
	struct ci_tx_queue *txq;
	int i;
	uint16_t nb_txq, nb_rxq;

	for (nb_txq = 0; nb_txq < dev->data.nb_tx_queues; nb_txq++) {
		txq = (struct ci_tx_queue *)dev->data.tx_queues[nb_txq];
		if (txq->tx_deferred_start)
			continue;
		if (iavf_dev_tx_queue_start(dev, nb_txq) != 0) {
			PMD_DRV_LOG(ERR, "Fail to start tx queue %u", nb_txq);
			goto tx_err;
		}
	}

	for (nb_rxq = 0; nb_rxq < dev->data.nb_rx_queues; nb_rxq++) {
		rxq = (struct ci_rx_queue *)dev->data.rx_queues[nb_rxq];
		if (rxq->rx_deferred_start)
			continue;
		if (iavf_dev_rx_queue_start(dev, nb_rxq) != 0) {
			PMD_DRV_LOG(ERR, "Fail to start rx queue %u", nb_rxq);
			goto rx_err;
		}
	}

	return 0;

rx_err:
	for (i = 0; i < nb_rxq; i++)
		iavf_dev_rx_queue_stop(dev, i);
tx_err:
	for (i = 0; i < nb_txq; i++)
		iavf_dev_tx_queue_stop(dev, i);

	return -1;
}

static int
iavf_dev_rx_queue_intr_enable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint16_t msix_intr;

	if (adapter->closed)
		return -EIO;

	msix_intr = vf->qv_map[queue_id].vector_id;
	if (msix_intr == IAVF_MISC_VEC_ID) {
		PMD_DRV_LOG(INFO, "MISC is also enabled for control");
		IAVF_WRITE_REG(hw, IAVF_VFINT_DYN_CTL01,
			       IAVF_VFINT_DYN_CTL01_INTENA_MASK |
			       IAVF_VFINT_DYN_CTL01_CLEARPBA_MASK |
			       IAVF_VFINT_DYN_CTL01_ITR_INDX_MASK);
	} else {
		IAVF_WRITE_REG(hw,
			       IAVF_VFINT_DYN_CTLN1
				(msix_intr - IAVF_RX_VEC_START),
			       IAVF_VFINT_DYN_CTLN1_INTENA_MASK |
			       IAVF_VFINT_DYN_CTL01_CLEARPBA_MASK |
			       IAVF_VFINT_DYN_CTLN1_ITR_INDX_MASK);
	}

	IAVF_WRITE_FLUSH(hw);

	return 0;
}

static int
iavf_dev_rx_queue_intr_disable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	uint16_t msix_intr;

	msix_intr = adapter->vf.qv_map[queue_id].vector_id;
	if (msix_intr == IAVF_MISC_VEC_ID) {
		PMD_DRV_LOG(ERR, "MISC is used for control, cannot disable it");
		return -EIO;
	}

	/* Polled from here on: descriptors are written back when the ITR expires, 2 units (4 us) as in polling mode. */
	IAVF_WRITE_REG(hw,
		      IAVF_VFINT_DYN_CTLN1(msix_intr - IAVF_RX_VEC_START),
		      IAVF_VFINT_DYN_CTLN1_WB_ON_ITR_MASK |
		      (0 << IAVF_VFINT_DYN_CTLN1_ITR_INDX_SHIFT) |
		      (2UL << IAVF_VFINT_DYN_CTLN1_INTERVAL_SHIFT));

	IAVF_WRITE_FLUSH(hw);
	return 0;
}

/*
 * Rx interrupts. A queue's vector fires once and stays masked until re-enabled. In callback mode (a handler given
 * at rx_queue_setup) it wakes the driver's thread, which drains the ring into the callback and re-arms, as ENA does.
 * Otherwise the queue's owner polls it and arms the interrupt to sleep (rx_intr_enable): the vector calls its waker.
 */
static void
iavf_rxq_intr_loop(struct rte_eth_dev *dev, uint16_t qid)
{
	static constexpr unsigned kBurst = 32;
	static constexpr unsigned kBudget = 1024;
	iavf_eth_dev *edev = iavf_edev(dev);
	auto *rxq = (struct ci_rx_queue *)dev->data.rx_queues[qid];
	struct iavf_rxq_intr *intr = rxq->intr;
	rte_mbuf *pkts[kBurst];

	while (true) {
		sched::thread::current()->wait_for([intr] {
			return intr->ready.load(std::memory_order_acquire) ||
			       intr->stop.load(std::memory_order_acquire);
		});
		if (intr->stop.load(std::memory_order_acquire))
			return;
		intr->ready.store(false, std::memory_order_release);
		unsigned n = 0;
		do {
			uint16_t got = edev->rx_fn(rxq, pkts, kBurst);
			if (!got)
				break;
			n += got;
			intr->handler(pkts, got);
		} while (n < kBudget);
		iavf_dev_rx_queue_intr_enable(dev, qid);
	}
}

static int
iavf_rxq_intr_start(struct rte_eth_dev *dev)
{
	struct iavf_adapter *adapter = IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = &adapter->vf;
	iavf_eth_dev *edev = iavf_edev(dev);

	if (!dev->data.dev_conf.intr_conf.rxq || vf->msix_base != IAVF_RX_VEC_START)
		return 0;

	interrupt_manager msi(edev->pdev);
	auto vecs = msi.request_vectors(vf->nb_msix);
	if (vecs.size() != vf->nb_msix) {
		msi.free_vectors(vecs);
		PMD_DRV_LOG(ERR, "could not get %u Rx vectors", vf->nb_msix);
		return -ENOSPC;
	}
	for (uint16_t v = 0; v < vf->nb_msix; v++) {
		uint16_t vector = IAVF_RX_VEC_START + v;
		std::vector<struct ci_rx_queue *> queues;
		for (uint16_t q = 0; q < dev->data.nb_rx_queues; q++) {
			auto *rxq = (struct ci_rx_queue *)dev->data.rx_queues[q];
			if (vf->qv_map[q].vector_id == vector)
				queues.push_back(rxq);
		}
		msi.assign_isr(vecs[v], [dev, queues] {
			for (auto *rxq : queues) {
				if (rxq->intr) {
					rxq->intr->interrupts++;
					rxq->intr->ready.store(true, std::memory_order_release);
					rxq->intr->thread->wake_with_irq_disabled();
				} else {
					dev->rx_wake(rxq->queue_id);
				}
			}
		});
		msi.setup_entry(vector, vecs[v]);
		vecs[v]->set_affinity(sched::cpus[v % sched::cpus.size()]);
		edev->vectors.push_back(vecs[v]);
	}

	for (uint16_t q = 0; q < dev->data.nb_rx_queues; q++) {
		auto *rxq = (struct ci_rx_queue *)dev->data.rx_queues[q];
		if (!rxq->intr)
			continue;
		rxq->intr->vector = vf->qv_map[q].vector_id;
		rxq->intr->stop.store(false, std::memory_order_release);
		rxq->intr->thread = sched::thread::make([dev, q] { iavf_rxq_intr_loop(dev, q); },
			sched::thread::attr().name("iavf-rx" + std::to_string(q)));
		rxq->intr->thread->start();
	}
	msi.unmask_interrupts(vecs);
	/* Callback queues start armed; the others are polled until their owner arms them. */
	for (uint16_t q = 0; q < dev->data.nb_rx_queues; q++) {
		if (((struct ci_rx_queue *)dev->data.rx_queues[q])->intr)
			iavf_dev_rx_queue_intr_enable(dev, q);
		else
			iavf_dev_rx_queue_intr_disable(dev, q);
	}
	return 0;
}

static void
iavf_rxq_intr_stop(struct rte_eth_dev *dev)
{
	iavf_eth_dev *edev = iavf_edev(dev);

	for (uint16_t q = 0; q < dev->data.nb_rx_queues; q++) {
		auto *rxq = (struct ci_rx_queue *)dev->data.rx_queues[q];
		if (!rxq || !rxq->intr || !rxq->intr->thread)
			continue;
		iavf_dev_rx_queue_intr_disable(dev, q);
		rxq->intr->stop.store(true, std::memory_order_release);
		rxq->intr->thread->wake();
		rxq->intr->thread->join();
		delete rxq->intr->thread;
		rxq->intr->thread = nullptr;
	}
	if (edev->vectors.size() > 1) {
		interrupt_manager msi(edev->pdev);
		std::vector<msix_vector *> rx(edev->vectors.begin() + 1, edev->vectors.end());
		msi.free_vectors(rx);
		edev->vectors.resize(1);
	}
}

static int
iavf_dev_start(struct rte_eth_dev *dev)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint16_t num_queue_pairs;

	PMD_INIT_FUNC_TRACE();

	if (adapter->closed)
		return -1;

	adapter->stopped = 0;

	vf->max_pkt_len = adapter->mtu + IAVF_ETH_OVERHEAD;
	vf->num_queue_pairs = RTE_MAX(dev->data.nb_rx_queues,
				      dev->data.nb_tx_queues);
	num_queue_pairs = vf->num_queue_pairs;

	if (iavf_init_queues(dev) != 0) {
		PMD_DRV_LOG(ERR, "failed to do Queue init");
		return -1;
	}

	if (iavf_configure_queues(adapter, num_queue_pairs) != 0) {
		PMD_DRV_LOG(ERR, "configure queues failed");
		goto error;
	}

	if (iavf_config_rx_queues_irqs(dev) != 0) {
		PMD_DRV_LOG(ERR, "configure irq failed");
		goto error;
	}

	if (!adapter->mac_primary_set) {
		if (iavf_add_del_eth_addr(adapter, &vf->mac_addrs[0], true,
				VIRTCHNL_ETHER_ADDR_PRIMARY) != 0)
			PMD_DRV_LOG(ERR, "failed to add primary MAC");
		else
			adapter->mac_primary_set = true;
	}

	if (iavf_start_queues(dev) != 0) {
		PMD_DRV_LOG(ERR, "enable queues failed");
		goto error;
	}

	if (iavf_rxq_intr_start(dev) != 0)
		goto error;

	/* An explicit start supersedes any pending deferred start */
	vf->start_pending = false;
	dev->data.dev_started = 1;

	return 0;

error:
	return -1;
}

static int
iavf_dev_stop(struct rte_eth_dev *dev)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(dev->data.data);
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);

	PMD_INIT_FUNC_TRACE();

	if (adapter->closed)
		return -1;

	if (adapter->stopped == 1)
		return 0;

	iavf_rxq_intr_stop(dev);

	iavf_stop_queues(dev);

	adapter->stopped = 1;
	dev->data.dev_started = 0;

	/* An explicit stop cancels any pending deferred start */
	vf->start_pending = false;

	return 0;
}

static int
iavf_dev_info_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = &adapter->vf;
	uint16_t max_queue_pairs;

	if (adapter->closed)
		return -EIO;

	if (vf->vf_res->vf_cap_flags & VIRTCHNL_VF_LARGE_NUM_QPAIRS)
		max_queue_pairs = IAVF_MAX_NUM_QUEUES_LV;
	else
		max_queue_pairs = RTE_MIN(vf->vsi_res->num_queue_pairs,
					  IAVF_MAX_NUM_QUEUES_DFLT);

	dev_info->max_rx_queues = max_queue_pairs;
	dev_info->max_tx_queues = max_queue_pairs;
	dev_info->min_rx_bufsize = IAVF_BUF_SIZE_MIN;
	dev_info->max_rx_bufsize = IAVF_RX_MAX_DATA_BUF_SIZE;
	dev_info->max_rx_pktlen = IAVF_FRAME_SIZE_MAX;
	dev_info->max_mtu = dev_info->max_rx_pktlen - IAVF_ETH_OVERHEAD;
	dev_info->min_mtu = RTE_ETHER_MIN_MTU;
	dev_info->hash_key_size = vf->vf_res->rss_key_size;
	dev_info->reta_size = vf->vf_res->rss_lut_size;
	dev_info->flow_type_rss_offloads = IAVF_RSS_OFFLOAD_ALL;
	dev_info->max_mac_addrs = RTE_DIM(vf->mac_addrs);
	dev_info->rx_offload_capa = IAVF_RX_SCALAR_OFFLOADS &
		~(RTE_ETH_RX_OFFLOAD_KEEP_CRC |
		  RTE_ETH_RX_OFFLOAD_VLAN_STRIP |
		  RTE_ETH_RX_OFFLOAD_QINQ_STRIP |
		  RTE_ETH_RX_OFFLOAD_VLAN_FILTER |
		  RTE_ETH_RX_OFFLOAD_VLAN_EXTEND);
	/* minidpdk mbufs carry neither VLAN tags nor TSO segment sizes. */
	dev_info->tx_offload_capa =
		RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
		RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
		RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
		RTE_ETH_TX_OFFLOAD_SCTP_CKSUM |
		RTE_ETH_TX_OFFLOAD_MULTI_SEGS;
	dev_info->rx_queue_offload_capa = 0;
	dev_info->tx_queue_offload_capa = 0;

	if (vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_CRC)
		dev_info->rx_offload_capa |= RTE_ETH_RX_OFFLOAD_KEEP_CRC;

	dev_info->default_rxconf.rx_free_thresh = IAVF_DEFAULT_RX_FREE_THRESH;
	dev_info->default_rxconf.offloads = 0;
	dev_info->default_txconf.tx_free_thresh = IAVF_DEFAULT_TX_FREE_THRESH;
	dev_info->default_txconf.offloads = 0;

	dev_info->rx_desc_lim.nb_max = IAVF_MAX_RING_DESC;
	dev_info->rx_desc_lim.nb_min = IAVF_MIN_RING_DESC;
	dev_info->tx_desc_lim.nb_max = IAVF_MAX_RING_DESC;
	dev_info->tx_desc_lim.nb_min = IAVF_MIN_RING_DESC;
	dev_info->tx_desc_lim.nb_mtu_seg_max = IAVF_TX_MAX_MTU_SEG;
	dev_info->tx_desc_lim.nb_seg_max = IAVF_MAX_RING_DESC;
	dev_info->nb_rx_queues = dev->data.nb_rx_queues;
	dev_info->nb_tx_queues = dev->data.nb_tx_queues;

	return 0;
}

/* minidpdk has no link status to publish; the VF's state is what the PF last said. */
int
iavf_dev_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(dev->data.data);

	(void)wait_to_complete;
	PMD_DRV_LOG(INFO, "link %s, %u Mbps", vf->link_up ? "up" : "down", vf->link_speed);
	return 0;
}

static int
iavf_dev_rss_reta_update(struct rte_eth_dev *dev,
			struct rte_eth_rss_reta_entry64 *reta_conf,
			uint16_t reta_size)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t *lut;
	uint16_t i, idx, shift;
	int ret;

	if (adapter->closed)
		return -EIO;

	if (!(vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RSS_PF))
		return -ENOTSUP;

	if (reta_size != vf->vf_res->rss_lut_size) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number of hardware can "
			"support (%d)", reta_size, vf->vf_res->rss_lut_size);
		return -EINVAL;
	}

	lut = (uint8_t *)calloc(1, reta_size);
	if (!lut) {
		PMD_DRV_LOG(ERR, "No memory can be allocated");
		return -ENOMEM;
	}
	/* store the old lut table temporarily */
	memcpy(lut, vf->rss_lut, reta_size);

	for (i = 0; i < reta_size; i++) {
		idx = i / RTE_ETH_RETA_GROUP_SIZE;
		shift = i % RTE_ETH_RETA_GROUP_SIZE;
		if (reta_conf[idx].mask & (1ULL << shift))
			lut[i] = reta_conf[idx].reta[shift];
	}

	memcpy(vf->rss_lut, lut, reta_size);
	/* send virtchnl ops to configure RSS */
	ret = iavf_configure_rss_lut(adapter);
	if (ret) /* revert back */
		memcpy(vf->rss_lut, lut, reta_size);
	free(lut);

	return ret;
}

static int
iavf_dev_rss_reta_query(struct rte_eth_dev *dev,
		       struct rte_eth_rss_reta_entry64 *reta_conf,
		       uint16_t reta_size)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint16_t i, idx, shift;

	if (adapter->closed)
		return -EIO;

	if (!(vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RSS_PF))
		return -ENOTSUP;

	if (reta_size != vf->vf_res->rss_lut_size) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number of hardware can "
			"support (%d)", reta_size, vf->vf_res->rss_lut_size);
		return -EINVAL;
	}

	for (i = 0; i < reta_size; i++) {
		idx = i / RTE_ETH_RETA_GROUP_SIZE;
		shift = i % RTE_ETH_RETA_GROUP_SIZE;
		if (reta_conf[idx].mask & (1ULL << shift))
			reta_conf[idx].reta[shift] = vf->rss_lut[i];
	}

	return 0;
}

static int
iavf_dev_rss_hash_conf_get(struct rte_eth_dev *dev,
			  struct rte_eth_rss_conf *rss_conf)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);

	if (adapter->closed)
		return -EIO;

	if (!(vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RSS_PF))
		return -ENOTSUP;

	rss_conf->rss_hf = vf->rss_hf;
	rss_conf->algorithm = RTE_ETH_HASH_FUNCTION_TOEPLITZ;

	if (!rss_conf->rss_key)
		return 0;

	rss_conf->rss_key_len = vf->vf_res->rss_key_size;
	memcpy(rss_conf->rss_key, vf->rss_key, rss_conf->rss_key_len);

	return 0;
}

static void
iavf_stat_update_48(uint64_t *offset, uint64_t *stat)
{
	if (*stat >= *offset)
		*stat = *stat - *offset;
	else
		*stat = (uint64_t)((*stat +
			((uint64_t)1 << IAVF_48_BIT_WIDTH)) - *offset);

	*stat &= IAVF_48_BIT_MASK;
}

static void
iavf_stat_update_32(uint64_t *offset, uint64_t *stat)
{
	if (*stat >= *offset)
		*stat = (uint64_t)(*stat - *offset);
	else
		*stat = (uint64_t)((*stat +
			((uint64_t)1 << IAVF_32_BIT_WIDTH)) - *offset);
}

static void
iavf_update_stats(struct iavf_vsi *vsi, struct virtchnl_eth_stats *nes)
{
	struct virtchnl_eth_stats *oes = &vsi->eth_stats_offset.eth_stats;

	iavf_stat_update_48(&oes->rx_bytes, &nes->rx_bytes);
	iavf_stat_update_48(&oes->rx_unicast, &nes->rx_unicast);
	iavf_stat_update_48(&oes->rx_multicast, &nes->rx_multicast);
	iavf_stat_update_48(&oes->rx_broadcast, &nes->rx_broadcast);
	iavf_stat_update_32(&oes->rx_discards, &nes->rx_discards);
	iavf_stat_update_48(&oes->tx_bytes, &nes->tx_bytes);
	iavf_stat_update_48(&oes->tx_unicast, &nes->tx_unicast);
	iavf_stat_update_48(&oes->tx_multicast, &nes->tx_multicast);
	iavf_stat_update_48(&oes->tx_broadcast, &nes->tx_broadcast);
	iavf_stat_update_32(&oes->tx_errors, &nes->tx_errors);
	iavf_stat_update_32(&oes->tx_discards, &nes->tx_discards);
}

static int
iavf_dev_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct iavf_vsi *vsi = &vf->vsi;
	struct virtchnl_eth_stats pstats;
	int ret;

	ret = iavf_query_stats(adapter, &pstats);
	if (ret == 0) {
		uint8_t crc_stats_len = (dev->data.dev_conf.rxmode.offloads &
					 RTE_ETH_RX_OFFLOAD_KEEP_CRC) ? 0 :
					 RTE_ETHER_CRC_LEN;
		iavf_update_stats(vsi, &pstats);
		uint64_t ipackets = pstats.rx_unicast + pstats.rx_multicast + pstats.rx_broadcast;
		/* The unicast/multicast/broadcast counters include discards; saturate rather than underflow. */
		ipackets = ipackets >= pstats.rx_discards ? ipackets - pstats.rx_discards : 0;
		stats->ipackets = ipackets;
		stats->opackets = pstats.tx_broadcast + pstats.tx_multicast +
						pstats.tx_unicast;
		stats->imissed = pstats.rx_discards;
		stats->oerrors = pstats.tx_errors + pstats.tx_discards;
		stats->ibytes = pstats.rx_bytes - ipackets * crc_stats_len;
		stats->obytes = pstats.tx_bytes;
	} else {
		PMD_DRV_LOG(ERR, "Get statistics failed");
	}
	return ret;
}

static int
iavf_dev_stats_reset(struct rte_eth_dev *dev)
{
	int ret;
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct iavf_vsi *vsi = &vf->vsi;
	struct virtchnl_eth_stats stats;

	/* read stat values to clear hardware registers */
	ret = iavf_query_stats(adapter, &stats);
	if (ret != 0)
		return ret;

	/* set stats offset base on current values */
	vsi->eth_stats_offset.eth_stats = stats;

	return 0;
}

static int
iavf_check_vf_reset_done(struct iavf_hw *hw)
{
	int i, reset;

	for (i = 0; i < IAVF_RESET_WAIT_CNT; i++) {
		reset = IAVF_READ_REG(hw, IAVF_VFGEN_RSTAT) &
			IAVF_VFGEN_RSTAT_VFR_STATE_MASK;
		reset = reset >> IAVF_VFGEN_RSTAT_VFR_STATE_SHIFT;
		if (reset == VIRTCHNL_VFR_VFACTIVE ||
		    reset == VIRTCHNL_VFR_COMPLETED)
			break;
		rte_delay_us_sleep(20 * 1000);
	}

	if (i >= IAVF_RESET_WAIT_CNT)
		return -1;

	return 0;
}

static int
iavf_init_vf(struct rte_eth_dev *dev)
{
	int err, bufsz;
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);

	vf->eth_dev = dev;

	err = iavf_set_mac_type(hw);
	if (err) {
		PMD_INIT_LOG(ERR, "set_mac_type failed: %d", err);
		goto err;
	}

	err = iavf_check_vf_reset_done(hw);
	if (err) {
		PMD_INIT_LOG(ERR, "VF is still resetting");
		goto err;
	}

	iavf_init_adminq_parameter(hw);
	err = iavf_init_adminq(hw);
	if (err) {
		PMD_INIT_LOG(ERR, "init_adminq failed: %d", err);
		goto err;
	}

	if (iavf_check_api_version(adapter) != 0) {
		PMD_INIT_LOG(ERR, "check_api version failed");
		goto err_api;
	}

	bufsz = sizeof(struct virtchnl_vf_resource) +
		(IAVF_MAX_VF_VSI * sizeof(struct virtchnl_vsi_resource));
	vf->vf_res = (struct virtchnl_vf_resource *)rte_zmalloc("vf_res", bufsz, 0);
	if (!vf->vf_res) {
		PMD_INIT_LOG(ERR, "unable to allocate vf_res memory");
		goto err_api;
	}

	if (iavf_get_vf_resource(adapter) != 0) {
		PMD_INIT_LOG(ERR, "iavf_get_vf_config failed");
		goto err_alloc;
	}
	/* Allocate memort for RSS info */
	if (vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RSS_PF) {
		vf->rss_key = (uint8_t *)rte_zmalloc("rss_key",
					  vf->vf_res->rss_key_size, 0);
		if (!vf->rss_key) {
			PMD_INIT_LOG(ERR, "unable to allocate rss_key memory");
			goto err_rss;
		}
		vf->rss_lut = (uint8_t *)rte_zmalloc("rss_lut",
					  vf->vf_res->rss_lut_size, 0);
		if (!vf->rss_lut) {
			PMD_INIT_LOG(ERR, "unable to allocate rss_lut memory");
			goto err_rss;
		}
	}

	if (vf->vsi_res->num_queue_pairs > IAVF_MAX_NUM_QUEUES_DFLT)
		vf->lv_enabled = true;

	if (vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RX_FLEX_DESC) {
		if (iavf_get_supported_rxdid(adapter) != 0) {
			PMD_INIT_LOG(ERR, "failed to do get supported rxdid");
			goto err_rss;
		}
	}

	return 0;
err_rss:
	rte_free(vf->rss_key);
	rte_free(vf->rss_lut);
err_alloc:
	rte_free(vf->vf_res);
	vf->vsi_res = NULL;
err_api:
	iavf_shutdown_adminq(hw);
err:
	return -1;
}

static void
iavf_uninit_vf(struct rte_eth_dev *dev)
{
	struct iavf_adapter *adapter = IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);

	iavf_shutdown_adminq(hw);

	rte_free(vf->vf_res);
	vf->vsi_res = NULL;
	vf->vf_res = NULL;

	rte_free(vf->rss_lut);
	vf->rss_lut = NULL;
	rte_free(vf->rss_key);
	vf->rss_key = NULL;
}

/* Enable default admin queue interrupt setting */
static inline void
iavf_enable_irq0(struct iavf_hw *hw)
{
	/* Enable admin queue interrupt trigger */
	IAVF_WRITE_REG(hw, IAVF_VFINT_ICR0_ENA1,
		       IAVF_VFINT_ICR0_ENA1_ADMINQ_MASK);

	IAVF_WRITE_REG(hw, IAVF_VFINT_DYN_CTL01,
		       IAVF_VFINT_DYN_CTL01_INTENA_MASK |
		       IAVF_VFINT_DYN_CTL01_CLEARPBA_MASK |
		       IAVF_VFINT_DYN_CTL01_ITR_INDX_MASK);

	IAVF_WRITE_FLUSH(hw);
}

static inline void
iavf_disable_irq0(struct iavf_hw *hw)
{
	/* Disable all interrupt types */
	IAVF_WRITE_REG(hw, IAVF_VFINT_ICR0_ENA1, 0);
	IAVF_WRITE_REG(hw, IAVF_VFINT_DYN_CTL01,
		       IAVF_VFINT_DYN_CTL01_ITR_INDX_MASK);
	IAVF_WRITE_FLUSH(hw);
}

static void
iavf_dev_interrupt_handler(void *param)
{
	struct rte_eth_dev *dev = (struct rte_eth_dev *)param;
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(dev->data.data);

	iavf_disable_irq0(hw);

	iavf_handle_virtchnl_msg(dev);

	iavf_enable_irq0(hw);
}

static void
iavf_default_rss_disable(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	int ret = 0;

	if (vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RSS_PF) {
		/* Set hena = 0 to ask PF to cleanup all existing RSS. */
		ret = iavf_set_hena(adapter, 0);
		if (ret)
			/* Temporarily allow an error, a PF may not handle hena = 0. */
			PMD_INIT_LOG(WARNING, "fail to disable default RSS,"
				    "lack PF support");
	}
}

void
iavf_set_no_poll(struct iavf_adapter *adapter, bool link_change)
{
	struct iavf_info *vf = &adapter->vf;
	bool no_poll;

	no_poll = (link_change & !vf->link_up) ||
		vf->vf_reset ||
		rte_atomic_load_explicit(&vf->in_reset_recovery, rte_memory_order_relaxed);

	rte_atomic_store_explicit(&adapter->no_poll, no_poll,
				  rte_memory_order_release);
}

static bool
iavf_is_reset(struct iavf_hw *hw)
{
	uint32_t rstat;

	/* ARQ has been disabled by the PF as part of the VFR. */
	if (!(IAVF_READ_REG(hw, IAVF_VF_ARQLEN1) &
		IAVF_VF_ARQLEN1_ARQENABLE_MASK))
		return true;

	/*
	 * VFGEN_RSTAT reports VIRTCHNL_VFR_INPROGRESS. At times, the PF flips ARQENABLE so quickly around a VFR that
	 * the ARQLEN1 sample window misses it.
	 */
	rstat = (IAVF_READ_REG(hw, IAVF_VFGEN_RSTAT) &
		 IAVF_VFGEN_RSTAT_VFR_STATE_MASK) >>
		IAVF_VFGEN_RSTAT_VFR_STATE_SHIFT;

	return rstat == VIRTCHNL_VFR_INPROGRESS;
}

static bool
iavf_is_reset_detected(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	int i;

	/* Poll until the reset actually happens, every 5 ms to catch the fast ARQ flips. */
	for (i = 0; i < IAVF_RESET_DETECTED_CNT * IAVF_RESET_POLL_SCALE; i++) {
		if (iavf_is_reset(hw))
			return true;
		rte_delay_us_sleep(5000);
	}

	return false;
}

/*
 * After a queue request the PF resets the VF. DPDK closes and re-probes the port (iavf_dev_reset); here the mailbox
 * thread keeps running, so the mailbox switches to polling while the admin queue is torn down and brought back.
 */
static int
iavf_reinit(struct rte_eth_dev *dev)
{
	struct iavf_adapter *adapter = IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	struct iavf_info *vf = &adapter->vf;

	vf->aq_intr_enabled = false;
	iavf_disable_irq0(hw);
	/* The PF announced the reset; probing its status before it starts would find the VF still active. */
	if (!iavf_is_reset_detected(adapter))
		PMD_DRV_LOG(WARNING, "no VF reset seen, reinitialising anyway");
	iavf_uninit_vf(dev);
	vf->vf_reset = false;
	adapter->mac_primary_set = false;
	if (iavf_init_vf(dev) != 0) {
		PMD_INIT_LOG(ERR, "Init vf failed after reset");
		return -1;
	}
	iavf_enable_irq0(hw);
	vf->aq_intr_enabled = true;
	iavf_set_no_poll(adapter, false);
	return 0;
}

bool
iavf_in_mgmt_thread(struct rte_eth_dev *dev)
{
	return dev && sched::thread::current() == iavf_edev(dev)->mgmt_thread;
}

static void
iavf_mgmt_loop(iavf_eth_dev *edev)
{
	while (true) {
		sched::thread::current()->wait_for([edev] {
			return edev->mgmt_pending.load(std::memory_order_acquire) ||
			       edev->mgmt_stop.load(std::memory_order_acquire);
		});
		if (edev->mgmt_stop.load(std::memory_order_acquire))
			return;
		edev->mgmt_pending.store(false, std::memory_order_release);
		iavf_dev_interrupt_handler(edev);
	}
}

/* Vector 0: the mailbox and PF events, handled on a thread since reading them takes sleeping locks. */
static int
iavf_setup_misc_vector(iavf_eth_dev *edev)
{
	pci::device *pdev = edev->pdev;

	pdev->set_bus_master(true);
	pdev->msix_enable();
	if (!pdev->is_msix() || pdev->msix_get_num_entries() < 1) {
		PMD_INIT_LOG(ERR, "no MSI-X");
		return -ENOTSUP;
	}

	edev->mgmt_thread = sched::thread::make([edev] { iavf_mgmt_loop(edev); },
		sched::thread::attr().name("iavf-mbx"));
	edev->mgmt_thread->start();

	interrupt_manager msi(pdev);
	auto vecs = msi.request_vectors(1);
	if (vecs.size() != 1 ||
	    !msi.assign_isr(vecs[0], [edev] {
		    edev->mgmt_pending.store(true, std::memory_order_release);
		    edev->mgmt_thread->wake_with_irq_disabled();
	    }) ||
	    !msi.setup_entry(IAVF_MISC_VEC_ID, vecs[0])) {
		msi.free_vectors(vecs);
		PMD_INIT_LOG(ERR, "could not set up the mailbox vector");
		return -ENXIO;
	}
	msi.unmask_interrupts(vecs);
	edev->vectors = vecs;
	return 0;
}

static int
iavf_dev_init(iavf_eth_dev *eth_dev)
{
	struct iavf_adapter *adapter = eth_dev->adapter;
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	pci::device *pci_dev = eth_dev->pdev;
	u8 bus, devid, func;

	PMD_INIT_FUNC_TRACE();

	pci_dev->get_bdf(bus, devid, func);
	hw->vendor_id = pci_dev->get_vendor_id();
	hw->device_id = pci_dev->get_device_id();
	hw->subsystem_vendor_id = pci_dev->get_subsystem_vid();
	hw->subsystem_device_id = pci_dev->get_subsystem_id();
	hw->bus.bus_id = bus;
	hw->bus.device = devid;
	hw->bus.func = func;

	eth_dev->regs = pci_dev->get_bar(1);
	if (!eth_dev->regs) {
		PMD_INIT_LOG(ERR, "no register BAR");
		return -ENXIO;
	}
	eth_dev->regs->map();
	hw->hw_addr = (u8 *)const_cast<void *>(eth_dev->regs->get_mmio());
	hw->back = adapter;
	adapter->dev_data = &eth_dev->data;
	adapter->stopped = 1;
	adapter->mac_primary_set = false;
	adapter->tpid = RTE_ETHER_TYPE_VLAN; /* VLAN TPID set to 0x8100 by default */
	adapter->mtu = RTE_ETHER_MTU;
	rte_spinlock_init(&vf->aq_lock);

	if (iavf_init_vf(eth_dev) != 0) {
		PMD_INIT_LOG(ERR, "Init vf failed");
		return -1;
	}

	/* set default ptype table */
	iavf_set_default_ptype_table(eth_dev);

	/* If the MAC address is not configured by host, generate a random one. */
	if (!rte_is_valid_assigned_ether_addr(
			(struct rte_ether_addr *)hw->mac.addr)) {
		for (auto &b : hw->mac.addr)
			b = iavf_rand_byte();
		hw->mac.addr[0] = (hw->mac.addr[0] & 0xfe) | 0x02;
	}
	memcpy(vf->mac_addrs[0].addr.data(), hw->mac.addr, RTE_ETHER_ADDR_LEN);
	memcpy(eth_dev->data.mac_addr.addr.data(), hw->mac.addr, RTE_ETHER_ADDR_LEN);

	if (iavf_setup_misc_vector(eth_dev) != 0)
		goto init_vf_err;

	/* configure and enable device interrupt */
	iavf_enable_irq0(hw);
	vf->aq_intr_enabled = true;

	iavf_default_rss_disable(adapter);

	iavf_dev_stats_reset(eth_dev);

	adapter->closed = false;

	return 0;

init_vf_err:
	iavf_uninit_vf(eth_dev);

	return -1;
}

/* The ethdev entry points minidpdk calls. */

static uint16_t
iavf_rx_burst(rte_eth_dev *dev, uint16_t qid, rte_mbuf **pkts, uint16_t n)
{
	iavf_eth_dev *edev = iavf_edev(dev);

	/* Nothing moves while the VF resets. */
	if (unlikely(edev->adapter->no_poll.load(std::memory_order_acquire)))
		return 0;
	return edev->rx_fn(dev->data.rx_queues[qid], pkts, n);
}

static uint16_t
iavf_tx_burst(rte_eth_dev *dev, uint16_t qid, rte_mbuf **pkts, uint16_t n)
{
	iavf_eth_dev *edev = iavf_edev(dev);

	if (unlikely(edev->adapter->no_poll.load(std::memory_order_acquire)))
		return 0;
	return edev->tx_fn(dev->data.tx_queues[qid], pkts, n);
}

int iavf_eth_dev::mtu_set(uint16_t mtu)
{
	/* mtu setting is forbidden if port is start */
	if (data.dev_started) {
		PMD_DRV_LOG(ERR, "port must be stopped before configuration");
		return -EBUSY;
	}
	if (mtu < RTE_ETHER_MIN_MTU || mtu > IAVF_FRAME_SIZE_MAX - IAVF_ETH_OVERHEAD)
		return -EINVAL;
	adapter->mtu = mtu;
	return 0;
}

int iavf_eth_dev::start()
{
	return iavf_dev_start(this);
}

int iavf_eth_dev::stop()
{
	return iavf_dev_stop(this);
}

int iavf_eth_dev::tx_queue_setup(uint16_t qid, uint16_t nb_desc, unsigned int socket_id,
				 const struct rte_eth_txconf *tx_conf)
{
	return iavf_dev_tx_queue_setup(this, qid, nb_desc, socket_id, tx_conf);
}

int iavf_eth_dev::rx_queue_setup(uint16_t qid, uint16_t nb_desc, unsigned int socket_id,
				 const struct rte_eth_rxconf *rx_conf, rte_mempool *mp,
				 irq_handler_cb_t handler)
{
	int ret = iavf_dev_rx_queue_setup(this, qid, nb_desc, socket_id, rx_conf, mp);
	if (ret || !handler)
		return ret;
	auto *rxq = (struct ci_rx_queue *)data.rx_queues[qid];
	rxq->intr = new iavf_rxq_intr();
	rxq->intr->handler = std::move(handler);
	return 0;
}

int iavf_eth_dev::drv_configure()
{
	return iavf_dev_configure(this);
}

void iavf_eth_dev::get_stats(rte_eth_stats *stats)
{
	iavf_dev_stats_get(this, stats);
}

int iavf_eth_dev::get_dev_info(rte_eth_dev_info *info)
{
	return iavf_dev_info_get(this, info);
}

int iavf_eth_dev::rss_reta_update(rte_eth_rss_reta_entry64 *reta, uint16_t reta_size)
{
	return iavf_dev_rss_reta_update(this, reta, reta_size);
}

int iavf_eth_dev::rss_reta_query(rte_eth_rss_reta_entry64 *reta, uint16_t reta_size)
{
	return iavf_dev_rss_reta_query(this, reta, reta_size);
}

int iavf_eth_dev::rss_hash_conf_get(rte_eth_rss_conf *rss_conf)
{
	return iavf_dev_rss_hash_conf_get(this, rss_conf);
}

int iavf_eth_dev::rx_intr_enable(uint16_t qid)
{
	if (!data.dev_conf.intr_conf.rxq || adapter->vf.msix_base != IAVF_RX_VEC_START || qid >= data.nb_rx_queues)
		return -ENOTSUP;
	return iavf_dev_rx_queue_intr_enable(this, qid);
}

int iavf_eth_dev::rx_intr_disable(uint16_t qid)
{
	if (!data.dev_conf.intr_conf.rxq || adapter->vf.msix_base != IAVF_RX_VEC_START || qid >= data.nb_rx_queues)
		return -ENOTSUP;
	return iavf_dev_rx_queue_intr_disable(this, qid);
}

bool
iavf_probe(pci::device *dev)
{
	static const uint16_t ids[] = {
		IAVF_DEV_ID_ADAPTIVE_VF, IAVF_DEV_ID_VF, IAVF_DEV_ID_VF_HV,
		IAVF_DEV_ID_X722_VF, IAVF_DEV_ID_X722_A0_VF,
	};
	for (auto id : ids)
		if (dev->get_id() == hw_device_id(IAVF_INTEL_VENDOR_ID, id))
			return true;
	return false;
}

int
iavf_attach(pci::device *dev, iavf_eth_dev **out)
{
	auto *adapter = aligned_new<iavf_adapter>();
	auto *edev = new iavf_eth_dev(adapter);
	edev->pdev = dev;
	edev->tx_burst = iavf_tx_burst;
	edev->rx_burst = iavf_rx_burst;

	if (iavf_dev_init(edev) != 0) {
		delete edev;
		return -ENXIO;
	}

	struct iavf_info *vf = &adapter->vf;
	const uint8_t *m = adapter->hw.mac.addr;
	printf("iavf: %02x:%02x:%02x:%02x:%02x:%02x, virtchnl %u.%u, %u queue pairs, %u vectors, "
	       "rss key %u lut %u, caps 0x%08x%s\n",
	       m[0], m[1], m[2], m[3], m[4], m[5], vf->virtchnl_version.major, vf->virtchnl_version.minor,
	       vf->vsi_res->num_queue_pairs, vf->vf_res->max_vectors, vf->vf_res->rss_key_size,
	       vf->vf_res->rss_lut_size, vf->vf_res->vf_cap_flags,
	       (vf->vf_res->vf_cap_flags & VIRTCHNL_VF_LARGE_NUM_QPAIRS) ? " (large VF)" : "");

	eth_os::register_port(edev);
	*out = edev;
	return 0;
}
