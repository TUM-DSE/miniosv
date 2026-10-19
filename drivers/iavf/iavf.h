/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017 Intel Corporation
 */

// DPDK's drivers/net/intel/iavf/iavf.h without traffic management, rte_flow parsing, inline IPsec, PTP, VLAN
// offloads, devargs and the multi-process pieces.

#ifndef _IAVF_ETHDEV_H_
#define _IAVF_ETHDEV_H_

#include "iavf_plat.h"

#include "base/iavf_adminq_cmd.h"
#include "base/iavf_prototype.h"
#include "base/iavf_type.h"

#define IAVF_AQ_LEN               32
#define IAVF_AQ_BUF_SZ            4096
#define IAVF_RESET_WAIT_CNT       2000
#define IAVF_RESET_DETECTED_CNT   500
#define IAVF_RESET_POLL_SCALE     4  /* Poll-interval scale for reset detection */
#define IAVF_BUF_SIZE_MIN         1024
#define IAVF_FRAME_SIZE_MAX       9728
#define IAVF_QUEUE_BASE_ADDR_UNIT 128

#define IAVF_MAX_NUM_QUEUES_DFLT	 16
#define IAVF_MAX_NUM_QUEUES_LV		 256
#define IAVF_CFG_Q_NUM_PER_BUF		 32
#define IAVF_IRQ_MAP_NUM_PER_BUF	 128
#define IAVF_RXTX_QUEUE_CHUNKS_NUM	 2

/* DPDK keeps 32768 unicast addresses; one VF here needs its own and a few more. */
#define IAVF_UC_MACADDR_MAX      16
#define IAVF_MC_MACADDR_MAX      64

#define IAVF_DEFAULT_RX_PTHRESH      8
#define IAVF_DEFAULT_RX_HTHRESH      8
#define IAVF_DEFAULT_RX_WTHRESH      0

#define IAVF_DEFAULT_RX_FREE_THRESH  32

#define IAVF_DEFAULT_TX_PTHRESH      32
#define IAVF_DEFAULT_TX_HTHRESH      0
#define IAVF_DEFAULT_TX_WTHRESH      0

#define IAVF_DEFAULT_TX_FREE_THRESH  32
#define IAVF_DEFAULT_TX_RS_THRESH 32

#define IAVF_BASIC_OFFLOAD_CAPS  ( \
	VF_BASE_MODE_OFFLOADS | \
	VIRTCHNL_VF_OFFLOAD_WB_ON_ITR | \
	VIRTCHNL_VF_OFFLOAD_RX_POLLING)

#define IAVF_RSS_OFFLOAD_ALL ( \
	RTE_ETH_RSS_IPV4 | \
	RTE_ETH_RSS_FRAG_IPV4 |         \
	RTE_ETH_RSS_NONFRAG_IPV4_TCP |  \
	RTE_ETH_RSS_NONFRAG_IPV4_UDP |  \
	RTE_ETH_RSS_NONFRAG_IPV4_SCTP | \
	RTE_ETH_RSS_NONFRAG_IPV4_OTHER | \
	RTE_ETH_RSS_IPV6 | \
	RTE_ETH_RSS_FRAG_IPV6 | \
	RTE_ETH_RSS_NONFRAG_IPV6_TCP | \
	RTE_ETH_RSS_NONFRAG_IPV6_UDP | \
	RTE_ETH_RSS_NONFRAG_IPV6_SCTP | \
	RTE_ETH_RSS_NONFRAG_IPV6_OTHER)

/* MSI-X vector 0 carries the mailbox and PF events; queue vectors start at 1. */
#define IAVF_MISC_VEC_ID                0
#define IAVF_RX_VEC_START               1

/* Default queue interrupt throttling time in microseconds */
#define IAVF_ITR_INDEX_DEFAULT          0
#define IAVF_QUEUE_ITR_INTERVAL_DEFAULT 32 /* 32 us */
#define IAVF_QUEUE_ITR_INTERVAL_MAX     8160 /* 8160 us */

/* The overhead from MTU to max frame size.
 * Considering QinQ packet, the VLAN tag needs to be counted twice.
 */
#define IAVF_ETH_OVERHEAD \
	(RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN + RTE_VLAN_HLEN * 2)
#define IAVF_ETH_MAX_LEN (RTE_ETHER_MTU + IAVF_ETH_OVERHEAD)

#define IAVF_32_BIT_WIDTH (CHAR_BIT * 4)
#define IAVF_48_BIT_WIDTH (CHAR_BIT * 6)
#define IAVF_48_BIT_MASK  RTE_LEN2MASK(IAVF_48_BIT_WIDTH, uint64_t)

#define IAVF_RX_DESC_EXT_STATUS_FLEXBH_MASK  0x03
#define IAVF_RX_DESC_EXT_STATUS_FLEXBH_FD_ID 0x01

#define IAVF_BITS_PER_BYTE 8

struct iavf_adapter;
struct ci_rx_queue;
struct ci_tx_queue;

struct iavf_mbuf_stats {
	uint64_t tx_pkt_errors;
};

struct iavf_eth_xstats {
	struct virtchnl_eth_stats eth_stats;
	struct iavf_mbuf_stats mbuf_stats;
};

/* Structure that defines a VSI, associated with a adapter. */
struct iavf_vsi {
	struct iavf_adapter *adapter; /* Backreference to associated adapter */
	uint16_t vsi_id;
	uint16_t nb_qps;         /* Number of queue pairs VSI can occupy */
	uint16_t nb_used_qps;    /* Number of queue pairs VSI uses */
	uint16_t max_macaddrs;   /* Maximum number of MAC addresses */
	uint16_t base_vector;
	uint16_t msix_intr;      /* The MSIX interrupt binds to VSI */
	struct iavf_eth_xstats eth_stats_offset;
};

struct iavf_fdir_conf {
	struct virtchnl_fdir_add add_fltr;
	struct virtchnl_fdir_del del_fltr;
	uint64_t input_set;
	uint32_t flow_id;
	uint32_t mark_flag;
};

struct iavf_qv_map {
	uint16_t queue_id;
	uint16_t vector_id;
};

/* Message type read in admin queue from PF */
enum iavf_aq_result {
	IAVF_MSG_ERR = -1, /* Meet error when accessing admin queue */
	IAVF_MSG_NON,      /* Read nothing from admin queue */
	IAVF_MSG_SYS,      /* Read system msg from admin queue */
	IAVF_MSG_CMD,      /* Read async command result */
};

/* Structure to store private data specific for VF instance. */
struct iavf_info {
	uint16_t num_queue_pairs;
	uint16_t max_pkt_len; /* Maximum packet length */
	uint16_t mac_num;     /* Number of MAC addresses */
	bool promisc_unicast_enabled;
	bool promisc_multicast_enabled;

	struct virtchnl_version_info virtchnl_version;
	struct virtchnl_vf_resource *vf_res; /* VF resource */
	struct virtchnl_vsi_resource *vsi_res; /* LAN VSI */
	uint64_t supported_rxdid;
	volatile RTE_ATOMIC(enum virtchnl_ops) pend_cmd; /* pending command not finished */
	RTE_ATOMIC(uint32_t) pend_cmd_count;
	int cmd_retval; /* return value of the cmd response from PF */
	uint8_t aq_resp[IAVF_AQ_BUF_SZ]; /* buffer to store the adminq response from PF */
	bool aq_intr_enabled;

	/* Event from pf */
	bool dev_closed;
	bool link_up;
	uint32_t link_speed;

	/* Unicast addrs */
	struct rte_ether_addr mac_addrs[IAVF_UC_MACADDR_MAX];

	struct iavf_vsi vsi;
	bool vf_reset;	/* true for VF reset pending, false for no VF reset */
	uint64_t flags;

	uint8_t *rss_lut;
	uint8_t *rss_key;
	uint64_t rss_hf;
	uint16_t nb_msix;   /* number of MSI-X interrupts on Rx */
	uint16_t msix_base; /* msix vector base from */
	uint16_t max_rss_qregion; /* max RSS queue region supported by PF */
	struct iavf_qv_map *qv_map; /* queue vector mapping */
	rte_spinlock_t aq_lock;

	/* indicate large VF support enabled or not */
	bool lv_enabled;

	struct rte_eth_dev *eth_dev;

	RTE_ATOMIC(bool) in_reset_recovery;
	bool reset_pending;
	bool pf_reset_in_progress;
	bool start_pending;

	uint32_t ptp_caps;
};

#define IAVF_MAX_PKT_TYPE 1024

#define IAVF_MAX_QUEUE_NUM  2048

enum iavf_rx_func_type {
	IAVF_RX_DISABLED,
	IAVF_RX_DEFAULT,
	IAVF_RX_SCATTERED,
	IAVF_RX_FLEX_RXD,
	IAVF_RX_SCATTERED_FLEX_RXD,
	IAVF_RX_BULK_ALLOC,
	IAVF_RX_BULK_ALLOC_FLEX_RXD,
};

enum iavf_tx_func_type {
	IAVF_TX_DISABLED,
	IAVF_TX_DEFAULT,
};

/* Structure to store private data for each VF instance. */
struct iavf_adapter {
	struct iavf_hw hw;
	struct rte_eth_dev_data *dev_data;
	struct iavf_info vf;

	bool rx_bulk_alloc_allowed;
	alignas(RTE_CACHE_LINE_SIZE) uint32_t ptype_tbl[IAVF_MAX_PKT_TYPE];
	bool stopped;
	bool closed;
	RTE_ATOMIC(bool) no_poll;
	enum iavf_rx_func_type rx_func_type;
	enum iavf_tx_func_type tx_func_type;
	uint16_t fdir_ref_cnt;
	bool mac_primary_set;
	uint16_t tpid;  /* VLAN tag identifier */
	/* miniosv: what DPDK keeps in rte_eth_dev_data and minidpdk does not. */
	uint16_t mtu;
	bool scattered_rx;
};

/* IAVF_DEV_PRIVATE_TO */
#define IAVF_DEV_PRIVATE_TO_ADAPTER(adapter) \
	((struct iavf_adapter *)adapter)
#define IAVF_DEV_PRIVATE_TO_VF(adapter) \
	(&((struct iavf_adapter *)adapter)->vf)
#define IAVF_DEV_PRIVATE_TO_HW(adapter) \
	(&((struct iavf_adapter *)adapter)->hw)

/* IAVF_VSI_TO */
#define IAVF_VSI_TO_HW(vsi) \
	(&(((struct iavf_vsi *)vsi)->adapter->hw))
#define IAVF_VSI_TO_VF(vsi) \
	(&(((struct iavf_vsi *)vsi)->adapter->vf))

static inline void
iavf_init_adminq_parameter(struct iavf_hw *hw)
{
	hw->aq.num_arq_entries = IAVF_AQ_LEN;
	hw->aq.num_asq_entries = IAVF_AQ_LEN;
	hw->aq.arq_buf_size = IAVF_AQ_BUF_SZ;
	hw->aq.asq_buf_size = IAVF_AQ_BUF_SZ;
}

static inline uint16_t
iavf_calc_itr_interval(int16_t interval)
{
	if (interval < 0 || interval > IAVF_QUEUE_ITR_INTERVAL_MAX)
		interval = IAVF_QUEUE_ITR_INTERVAL_DEFAULT;

	/* Convert to hardware count, as writing each 1 represents 2 us */
	return interval / 2;
}

/* structure used for sending and checking response of virtchnl ops */
struct iavf_cmd_info {
	enum virtchnl_ops ops;
	uint8_t *in_args;       /* buffer for sending */
	uint32_t in_args_size;  /* buffer size for sending */
	uint8_t *out_buffer;    /* buffer for response */
	uint32_t out_size;      /* buffer size for response */
};

int iavf_check_api_version(struct iavf_adapter *adapter);
int iavf_get_vf_resource(struct iavf_adapter *adapter);
void iavf_handle_virtchnl_msg(struct rte_eth_dev *dev);
int iavf_switch_queue(struct iavf_adapter *adapter, uint16_t qid,
		     bool rx, bool on);
int iavf_switch_queue_lv(struct iavf_adapter *adapter, uint16_t qid,
		     bool rx, bool on);
int iavf_enable_queues(struct iavf_adapter *adapter);
int iavf_enable_queues_lv(struct iavf_adapter *adapter);
int iavf_disable_queues(struct iavf_adapter *adapter);
int iavf_disable_queues_lv(struct iavf_adapter *adapter);
int iavf_configure_rss_lut(struct iavf_adapter *adapter);
int iavf_configure_rss_key(struct iavf_adapter *adapter);
int iavf_configure_queues(struct iavf_adapter *adapter, uint16_t num_queue_pairs);
int iavf_get_supported_rxdid(struct iavf_adapter *adapter);
int iavf_config_irq_map(struct iavf_adapter *adapter);
int iavf_config_irq_map_lv(struct iavf_adapter *adapter, uint16_t num);
void iavf_add_del_secondary_mac_addr(struct iavf_adapter *adapter, bool add);
int iavf_dev_link_update(struct rte_eth_dev *dev, int wait_to_complete);
int iavf_query_stats(struct iavf_adapter *adapter,
		    struct virtchnl_eth_stats *pstats);
int iavf_config_promisc(struct iavf_adapter *adapter, bool enable_unicast,
		       bool enable_multicast);
int iavf_add_del_eth_addr(struct iavf_adapter *adapter,
			 struct rte_ether_addr *addr, bool add, uint8_t type);
int iavf_fdir_add(struct iavf_adapter *adapter, struct iavf_fdir_conf *filter);
int iavf_fdir_del(struct iavf_adapter *adapter, struct iavf_fdir_conf *filter);
int iavf_fdir_check(struct iavf_adapter *adapter,
		struct iavf_fdir_conf *filter);
int iavf_add_del_rss_cfg(struct iavf_adapter *adapter,
			 struct virtchnl_rss_cfg *rss_cfg, bool add);
int iavf_get_hena_caps(struct iavf_adapter *adapter, uint64_t *caps);
int iavf_set_hena(struct iavf_adapter *adapter, uint64_t hena);
int iavf_request_queues(struct rte_eth_dev *dev, uint16_t num);
int iavf_get_max_rss_queue_region(struct iavf_adapter *adapter);
void iavf_set_no_poll(struct iavf_adapter *adapter, bool link_change);

#endif /* _IAVF_ETHDEV_H_ */
