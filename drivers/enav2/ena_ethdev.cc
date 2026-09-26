/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2015-2020 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 */

#include "ena_ethdev.h"
#include "base/ena_com.h"
#include "base/ena_defs/ena_includes.h"
#include "base/ena_eth_com.h"
#include "ena_if.h"
#include "osv/aligned_new.hh"
#include "osv/msi.hh"
#include "osv/sched.hh"
#include "osv/version.hh"

#include "base/ena_plat.h"
#include <api/minidpdk/bit.hh>
#include <api/minidpdk/defs.hh>
#include <api/minidpdk/dev.hh>
#include <api/minidpdk/mem.hh>
#include <api/minidpdk/net.hh>
#include <api/minidpdk/rss.hh>
#include <api/minidpdk/time.hh>
#include <api/minidpdk/util.hh>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <drivers/pci-device.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/phys.hh>
#include <osv/trace.hh>

#define DRV_MODULE_VER_MAJOR 2
#define DRV_MODULE_VER_MINOR 11
#define DRV_MODULE_VER_SUBMINOR 0

#define __MERGE_64B_H_L(h, l) (((uint64_t)h << 32) | l)

#define ETH_GSTRING_LEN 32

#define ARRAY_SIZE(x) RTE_DIM(x)

#define ENA_MIN_RING_DESC 128

#define USEC_PER_MSEC 1000UL

#define BITS_PER_BYTE 8

#define BITS_PER_TYPE(type) (sizeof(type) * BITS_PER_BYTE)

#define DECIMAL_BASE 10

#define MAX_WIDE_LLQ_DEPTH_UNSUPPORTED 0

/*
 * We should try to keep ENA_CLEANUP_BUF_THRESH lower than
 * RTE_MEMPOOL_CACHE_MAX_SIZE, so we can fit this in mempool local cache.
 */
#define ENA_CLEANUP_BUF_THRESH 256

struct ena_stats {
  char name[ETH_GSTRING_LEN];
  int stat_offset;
};

#define ENA_STAT_ENTRY(stat, stat_type)                                        \
  {.name = #stat, .stat_offset = offsetof(struct ena_stats_##stat_type, stat)}

#define ENA_STAT_RX_ENTRY(stat) ENA_STAT_ENTRY(stat, rx)

#define ENA_STAT_TX_ENTRY(stat) ENA_STAT_ENTRY(stat, tx)

#define ENA_STAT_METRICS_ENTRY(stat) ENA_STAT_ENTRY(stat, metrics)

#define ENA_STAT_GLOBAL_ENTRY(stat) ENA_STAT_ENTRY(stat, dev)

#define ENA_STAT_ENA_SRD_ENTRY(stat) ENA_STAT_ENTRY(stat, srd)
/*
 * Each rte_memzone should have unique name.
 * To satisfy it, count number of allocation and add it to name.
 */
rte_atomic64_t ena_alloc_cnt;

static const struct ena_stats ena_stats_global_strings[]
    __attribute__((unused)) = {
        ENA_STAT_GLOBAL_ENTRY(wd_expired),
        ENA_STAT_GLOBAL_ENTRY(dev_start),
        ENA_STAT_GLOBAL_ENTRY(dev_stop),
        ENA_STAT_GLOBAL_ENTRY(tx_drops),
};

/*
 * The legacy metrics (also known as eni stats) consisted of 5 stats, while the
 * reworked metrics (also known as customer metrics) support an additional stat.
 */
static struct ena_stats ena_stats_metrics_strings[] __attribute__((unused)) = {
    ENA_STAT_METRICS_ENTRY(bw_in_allowance_exceeded),
    ENA_STAT_METRICS_ENTRY(bw_out_allowance_exceeded),
    ENA_STAT_METRICS_ENTRY(pps_allowance_exceeded),
    ENA_STAT_METRICS_ENTRY(conntrack_allowance_exceeded),
    ENA_STAT_METRICS_ENTRY(linklocal_allowance_exceeded),
    ENA_STAT_METRICS_ENTRY(conntrack_allowance_available),
};

static const struct ena_stats ena_stats_srd_strings[]
    __attribute__((unused)) = {
        ENA_STAT_ENA_SRD_ENTRY(ena_srd_mode),
        ENA_STAT_ENA_SRD_ENTRY(ena_srd_tx_pkts),
        ENA_STAT_ENA_SRD_ENTRY(ena_srd_eligible_tx_pkts),
        ENA_STAT_ENA_SRD_ENTRY(ena_srd_rx_pkts),
        ENA_STAT_ENA_SRD_ENTRY(ena_srd_resource_utilization),
};

static const struct ena_stats ena_stats_tx_strings[] __attribute__((unused)) = {
    ENA_STAT_TX_ENTRY(cnt),
    ENA_STAT_TX_ENTRY(bytes),
    ENA_STAT_TX_ENTRY(prepare_ctx_err),
    ENA_STAT_TX_ENTRY(tx_poll),
    ENA_STAT_TX_ENTRY(doorbells),
    ENA_STAT_TX_ENTRY(bad_req_id),
    ENA_STAT_TX_ENTRY(available_desc),
    ENA_STAT_TX_ENTRY(missed_tx),
};

static const struct ena_stats ena_stats_rx_strings[] __attribute__((unused)) = {
    ENA_STAT_RX_ENTRY(cnt),
    ENA_STAT_RX_ENTRY(bytes),
    ENA_STAT_RX_ENTRY(refill_partial),
    ENA_STAT_RX_ENTRY(l3_csum_bad),
    ENA_STAT_RX_ENTRY(l4_csum_bad),
    ENA_STAT_RX_ENTRY(l4_csum_good),
    ENA_STAT_RX_ENTRY(mbuf_alloc_fail),
    ENA_STAT_RX_ENTRY(bad_desc_num),
    ENA_STAT_RX_ENTRY(bad_req_id),
    ENA_STAT_RX_ENTRY(bad_desc),
    ENA_STAT_RX_ENTRY(unknown_error),
};

#define ENA_STATS_ARRAY_GLOBAL ARRAY_SIZE(ena_stats_global_strings)
#define ENA_STATS_ARRAY_METRICS ARRAY_SIZE(ena_stats_metrics_strings)
#define ENA_STATS_ARRAY_METRICS_LEGACY (ENA_STATS_ARRAY_METRICS - 1)
#define ENA_STATS_ARRAY_ENA_SRD ARRAY_SIZE(ena_stats_srd_strings)
#define ENA_STATS_ARRAY_TX ARRAY_SIZE(ena_stats_tx_strings)
#define ENA_STATS_ARRAY_RX ARRAY_SIZE(ena_stats_rx_strings)

#define QUEUE_OFFLOADS                                                         \
  (RTE_ETH_TX_OFFLOAD_TCP_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM |               \
   RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_TSO)
#define MBUF_OFFLOADS                                                          \
  (RTE_MBUF_F_TX_L4_MASK | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_TCP_SEG)

/** Vendor ID used by Amazon devices */
#define PCI_VENDOR_ID_AMAZON 0x1D0F
/** Amazon devices */
#define PCI_DEVICE_ID_ENA_VF 0xEC20
#define PCI_DEVICE_ID_ENA_VF_RSERV0 0xEC21

#define ENA_TX_OFFLOAD_MASK                                                    \
  (RTE_MBUF_F_TX_L4_MASK | RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_IPV4 |           \
   RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_TCP_SEG)

#define ENA_TX_OFFLOAD_NOTSUP_MASK                                             \
  (RTE_MBUF_F_TX_OFFLOAD_MASK ^ ENA_TX_OFFLOAD_MASK)

/** HW specific offloads capabilities. */
/* IPv4 checksum offload. */
#define ENA_L3_IPV4_CSUM 0x0001
/* TCP/UDP checksum offload for IPv4 packets. */
#define ENA_L4_IPV4_CSUM 0x0002
/* TCP/UDP checksum offload for IPv4 packets with pseudo header checksum. */
#define ENA_L4_IPV4_CSUM_PARTIAL 0x0004
/* TCP/UDP checksum offload for IPv6 packets. */
#define ENA_L4_IPV6_CSUM 0x0008
/* TCP/UDP checksum offload for IPv6 packets with pseudo header checksum. */
#define ENA_L4_IPV6_CSUM_PARTIAL 0x0010
/* TSO support for IPv4 packets. */
#define ENA_IPV4_TSO 0x0020

/* Device supports setting RSS hash. */
#define ENA_RX_RSS_HASH 0x0040

int ena_log_level = ENA_WARN;

static ena_vendor_info_t ena_vendor_info_array[] = {
    {PCI_VENDOR_ID_AMAZON, PCI_DEVICE_ID_ENA_VF, 0},
    {PCI_VENDOR_ID_AMAZON, PCI_DEVICE_ID_ENA_VF_RSERV0, 0},
    /* Last entry */
    {0, 0, 0}};
static ena_aenq_handlers aenq_handlers;

static void ena_rx_cleanup(ena_ring *rxq, rte_eth_dev *dev,
                           irq_handler_cb_t handler);
static void ena_rx_queue_intr_set(ena_ring *rx_ring, bool unmask,
                                  u32 rx_delay = 0);
static uint16_t ena_rx_burst(rte_eth_dev *dev, uint16_t qid, rte_mbuf **rx_pkts,
                             uint16_t nb_pkts);
static int ena_request_io_irq(struct ena_adapter *adapter);
static int ena_dev_configure(rte_eth_dev *dev);
static int ena_device_init(struct ena_adapter *adapter, pci::device *pdev,
                           struct ena_com_dev_get_features_ctx *get_feat_ctx);
static void ena_tx_map_mbuf(struct ena_ring *tx_ring,
                            struct ena_tx_buffer *tx_info, rte_mbuf *mbuf,
                            void **push_header, uint16_t *header_len);
static int ena_xmit_mbuf(struct ena_ring *tx_ring, rte_mbuf *mbuf);
static int ena_tx_cleanup(void *txp, uint32_t free_pkt_cnt);

static inline void ena_init_rx_mbuf(rte_mbuf *mbuf, uint16_t len);
static rte_mbuf *ena_rx_mbuf(struct ena_ring *rx_ring,
                             struct ena_com_rx_buf_info *ena_bufs,
                             uint32_t descs, uint16_t *next_to_clean,
                             uint8_t offset);

static int ena_add_single_rx_desc(struct ena_com_io_sq *io_sq, rte_mbuf *mbuf,
                                  uint16_t id);
static int ena_populate_rx_queue(struct ena_ring *rxq, unsigned int count);
static void ena_init_rings(struct ena_adapter *adapter,
                           bool disable_meta_caching);

static void ena_rx_queue_release_all(rte_eth_dev *dev);
static void ena_tx_queue_release_all(rte_eth_dev *dev);
static void ena_rx_queue_release(rte_eth_dev *dev, uint16_t qid);
static void ena_tx_queue_release(rte_eth_dev *dev, uint16_t qid);
static void ena_rx_queue_release_bufs(ena_ring *ring);
static void ena_tx_queue_release_bufs(ena_ring *ring);

static int ena_create_io_queue(struct rte_eth_dev *dev, struct ena_ring *ring);
static void ena_queue_stop(struct ena_ring *ring);
static void ena_queue_stop_all(struct rte_eth_dev *dev,
                               enum ena_ring_type ring_type);
static void ena_collect_intr_threads(struct ena_adapter *adapter);
static int ena_queue_start(struct rte_eth_dev *dev, struct ena_ring *ring);
static int ena_queue_start_all(struct rte_eth_dev *dev,
                               enum ena_ring_type ring_type);

static void ena_stats_restart(struct rte_eth_dev *dev);

static uint64_t ena_get_rx_port_offloads(struct ena_adapter *adapter);
static uint64_t ena_get_tx_port_offloads(struct ena_adapter *adapter);
static uint64_t ena_get_rx_queue_offloads(struct ena_adapter *adapter);
static uint64_t ena_get_tx_queue_offloads(struct ena_adapter *adapter);

static int ena_infos_get(struct rte_eth_dev *dev,
                         struct rte_eth_dev_info *dev_info);

static void ena_timer_wd_callback(void *arg);

static void ena_update_hints(struct ena_adapter *adapter,
                             struct ena_admin_ena_hw_hints *hints);

[[maybe_unused]] static void
ena_copy_customer_metrics(struct ena_adapter *adapter, uint64_t *buf,
                          size_t buf_size);

static bool ena_use_large_llq_hdr(struct ena_adapter *adapter,
                                  uint8_t recommended_entry_size);

static int ena_enable_msix_and_set_admin_interrupts(ena_adapter *adapter);

static void ena_free_irqs(ena_adapter *adapter);

/******************************************************************************
 ******************************** AENQ Handlers *******************************
 *****************************************************************************/
static void ena_notification(void *data, struct ena_admin_aenq_entry *aenq_e) {
  auto *adapter = static_cast<ena_adapter *>(data);
  ena_admin_ena_hw_hints *hints;

  if (aenq_e->aenq_common_desc.group != ENA_ADMIN_NOTIFICATION)
    ena_log_raw(WARN, "Invalid AENQ group: %x. Expected: %x",
                aenq_e->aenq_common_desc.group, ENA_ADMIN_NOTIFICATION);

  switch (aenq_e->aenq_common_desc.syndrome) {
  case ENA_ADMIN_UPDATE_HINTS:
    hints = (struct ena_admin_ena_hw_hints *)(&aenq_e->inline_data_w4);
    ena_update_hints(adapter, hints);
    break;
  default:
    ena_log_raw(ERR, "Invalid AENQ notification link state: %d",
                aenq_e->aenq_common_desc.syndrome);
  }
}

static void ena_keep_alive(void *data,
                           __rte_unused struct ena_admin_aenq_entry *aenq_e) {
  auto *adapter = static_cast<ena_adapter *>(data);
  ena_admin_aenq_keep_alive_desc *desc;
  uint64_t rx_drops;
  uint64_t tx_drops;
  uint64_t rx_overruns;

  adapter->timestamp_wd = rte_get_timer_cycles();

  desc = (struct ena_admin_aenq_keep_alive_desc *)aenq_e;
  rx_drops = ((uint64_t)desc->rx_drops_high << 32) | desc->rx_drops_low;
  tx_drops = ((uint64_t)desc->tx_drops_high << 32) | desc->tx_drops_low;
  rx_overruns =
      ((uint64_t)desc->rx_overruns_high << 32) | desc->rx_overruns_low;

  adapter->drv_stats->rx_drops = rx_drops + rx_overruns;
  adapter->dev_stats.tx_drops = tx_drops;
}

static void ena_suboptimal_configuration(__rte_unused void *adapter_data,
                                         struct ena_admin_aenq_entry *aenq_e) {
  struct ena_admin_aenq_conf_notifications_desc *desc;
  int bit, num_bits;

  desc = (struct ena_admin_aenq_conf_notifications_desc *)aenq_e;
  num_bits = BITS_PER_TYPE(desc->notifications_bitmap);
  for (bit = 0; bit < num_bits; bit++) {
    if (desc->notifications_bitmap & RTE_BIT64(bit)) {
      ena_log_raw(WARN, "Sub-optimal configuration notification code: %d",
                  bit + 1);
    }
  }
}

static void ena_update_on_link_change(void *data,
                                      struct ena_admin_aenq_entry *aenq_e) {
  auto *adapter = static_cast<ena_adapter *>(data);
  struct ena_admin_aenq_link_change_desc *aenq_desc;
  int status;

  aenq_desc = (struct ena_admin_aenq_link_change_desc *)aenq_e;
  status = aenq_desc->flags & ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK;

  if (status != 0) {
    ena_log(adapter->pdev, DBG, "link is UP");
    adapter->link_status = 1;
  } else {
    ena_log(adapter->pdev, DBG, "link is DOWN");
    adapter->link_status = 0;
  }
}

/**
 * This handler will called for unknown event group or unimplemented handlers
 **/
static void
unimplemented_aenq_handler(__rte_unused void *data,
                           __rte_unused struct ena_admin_aenq_entry *aenq_e) {
  ena_log_raw(ERR,
              "Unknown event was received or event with unimplemented handler");
}

static inline void ena_trigger_reset(struct ena_adapter *adapter,
                                     enum ena_regs_reset_reason_types reason) {
  if (likely(!adapter->trigger_reset)) {
    adapter->reset_reason = reason;
    adapter->trigger_reset = true;
  }
}

static inline void ena_rx_mbuf_prepare(struct ena_ring *rx_ring, rte_mbuf *mbuf,
                                       struct ena_com_rx_ctx *ena_rx_ctx) {
  struct ena_stats_rx *rx_stats = &rx_ring->rx_stats;
  uint64_t ol_flags = 0;
  uint32_t packet_type = 0;

  switch (ena_rx_ctx->l3_proto) {
  case ENA_ETH_IO_L3_PROTO_IPV4:
    packet_type |= RTE_PTYPE_L3_IPV4;
    if (unlikely(ena_rx_ctx->l3_csum_err)) {
      ++rx_stats->l3_csum_bad;
      ol_flags |= RTE_MBUF_F_RX_IP_CKSUM_BAD;
    } else {
      ol_flags |= RTE_MBUF_F_RX_IP_CKSUM_GOOD;
    }
    break;
  case ENA_ETH_IO_L3_PROTO_IPV6:
    packet_type |= RTE_PTYPE_L3_IPV6;
    break;
  default:
    break;
  }

  switch (ena_rx_ctx->l4_proto) {
  case ENA_ETH_IO_L4_PROTO_TCP:
    packet_type |= RTE_PTYPE_L4_TCP;
    break;
  case ENA_ETH_IO_L4_PROTO_UDP:
    packet_type |= RTE_PTYPE_L4_UDP;
    break;
  default:
    break;
  }

  /* L4 csum is relevant only for TCP/UDP packets */
  if ((packet_type & (RTE_PTYPE_L4_UDP | RTE_PTYPE_L4_TCP)) &&
      !ena_rx_ctx->frag) {
    if (ena_rx_ctx->l4_csum_checked) {
      if (likely(!ena_rx_ctx->l4_csum_err)) {
        ++rx_stats->l4_csum_good;
        ol_flags |= RTE_MBUF_F_RX_L4_CKSUM_GOOD;
      } else {
        ++rx_stats->l4_csum_bad;
        ol_flags |= RTE_MBUF_F_RX_L4_CKSUM_BAD;
      }
    } else {
      ol_flags |= RTE_MBUF_F_RX_L4_CKSUM_UNKNOWN;
    }

    if (rx_ring->offloads & RTE_ETH_RX_OFFLOAD_RSS_HASH) {
      ol_flags |= RTE_MBUF_F_RX_RSS_HASH;
      mbuf->hash.rss = ena_rx_ctx->hash;
    }
  } else {
    ol_flags |= RTE_MBUF_F_RX_L4_CKSUM_UNKNOWN;
  }

  mbuf->ol_flags = ol_flags;
  mbuf->packet_type = packet_type;
}

static inline void ena_tx_mbuf_prepare(rte_mbuf *mbuf,
                                       struct ena_com_tx_ctx *ena_tx_ctx,
                                       uint64_t queue_offloads,
                                       bool disable_meta_caching) {
  struct ena_com_tx_meta *ena_meta = &ena_tx_ctx->ena_meta;

  if ((mbuf->ol_flags & MBUF_OFFLOADS) && (queue_offloads & QUEUE_OFFLOADS)) {

    /* check if L3 checksum is needed */
    if ((mbuf->ol_flags & RTE_MBUF_F_TX_IP_CKSUM) &&
        (queue_offloads & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM))
      ena_tx_ctx->l3_csum_enable = true;

    if (mbuf->ol_flags & RTE_MBUF_F_TX_IPV6) {
      ena_tx_ctx->l3_proto = ENA_ETH_IO_L3_PROTO_IPV6;
      /* For the IPv6 packets, DF always needs to be true. */
      ena_tx_ctx->df = 1;
    } else {
      ena_tx_ctx->l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;

      /* set don't fragment (DF) flag */
      if (mbuf->packet_type &
          (RTE_PTYPE_L4_NONFRAG | RTE_PTYPE_INNER_L4_NONFRAG))
        ena_tx_ctx->df = 1;
    }

    const uint64_t l4 = mbuf->ol_flags & RTE_MBUF_F_TX_L4_MASK;
    if (l4 == RTE_MBUF_F_TX_UDP_CKSUM &&
        (queue_offloads & RTE_ETH_TX_OFFLOAD_UDP_CKSUM)) {
      ena_tx_ctx->l4_proto = ENA_ETH_IO_L4_PROTO_UDP;
      ena_tx_ctx->l4_csum_enable = true;
    } else if (l4 == RTE_MBUF_F_TX_TCP_CKSUM &&
               (queue_offloads & RTE_ETH_TX_OFFLOAD_TCP_CKSUM)) {
      ena_tx_ctx->l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
      ena_tx_ctx->l4_csum_enable = true;
    } else {
      ena_tx_ctx->l4_proto = ENA_ETH_IO_L4_PROTO_UNKNOWN;
      ena_tx_ctx->l4_csum_enable = false;
    }

    ena_meta->mss = 0;
    ena_meta->l3_hdr_len = mbuf->l3_len;
    ena_meta->l3_hdr_offset = mbuf->l2_len;

    ena_tx_ctx->meta_valid = true;
  } else if (disable_meta_caching) {
    memset(ena_meta, 0, sizeof(*ena_meta));
    ena_tx_ctx->meta_valid = true;
  } else {
    ena_tx_ctx->meta_valid = false;
  }
}

static int validate_tx_req_id(ena_ring *tx_ring, u16 req_id) {
  struct ena_tx_buffer *tx_info = NULL;

  if (likely(req_id < tx_ring->ring_size)) {
    tx_info = &tx_ring->tx_buffer_info[req_id];
    if (likely(tx_info->mbuf))
      return 0;
  }

  if (tx_info)
    ena_log_raw(ERR, "tx_info doesn't have valid mbuf. queue %d:%d req_id %u",
                tx_ring->port_id, tx_ring->id, req_id);
  else
    ena_log_raw(ERR, "Invalid req_id: %hu in queue %d:%d", req_id,
                tx_ring->port_id, tx_ring->id);

  /* Trigger device reset */
  ++tx_ring->tx_stats.bad_req_id;
  ena_trigger_reset(tx_ring->adapter, ENA_REGS_RESET_INV_TX_REQ_ID);
  return -EFAULT;
}

static void ena_config_host_info(struct ena_com_dev *ena_dev) {
  struct ena_admin_host_info *host_info;
  int rc;

  /* Allocate only the host info */
  rc = ena_com_allocate_host_info(ena_dev);
  if (rc) {
    ena_log_raw(ERR, "Cannot allocate host info");
    return;
  }

  host_info = ena_dev->host_attr.host_info;
  host_info->os_type = ENA_ADMIN_OS_DPDK;
  host_info->kernel_ver = 1;
  strlcpy((char *)host_info->kernel_ver_str, osv::version().c_str(),
          sizeof(host_info->kernel_ver_str));
  strlcpy((char *)host_info->os_dist_str, osv::version().c_str(),
          sizeof(host_info->os_dist_str));
  host_info->driver_version =
      (DRV_MODULE_VER_MAJOR) |
      (DRV_MODULE_VER_MINOR << ENA_ADMIN_HOST_INFO_MINOR_SHIFT) |
      (DRV_MODULE_VER_SUBMINOR << ENA_ADMIN_HOST_INFO_SUB_MINOR_SHIFT);
  host_info->num_cpus = 1;

  host_info->driver_supported_features =
      ENA_ADMIN_HOST_INFO_RX_OFFSET_MASK |
      ENA_ADMIN_HOST_INFO_RSS_CONFIGURABLE_FUNCTION_KEY_MASK;

  rc = ena_com_set_host_attributes(ena_dev);
  if (rc) {
    if (rc == ENA_COM_UNSUPPORTED)
      ena_log_raw(WARN, "Cannot set host attributes");
    else
      ena_log_raw(ERR, "Cannot set host attributes");

    goto err;
  }
  return;

err:
  ena_com_delete_host_info(ena_dev);
}

/* This function calculates the number of xstats based on the current config */
static unsigned int ena_xstats_calc_num(rte_eth_dev_data *data) {
  ena_adapter *adapter = data->get<ena_adapter>();

  return ENA_STATS_ARRAY_GLOBAL + adapter->metrics_num +
         ENA_STATS_ARRAY_ENA_SRD + (data->nb_tx_queues * ENA_STATS_ARRAY_TX) +
         (data->nb_rx_queues * ENA_STATS_ARRAY_RX);
}

static void ena_config_debug_area(struct ena_adapter *adapter) {
  u32 debug_area_size;
  int rc, ss_count;

  ss_count = ena_xstats_calc_num(&adapter->edev->data);

  /* allocate 32 bytes for each string and 64bit for the value */
  debug_area_size = ss_count * ETH_GSTRING_LEN + sizeof(u64) * ss_count;

  rc = ena_com_allocate_debug_area(&adapter->ena_dev, debug_area_size);
  if (rc) {
    ena_log_raw(ERR, "Cannot allocate debug area");
    return;
  }

  rc = ena_com_set_host_attributes(&adapter->ena_dev);
  if (rc) {
    if (rc == ENA_COM_UNSUPPORTED)
      ena_log_raw(WARN, "Cannot set host attributes");
    else
      ena_log_raw(ERR, "Cannot set host attributes");

    goto err;
  }

  return;
err:
  ena_com_delete_debug_area(&adapter->ena_dev);
}

static void ena_rx_queue_release_all(struct rte_eth_dev *dev) {
  int nb_queues = dev->data.nb_rx_queues;
  int i;

  for (i = 0; i < nb_queues; i++)
    ena_rx_queue_release(dev, i);
}

static void ena_tx_queue_release_all(struct rte_eth_dev *dev) {
  int nb_queues = dev->data.nb_tx_queues;
  int i;

  for (i = 0; i < nb_queues; i++)
    ena_tx_queue_release(dev, i);
}

static void ena_rx_queue_release(struct rte_eth_dev *dev, uint16_t qid) {
  ena_ring *ring = &dev->get<ena_adapter>()->rx_ring[qid];

  /* Free ring resources */
  rte_free(ring->rx_buffer_info);
  ring->rx_buffer_info = NULL;

  rte_free(ring->rx_refill_buffer);
  ring->rx_refill_buffer = NULL;

  rte_free(ring->empty_rx_reqs);
  ring->empty_rx_reqs = NULL;

  ring->configured = 0;

  ena_log_raw(INFO, "Rx queue %d:%d released", ring->port_id, ring->id);
}

static void ena_tx_queue_release(rte_eth_dev *dev, uint16_t qid) {
  ena_ring *ring = &dev->get<ena_adapter>()->tx_ring[qid];

  /* Free ring resources */
  rte_free(ring->push_buf_intermediate_buf);

  rte_free(ring->tx_buffer_info);

  rte_free(ring->empty_tx_reqs);

  ring->empty_tx_reqs = NULL;
  ring->tx_buffer_info = NULL;
  ring->push_buf_intermediate_buf = NULL;

  ring->configured = 0;

  ena_log_raw(INFO, "Tx queue %d:%d released", ring->port_id, ring->id);
}

static void ena_rx_queue_release_bufs(struct ena_ring *ring) {
  unsigned int i;

  for (i = 0; i < ring->ring_size; ++i) {
    struct ena_rx_buffer *rx_info = &ring->rx_buffer_info[i];
    if (rx_info->mbuf) {
      rte_mbuf_raw_free(rx_info->mbuf);
      rx_info->mbuf = NULL;
    }
  }
}

static void ena_tx_queue_release_bufs(struct ena_ring *ring) {
  unsigned int i;

  for (i = 0; i < ring->ring_size; ++i) {
    struct ena_tx_buffer *tx_buf = &ring->tx_buffer_info[i];

    if (tx_buf->mbuf) {
      rte_pktmbuf_free(tx_buf->mbuf);
      tx_buf->mbuf = NULL;
    }
  }
}

static int ena_queue_start_all(rte_eth_dev *dev, enum ena_ring_type ring_type) {
  ena_adapter *adapter = dev->get<ena_adapter>();
  ena_ring *queues = NULL;
  int nb_queues;
  int i = 0;
  int rc = 0;

  if (ring_type == ENA_RING_TYPE_RX) {
    queues = adapter->rx_ring;
    nb_queues = dev->data.nb_rx_queues;
  } else {
    queues = adapter->tx_ring;
    nb_queues = dev->data.nb_tx_queues;
  }
  for (i = 0; i < nb_queues; i++) {
    if (queues[i].configured) {
      rc = ena_queue_start(dev, &queues[i]);
      if (rc) {
        ena_log_raw(ERR, "Failed to start queue[%d] of type(%d)", i, ring_type);
        goto err;
      }
    }
  }
  if (dev->data.dev_conf.intr_conf.rxq)
    ena_request_io_irq(adapter);
  return 0;
err:
  while (i--)
    if (queues[i].configured)
      ena_queue_stop(&queues[i]);

  return rc;
}

static int ena_calc_io_queue_size(ena_calc_queue_size_ctx *ctx,
                                  bool use_large_llq_hdr) {
  struct ena_admin_feature_llq_desc *dev = &ctx->get_feat_ctx->llq;
  struct ena_com_dev *ena_dev = ctx->ena_dev;
  uint32_t max_tx_queue_size;
  uint32_t max_rx_queue_size;

  if (ena_dev->supported_features & BIT(ENA_ADMIN_MAX_QUEUES_EXT)) {
    struct ena_admin_queue_ext_feature_fields *max_queue_ext =
        &ctx->get_feat_ctx->max_queue_ext.max_queue_ext;
    max_rx_queue_size =
        RTE_MIN(max_queue_ext->max_rx_cq_depth, max_queue_ext->max_rx_sq_depth);
    max_tx_queue_size = max_queue_ext->max_tx_cq_depth;

    if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
      max_tx_queue_size = RTE_MIN(max_tx_queue_size, dev->max_llq_depth);
    } else {
      max_tx_queue_size =
          RTE_MIN(max_tx_queue_size, max_queue_ext->max_tx_sq_depth);
    }

    ctx->max_rx_sgl_size =
        RTE_MIN(ENA_PKT_MAX_BUFS, max_queue_ext->max_per_packet_rx_descs);
    ctx->max_tx_sgl_size =
        RTE_MIN(ENA_PKT_MAX_BUFS, max_queue_ext->max_per_packet_tx_descs);
  } else {
    struct ena_admin_queue_feature_desc *max_queues =
        &ctx->get_feat_ctx->max_queues;
    max_rx_queue_size =
        RTE_MIN(max_queues->max_cq_depth, max_queues->max_sq_depth);
    max_tx_queue_size = max_queues->max_cq_depth;

    if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
      max_tx_queue_size = RTE_MIN(max_tx_queue_size, dev->max_llq_depth);
    } else {
      max_tx_queue_size = RTE_MIN(max_tx_queue_size, max_queues->max_sq_depth);
    }

    ctx->max_rx_sgl_size =
        RTE_MIN(ENA_PKT_MAX_BUFS, max_queues->max_packet_rx_descs);
    ctx->max_tx_sgl_size =
        RTE_MIN(ENA_PKT_MAX_BUFS, max_queues->max_packet_tx_descs);
  }

  /* Round down to the nearest power of 2 */
  max_rx_queue_size = rte_align32prevpow2(max_rx_queue_size);
  max_tx_queue_size = rte_align32prevpow2(max_tx_queue_size);

  if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV &&
      use_large_llq_hdr) {
    /* intersection between driver configuration and device capabilities */
    if (dev->entry_size_ctrl_supported & ENA_ADMIN_LIST_ENTRY_SIZE_256B) {
      if (dev->max_wide_llq_depth == MAX_WIDE_LLQ_DEPTH_UNSUPPORTED) {
        /* Devices that do not support the double-sized ENA memory BAR will
         * report max_wide_llq_depth as 0. In such case, driver halves the
         * queue depth when working in large llq policy.
         */
        max_tx_queue_size >>= 1;
        ena_log_raw(
            INFO,
            "large LLQ policy requires limiting Tx queue size to %u entries",
            max_tx_queue_size);
      } else if (dev->max_wide_llq_depth < max_tx_queue_size) {
        /* In case the queue depth that the driver calculated exceeds
         * the maximal value that the device allows, it will be limited
         * to that maximal value
         */
        max_tx_queue_size = dev->max_wide_llq_depth;
      }
    } else {
      ena_log_raw(
          INFO,
          "Forcing large LLQ headers failed since device lacks this support");
    }
  }

  if (unlikely(max_rx_queue_size == 0 || max_tx_queue_size == 0)) {
    ena_log_raw(ERR, "Invalid queue size");
    return -EFAULT;
  }

  ctx->max_tx_queue_size = max_tx_queue_size;
  ctx->max_rx_queue_size = max_rx_queue_size;

  ena_log_raw(INFO, "tx queue size %u", max_tx_queue_size);
  return 0;
}

static void ena_stats_restart(rte_eth_dev *dev) {
  ena_adapter *adapter = dev->get<ena_adapter>();

  adapter->drv_stats->ierrors.store(0);
  adapter->drv_stats->oerrors.store(0);
  adapter->drv_stats->rx_nombuf.store(0);
  adapter->drv_stats->rx_drops = 0;
}

static int ena_stats_get(rte_eth_dev *dev, rte_eth_stats *stats) {
  ena_admin_basic_stats ena_stats;
  struct ena_adapter *adapter = dev->get<ena_adapter>();
  struct ena_com_dev *ena_dev = &adapter->ena_dev;
  int rc;
  int i;
  int max_rings_stats;

  memset(&ena_stats, 0, sizeof(ena_stats));

  rte_spinlock_lock(&adapter->admin_lock);
  rc = ena_com_get_dev_basic_stats(ena_dev, &ena_stats);
  rte_spinlock_unlock(&adapter->admin_lock);
  if (unlikely(rc)) {
    ena_log_raw(ERR, "Could not retrieve statistics from ENA");
    return rc;
  }

  stats->ipackets =
      __MERGE_64B_H_L(ena_stats.rx_pkts_high, ena_stats.rx_pkts_low);
  stats->opackets =
      __MERGE_64B_H_L(ena_stats.tx_pkts_high, ena_stats.tx_pkts_low);
  stats->ibytes =
      __MERGE_64B_H_L(ena_stats.rx_bytes_high, ena_stats.rx_bytes_low);
  stats->obytes =
      __MERGE_64B_H_L(ena_stats.tx_bytes_high, ena_stats.tx_bytes_low);

  stats->imissed = adapter->drv_stats->rx_drops;
  stats->ierrors = rte_atomic64_read(&adapter->drv_stats->ierrors);
  stats->oerrors = rte_atomic64_read(&adapter->drv_stats->oerrors);
  stats->rx_nombuf = rte_atomic64_read(&adapter->drv_stats->rx_nombuf);

  max_rings_stats =
      RTE_MIN(dev->data.nb_rx_queues, RTE_ETHDEV_QUEUE_STAT_CNTRS);
  for (i = 0; i < max_rings_stats; ++i) {
    struct ena_stats_rx *rx_stats = &adapter->rx_ring[i].rx_stats;

    stats->q_ibytes[i] = rx_stats->bytes;
    stats->q_ipackets[i] = rx_stats->cnt;
    stats->q_errors[i] = rx_stats->bad_desc_num + rx_stats->bad_req_id +
                         rx_stats->bad_desc + rx_stats->unknown_error;
  }

  max_rings_stats =
      RTE_MIN(dev->data.nb_tx_queues, RTE_ETHDEV_QUEUE_STAT_CNTRS);
  for (i = 0; i < max_rings_stats; ++i) {
    struct ena_stats_tx *tx_stats = &adapter->tx_ring[i].tx_stats;

    stats->q_obytes[i] = tx_stats->bytes;
    stats->q_opackets[i] = tx_stats->cnt;
  }

  return 0;
}

static int ena_create_io_queue(rte_eth_dev *dev, ena_ring *ring) {
  ena_adapter *adapter = ring->adapter;
  ena_com_dev *ena_dev = &adapter->ena_dev;
  ena_com_create_io_ctx ctx =
      /* policy set to _HOST just to satisfy icc compiler */
      {ENA_ADMIN_PLACEMENT_POLICY_HOST,
       ENA_COM_IO_QUEUE_DIRECTION_RX,
       0,
       0,
       0,
       0};
  uint16_t ena_qid;
  unsigned int i;
  int rc;

  if (ring->type == ENA_RING_TYPE_TX) {
    ena_qid = ENA_IO_TXQ_IDX(ring->id);
    ctx.direction = ENA_COM_IO_QUEUE_DIRECTION_TX;
    ctx.mem_queue_type = ena_dev->tx_mem_queue_type;
    ctx.msix_vector = -1;
    for (i = 0; i < ring->ring_size; i++)
      ring->empty_tx_reqs[i] = i;
  } else {
    ena_qid = ENA_IO_RXQ_IDX(ring->id);
    ctx.direction = ENA_COM_IO_QUEUE_DIRECTION_RX;
    ctx.msix_vector = adapter->edev->data.dev_conf.intr_conf.rxq
                          ? ENA_IO_IRQ_IDX(ring->id)
                          : -1;

    for (i = 0; i < ring->ring_size; i++)
      ring->empty_rx_reqs[i] = i;
  }
  ctx.queue_size = ring->ring_size;
  ctx.qid = ena_qid;
  ctx.numa_node = ring->numa_socket_id;

  rc = ena_com_create_io_queue(ena_dev, &ctx);
  if (rc) {
    ena_log_raw(ERR, "Failed to create IO queue[%d] (qid:%d), rc: %d", ring->id,
                ena_qid, rc);
    return rc;
  }

  rc = ena_com_get_io_handlers(ena_dev, ena_qid, &ring->ena_com_io_sq,
                               &ring->ena_com_io_cq);
  if (rc) {
    ena_log_raw(ERR, "Failed to get IO queue[%d] handlers, rc: %d", ring->id,
                rc);
    ena_com_destroy_io_queue(ena_dev, ena_qid);
    return rc;
  }

  if (ring->type == ENA_RING_TYPE_TX)
    ena_com_update_numa_node(ring->ena_com_io_cq, ctx.numa_node);

  return 0;
}

static void ena_queue_stop(ena_ring *ring) {
  struct ena_com_dev *ena_dev = &ring->adapter->ena_dev;

  if (ring->type == ENA_RING_TYPE_RX) {
    ena_com_destroy_io_queue(ena_dev, ENA_IO_RXQ_IDX(ring->id));
    ena_rx_queue_release_bufs(ring);
    auto id = ENA_IO_IRQ_IDX(ring->id);
    if (ring->adapter->irq_tbl[id].mvec)
      delete ring->adapter->irq_tbl[id].mvec;
  } else {
    ena_com_destroy_io_queue(ena_dev, ENA_IO_TXQ_IDX(ring->id));
    ena_tx_queue_release_bufs(ring);
  }
}

static void ena_queue_stop_all(rte_eth_dev *dev, enum ena_ring_type ring_type) {
  ena_adapter *adapter = dev->get<ena_adapter>();
  ena_ring *queues = NULL;
  uint16_t nb_queues, i;

  if (ring_type == ENA_RING_TYPE_RX) {
    queues = adapter->rx_ring;
    nb_queues = dev->data.nb_rx_queues;
  } else {
    queues = adapter->tx_ring;
    nb_queues = dev->data.nb_tx_queues;
  }

  for (i = 0; i < nb_queues; ++i)
    if (queues[i].configured)
      ena_queue_stop(&queues[i]);
}

/* Signal, join and dispose the per-RX-ring interrupt handler threads. Only
 * relevant when MSI-X is enabled, since that is when the handler threads are
 * started. Must run before the IO queues are destroyed so the threads stop
 * touching the ring first.
 */
static void ena_collect_intr_threads(ena_adapter *adapter) {

  uint16_t nb_rx = adapter->edev->data.nb_rx_queues;
  for (uint16_t i = 0; i < nb_rx; i++) {
    ena_ring *ring = &adapter->rx_ring[i];
    sched::thread *t = ring->intr_thread;
    if (!t)
      continue;
    ring->should_stop.store(true, std::memory_order_release);
    t->wake();                 /* unblock wait_for so it observes the flag */
    t->join();                 /* wait for ena_rx_cleanup to return */
    sched::thread::dispose(t); /* make()'d threads are freed via dispose() */
    ring->intr_thread = nullptr;
  }
}

static int ena_queue_start(struct rte_eth_dev *dev, ena_ring *ring) {
  int rc, bufs_num;
  rc = ena_create_io_queue(dev, ring);
  if (rc) {
    ena_log_raw(ERR, "Failed to create IO queue");
    return rc;
  }

  ring->next_to_clean = 0;
  ring->next_to_use = 0;

  if (ring->type == ENA_RING_TYPE_TX) {
    ring->tx_stats.available_desc = ena_com_free_q_entries(ring->ena_com_io_sq);
    return 0;
  } else if (dev->data.dev_conf.intr_conf.rxq) {
    assert(ring->handler);
    ring->should_stop.store(false, std::memory_order_release);
    ring->intr_thread = sched::thread::make(
        [ring, dev]() { ena_rx_cleanup(ring, dev, ring->handler); });
    ring->intr_thread->start();
  }

  bufs_num = ring->ring_size - 1;
  rc = ena_populate_rx_queue(ring, bufs_num);
  if (rc != bufs_num) {
    ena_com_destroy_io_queue(&ring->adapter->ena_dev, ENA_IO_RXQ_IDX(ring->id));
    ena_log_raw(ERR, "Failed to populate Rx ring");
    return ENA_COM_FAULT;
  }

  /* Flush per-core RX buffers pools cache as they can be used on other
   * cores as well.
   */
  // rte_mempool_cache_flush(NULL, ring->mb_pool);
  return 0;
}

static int ena_add_single_rx_desc(struct ena_com_io_sq *io_sq, rte_mbuf *mbuf,
                                  uint16_t id) {
  struct ena_com_buf ebuf;
  int rc;

  /* prepare physical address for DMA transaction */
  ebuf.paddr = mbuf->iova;
  ebuf.len = mbuf->buf_len;

  /* pass resource to device */
  rc = ena_com_add_single_rx_desc(io_sq, &ebuf, id);
  if (unlikely(rc != 0))
    ena_log_raw(WARN, "Failed adding Rx desc");

  return rc;
}

static int ena_populate_rx_queue(ena_ring *rxq, unsigned int count) {
  unsigned int i;
  int rc;
  uint16_t next_to_use = rxq->next_to_use;
  uint16_t req_id;
#ifdef RTE_ETHDEV_DEBUG_RX
  uint16_t in_use;
#endif
  rte_mbuf **mbufs = rxq->rx_refill_buffer;

  if (unlikely(!count))
    return 0;

#ifdef RTE_ETHDEV_DEBUG_RX
  in_use = rxq->ring_size - 1 - ena_com_free_q_entries(rxq->ena_com_io_sq);
  if (unlikely((in_use + count) >= rxq->ring_size))
    ena_log_raw(ERR, "Bad Rx ring state");
#endif

  /* get resources for incoming packets */
  rc = rte_pktmbuf_alloc_bulk(rxq->mb_pool, mbufs, count);
  if (unlikely(rc < 0)) {
    rte_atomic64_inc(&rxq->adapter->drv_stats->rx_nombuf);
    ++rxq->rx_stats.mbuf_alloc_fail;
    ena_log_raw(DBG, "There are not enough free buffers");
    return 0;
  }

  for (i = 0; i < count; i++) {
    rte_mbuf *mbuf = mbufs[i];
    struct ena_rx_buffer *rx_info;

    if (likely((i + 4) < count))
      rte_prefetch0(mbufs[i + 4]);

    req_id = rxq->empty_rx_reqs[next_to_use];
    rx_info = &rxq->rx_buffer_info[req_id];

    rc = ena_add_single_rx_desc(rxq->ena_com_io_sq, mbuf, req_id);
    if (unlikely(rc != 0))
      break;

    rx_info->mbuf = mbuf;
    next_to_use = ENA_IDX_NEXT_MASKED(next_to_use, rxq->size_mask);
  }

  if (unlikely(i < count)) {
    ena_log_raw(WARN, "Refilled Rx queue[%d] with only %d/%d buffers", rxq->id,
                i, count);
    rte_pktmbuf_free_bulk(&mbufs[i], count - i);
    ++rxq->rx_stats.refill_partial;
  }

  /* When we submitted free resources to device... */
  if (likely(i > 0)) {
    /* ...let HW know that it can fill buffers with data. */
    ena_com_write_rx_sq_doorbell(rxq->ena_com_io_sq);

    rxq->next_to_use = next_to_use;
  }

  return i;
}

static size_t ena_get_metrics_entries(struct ena_adapter *adapter) {
  struct ena_com_dev *ena_dev = &adapter->ena_dev;
  size_t metrics_num = 0;

  if (ena_com_get_cap(ena_dev, ENA_ADMIN_CUSTOMER_METRICS))
    metrics_num = ENA_STATS_ARRAY_METRICS;
  else if (ena_com_get_cap(ena_dev, ENA_ADMIN_ENI_STATS))
    metrics_num = ENA_STATS_ARRAY_METRICS_LEGACY;
  ena_log_raw(NOTICE, "0x%x customer metrics are supported",
              (unsigned int)metrics_num);
  if (metrics_num > ENA_MAX_CUSTOMER_METRICS) {
    ena_log_raw(NOTICE, "Not enough space for the requested customer metrics");
    metrics_num = ENA_MAX_CUSTOMER_METRICS;
  }
  return metrics_num;
}

static int ena_device_init(struct ena_adapter *adapter, pci::device *pdev,
                           struct ena_com_dev_get_features_ctx *get_feat_ctx) {
  ena_com_dev *ena_dev = &adapter->ena_dev;
  uint32_t aenq_groups;
  int rc;
  bool readless_supported;

  /* Initialize mmio registers */
  rc = ena_com_mmio_reg_read_request_init(ena_dev);
  if (rc) {
    ena_log_raw(ERR, "Failed to init MMIO read less");
    return rc;
  }

  /* The PCIe configuration space revision id indicate if mmio reg
   * read is disabled.
   */
  readless_supported = !(pdev->get_revision_id() & ENA_MMIO_DISABLE_REG_READ);
  ena_com_set_mmio_read_mode(ena_dev, readless_supported);

  /* reset device */
  rc = ena_com_dev_reset(ena_dev, ENA_REGS_RESET_NORMAL);
  if (rc) {
    ena_log_raw(ERR, "Cannot reset device");
    goto err_mmio_read_less;
  }

  /* check FW version */
  rc = ena_com_validate_version(ena_dev);
  if (rc) {
    ena_log_raw(ERR, "Device version is too low");
    goto err_mmio_read_less;
  }

  ena_dev->dma_addr_bits = ena_com_get_dma_width(ena_dev);

  /* ENA device administration layer init */
  rc = ena_com_admin_init(ena_dev, &aenq_handlers);
  if (rc) {
    ena_log_raw(ERR, "Cannot initialize ENA admin queue");
    goto err_mmio_read_less;
  }

  /* To enable the msix interrupts the driver needs to know the number
   * of queues. So the driver uses polling mode to retrieve this
   * information.
   */
  ena_com_set_admin_polling_mode(ena_dev, true);

  /* AWS Nitro Gen 5 (c7i.large) doesn't deliver the ENA VF's MSI-X
   * through the bridge chain even with bus-master everywhere. Auto-poll
   * fallback keeps the admin queue alive after the resulting timeout
   * instead of marking it dead. */
  ena_com_set_admin_auto_polling_mode(ena_dev, true);

  ena_config_host_info(ena_dev);

  /* Get Device Attributes and features */
  rc = ena_com_get_dev_attr_feat(ena_dev, get_feat_ctx);
  if (rc) {
    ena_log_raw(ERR, "Cannot get attribute for ENA device, rc: %d", rc);
    goto err_admin_init;
  }

  aenq_groups = BIT(ENA_ADMIN_LINK_CHANGE) | BIT(ENA_ADMIN_NOTIFICATION) |
                BIT(ENA_ADMIN_KEEP_ALIVE) | BIT(ENA_ADMIN_FATAL_ERROR) |
                BIT(ENA_ADMIN_WARNING) | BIT(ENA_ADMIN_CONF_NOTIFICATIONS);

  aenq_groups &= get_feat_ctx->aenq.supported_groups;

  adapter->all_aenq_groups = aenq_groups;
  /* The actual supported number of metrics is negotiated with the device at
   * runtime */
  adapter->metrics_num = ena_get_metrics_entries(adapter);

  return 0;

err_admin_init:
  ena_com_admin_destroy(ena_dev);

err_mmio_read_less:
  ena_com_mmio_reg_read_request_destroy(ena_dev);

  return rc;
}

static void check_for_missing_keep_alive(struct ena_adapter *adapter) {
  if (!(adapter->active_aenq_groups & BIT(ENA_ADMIN_KEEP_ALIVE)))
    return;

  if (adapter->keep_alive_timeout == ENA_HW_HINTS_NO_TIMEOUT)
    return;

  if (unlikely((rte_get_timer_cycles() - adapter->timestamp_wd) >=
               adapter->keep_alive_timeout)) {
    ena_log_raw(ERR, "Keep alive timeout");
    ena_trigger_reset(adapter, ENA_REGS_RESET_KEEP_ALIVE_TO);
    ++adapter->dev_stats.wd_expired;
  }
}

/* Check if admin queue is enabled */
static void check_for_admin_com_state(struct ena_adapter *adapter) {
  if (unlikely(!ena_com_get_admin_running_state(&adapter->ena_dev))) {
    ena_log_raw(ERR, "ENA admin queue is not in running state");
    ena_trigger_reset(adapter, ENA_REGS_RESET_ADMIN_TO);
  }
}

static int check_for_tx_completion_in_queue(struct ena_adapter *adapter,
                                            struct ena_ring *tx_ring) {
  ena_tx_buffer *tx_buf;
  uint64_t timestamp;
  uint64_t completion_delay;
  uint32_t missed_tx = 0;
  unsigned int i;
  int rc = 0;

  for (i = 0; i < tx_ring->ring_size; ++i) {
    tx_buf = &tx_ring->tx_buffer_info[i];
    timestamp = tx_buf->timestamp;

    if (timestamp == 0)
      continue;
    completion_delay = rte_get_timer_cycles() - timestamp;
    if (completion_delay > adapter->missing_tx_completion_to) {
      if (unlikely(!tx_buf->print_once)) {
        ena_log_raw(
            WARN,
            "Found a Tx that wasn't completed on time, qid %d, index %d. "
            "Missing Tx outstanding for %" PRIu64 " msecs.",
            tx_ring->id, i, completion_delay / rte_get_timer_hz() * 1000);
        tx_buf->print_once = true;
      }
      ++missed_tx;
    }
  }

  if (unlikely(missed_tx > tx_ring->missing_tx_completion_threshold)) {
    ena_log_raw(
        ERR,
        "The number of lost Tx completions is above the threshold (%d > %d). "
        "Trigger the device reset.",
        missed_tx, tx_ring->missing_tx_completion_threshold);
    adapter->reset_reason = ENA_REGS_RESET_MISS_TX_CMPL;
    adapter->trigger_reset = true;
    rc = -EIO;
  }

  tx_ring->tx_stats.missed_tx += missed_tx;

  return rc;
}

static void check_for_tx_completions(ena_adapter *adapter) {
  struct ena_ring *tx_ring;
  uint64_t tx_cleanup_delay;
  size_t qid;
  int budget;
  auto &edev_data = adapter->edev->data;
  uint16_t nb_tx_queues = edev_data.nb_tx_queues;

  if (adapter->missing_tx_completion_to == ENA_HW_HINTS_NO_TIMEOUT)
    return;

  nb_tx_queues = edev_data.nb_tx_queues;
  budget = adapter->missing_tx_completion_budget;

  qid = adapter->last_tx_comp_qid;
  while (budget-- > 0) {
    tx_ring = &adapter->tx_ring[qid];

    /* Tx cleanup is called only by the burst function and can be
     * called dynamically by the application. Also cleanup is
     * limited by the threshold. To avoid false detection of the
     * missing HW Tx completion, get the delay since last cleanup
     * function was called.
     */
    tx_cleanup_delay = rte_get_timer_cycles() - tx_ring->last_cleanup_ticks;
    if (tx_cleanup_delay < adapter->tx_cleanup_stall_delay)
      check_for_tx_completion_in_queue(adapter, tx_ring);
    qid = (qid + 1) % nb_tx_queues;
  }
  adapter->last_tx_comp_qid = qid;
}

[[maybe_unused]] static void ena_timer_wd_callback(void *arg) {
  rte_eth_dev *dev = static_cast<rte_eth_dev *>(arg);
  ena_adapter *adapter = dev->get<ena_adapter>();

  if (unlikely(adapter->trigger_reset))
    return;
  check_for_missing_keep_alive(adapter);
  check_for_admin_com_state(adapter);
  check_for_tx_completions(adapter);
  if (unlikely(adapter->trigger_reset)) {
    ena_log_raw(ERR, "Trigger reset is on");
  }
  rte_timer_reset(&adapter->timer_wd, rte_get_timer_hz(), ena_timer_wd_callback,
                  arg);
}

static inline void
set_default_llq_configurations(struct ena_llq_configurations *llq_config,
                               struct ena_admin_feature_llq_desc *llq,
                               bool use_large_llq_hdr) {
  llq_config->llq_header_location = ENA_ADMIN_INLINE_HEADER;
  llq_config->llq_stride_ctrl = ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY;
  llq_config->llq_num_decs_before_header =
      ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2;

  if (use_large_llq_hdr &&
      (llq->entry_size_ctrl_supported & ENA_ADMIN_LIST_ENTRY_SIZE_256B)) {
    llq_config->llq_ring_entry_size = ENA_ADMIN_LIST_ENTRY_SIZE_256B;
    llq_config->llq_ring_entry_size_value = 256;
  } else {
    llq_config->llq_ring_entry_size = ENA_ADMIN_LIST_ENTRY_SIZE_128B;
    llq_config->llq_ring_entry_size_value = 128;
  }
}

static int ena_set_queues_placement_policy(
    struct ena_adapter *adapter, ena_com_dev *ena_dev,
    struct ena_admin_feature_llq_desc *llq,
    struct ena_llq_configurations *llq_default_configurations) {
  int rc;
  u32 llq_feature_mask;

  if (adapter->llq_header_policy == ENA_LLQ_POLICY_DISABLED) {
    ena_log_raw(WARN, "NOTE: LLQ has been disabled as per user's request. "
                      "This may lead to a huge performance degradation!");
    ena_dev->tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
    return 0;
  }

  llq_feature_mask = 1 << ENA_ADMIN_LLQ;
  if (!(ena_dev->supported_features & llq_feature_mask)) {
    ena_log_raw(INFO, "LLQ is not supported. Fallback to host mode policy.");
    ena_dev->tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
    return 0;
  }

  if (adapter->dev_mem == NULL) {
    ena_log_raw(
        ERR,
        "LLQ is advertised as supported, but device doesn't expose mem bar");
    ena_dev->tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
    return 0;
  }

  rc = ena_com_config_dev_mode(ena_dev, llq, llq_default_configurations);
  if (unlikely(rc)) {
    ena_log_raw(WARN,
                "Failed to config dev mode. Fallback to host mode policy.");
    ena_dev->tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
    return 0;
  }

  /* Nothing to config, exit */
  if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_HOST)
    return 0;
  // The LLQ BAR takes each frame's 128-byte entry as a burst of stores;
  // write-combined they leave as one transaction, uncached as sixteen.
  adapter->dev_mem->map(mem::mattr::wc);
  ena_dev->mem_bar = const_cast<void *>(adapter->dev_mem->get_mmio());

  return 0;
}

static uint32_t
ena_calc_max_io_queue_num(struct ena_com_dev *ena_dev,
                          struct ena_com_dev_get_features_ctx *get_feat_ctx) {
  uint32_t io_tx_sq_num, io_tx_cq_num, io_rx_num, max_num_io_queues;

  /* Regular queues capabilities */
  if (ena_dev->supported_features & BIT(ENA_ADMIN_MAX_QUEUES_EXT)) {
    struct ena_admin_queue_ext_feature_fields *max_queue_ext =
        &get_feat_ctx->max_queue_ext.max_queue_ext;
    io_rx_num =
        RTE_MIN(max_queue_ext->max_rx_sq_num, max_queue_ext->max_rx_cq_num);
    io_tx_sq_num = max_queue_ext->max_tx_sq_num;
    io_tx_cq_num = max_queue_ext->max_tx_cq_num;
  } else {
    struct ena_admin_queue_feature_desc *max_queues = &get_feat_ctx->max_queues;
    io_tx_sq_num = max_queues->max_sq_num;
    io_tx_cq_num = max_queues->max_cq_num;
    io_rx_num = RTE_MIN(io_tx_sq_num, io_tx_cq_num);
  }

  /* In case of LLQ use the llq number in the get feature cmd */
  if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV)
    io_tx_sq_num = get_feat_ctx->llq.max_llq_num;

  max_num_io_queues = RTE_MIN(ENA_MAX_NUM_IO_QUEUES, io_rx_num);
  max_num_io_queues = RTE_MIN(max_num_io_queues, io_tx_sq_num);
  max_num_io_queues = RTE_MIN(max_num_io_queues, io_tx_cq_num);

  if (unlikely(max_num_io_queues == 0)) {
    ena_log_raw(ERR, "Number of IO queues cannot not be 0");
    return -EFAULT;
  }

  return max_num_io_queues;
}

static void
ena_set_offloads(struct ena_offloads *offloads,
                 struct ena_admin_feature_offload_desc *offload_desc) {
  if (offload_desc->tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_IPV4_MASK)
    offloads->tx_offloads |= ENA_IPV4_TSO;

  /* Tx IPv4 checksum offloads */
  if (offload_desc->tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L3_CSUM_IPV4_MASK)
    offloads->tx_offloads |= ENA_L3_IPV4_CSUM;
  if (offload_desc->tx &
      ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_FULL_MASK)
    offloads->tx_offloads |= ENA_L4_IPV4_CSUM;
  if (offload_desc->tx &
      ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_PART_MASK)
    offloads->tx_offloads |= ENA_L4_IPV4_CSUM_PARTIAL;

  /* Tx IPv6 checksum offloads */
  if (offload_desc->tx &
      ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV6_CSUM_FULL_MASK)
    offloads->tx_offloads |= ENA_L4_IPV6_CSUM;
  if (offload_desc->tx &
      ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV6_CSUM_PART_MASK)
    offloads->tx_offloads |= ENA_L4_IPV6_CSUM_PARTIAL;

  /* Rx IPv4 checksum offloads */
  if (offload_desc->rx_supported &
      ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L3_CSUM_IPV4_MASK)
    offloads->rx_offloads |= ENA_L3_IPV4_CSUM;
  if (offload_desc->rx_supported &
      ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV4_CSUM_MASK)
    offloads->rx_offloads |= ENA_L4_IPV4_CSUM;

  /* Rx IPv6 checksum offloads */
  if (offload_desc->rx_supported &
      ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV6_CSUM_MASK)
    offloads->rx_offloads |= ENA_L4_IPV6_CSUM;

  if (offload_desc->rx_supported & ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_HASH_MASK)
    offloads->rx_offloads |= ENA_RX_RSS_HASH;
}

static void ena_init_rings(struct ena_adapter *adapter,
                           bool disable_meta_caching) {
  size_t i;

  for (i = 0; i < adapter->max_num_io_queues; i++) {
    struct ena_ring *ring = &adapter->tx_ring[i];

    ring->configured = 0;
    ring->type = ENA_RING_TYPE_TX;
    ring->adapter = adapter;
    ring->id = i;
    ring->tx_mem_queue_type = adapter->ena_dev.tx_mem_queue_type;
    ring->tx_max_header_size = adapter->ena_dev.tx_max_header_size;
    ring->sgl_size = adapter->max_tx_sgl_size;
    ring->disable_meta_caching = disable_meta_caching;
  }

  for (i = 0; i < adapter->max_num_io_queues; i++) {
    struct ena_ring *ring = &adapter->rx_ring[i];

    ring->configured = 0;
    ring->type = ENA_RING_TYPE_RX;
    ring->adapter = adapter;
    ring->id = i;
    ring->sgl_size = adapter->max_rx_sgl_size;
  }
}

static uint64_t ena_get_rx_port_offloads(struct ena_adapter *adapter) {
  uint64_t port_offloads = 0;

  if (adapter->offloads.rx_offloads & ENA_L3_IPV4_CSUM)
    port_offloads |= RTE_ETH_RX_OFFLOAD_IPV4_CKSUM;

  if (adapter->offloads.rx_offloads & (ENA_L4_IPV4_CSUM | ENA_L4_IPV6_CSUM))
    port_offloads |=
        RTE_ETH_RX_OFFLOAD_UDP_CKSUM | RTE_ETH_RX_OFFLOAD_TCP_CKSUM;

  if (adapter->offloads.rx_offloads & ENA_RX_RSS_HASH)
    port_offloads |= RTE_ETH_RX_OFFLOAD_RSS_HASH;

  port_offloads |= RTE_ETH_RX_OFFLOAD_SCATTER;

  return port_offloads;
}

static uint64_t ena_get_tx_port_offloads(struct ena_adapter *adapter) {
  uint64_t port_offloads = 0;

  if (adapter->offloads.tx_offloads & ENA_IPV4_TSO)
    port_offloads |= RTE_ETH_TX_OFFLOAD_TCP_TSO;

  if (adapter->offloads.tx_offloads & ENA_L3_IPV4_CSUM)
    port_offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
  if (adapter->offloads.tx_offloads &
      (ENA_L4_IPV4_CSUM_PARTIAL | ENA_L4_IPV4_CSUM | ENA_L4_IPV6_CSUM |
       ENA_L4_IPV6_CSUM_PARTIAL))
    port_offloads |=
        RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM;

  port_offloads |= RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

  port_offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

  return port_offloads;
}

static uint64_t ena_get_rx_queue_offloads(struct ena_adapter *adapter) {
  (void)(adapter);

  return 0;
}

static uint64_t ena_get_tx_queue_offloads(struct ena_adapter *adapter) {
  uint64_t queue_offloads = 0;
  (void)(adapter);

  queue_offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

  return queue_offloads;
}

static int ena_infos_get(rte_eth_dev *dev, rte_eth_dev_info *dev_info) {

  auto *adapter = dev->get<ena_adapter>();

  dev_info->rx_offload_capa = ena_get_rx_port_offloads(adapter);
  dev_info->tx_offload_capa = ena_get_tx_port_offloads(adapter);
  dev_info->rx_queue_offload_capa = ena_get_rx_queue_offloads(adapter);
  dev_info->tx_queue_offload_capa = ena_get_tx_queue_offloads(adapter);

  dev_info->flow_type_rss_offloads = ENA_ALL_RSS_HF;
  dev_info->hash_key_size = ENA_HASH_KEY_SIZE;

  dev_info->min_rx_bufsize = ENA_MIN_FRAME_LEN;
  dev_info->max_rx_pktlen =
      adapter->max_mtu + RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN;
  dev_info->min_mtu = ENA_MIN_MTU;
  dev_info->max_mtu = adapter->max_mtu;
  dev_info->max_mac_addrs = 1;

  dev_info->max_rx_queues = adapter->max_num_io_queues;
  dev_info->max_tx_queues = adapter->max_num_io_queues;
  dev_info->reta_size = ENA_RX_RSS_TABLE_SIZE;

  dev_info->rx_desc_lim.nb_max = adapter->max_rx_ring_size;
  dev_info->rx_desc_lim.nb_min = ENA_MIN_RING_DESC;
  dev_info->rx_desc_lim.nb_seg_max =
      RTE_MIN(ENA_PKT_MAX_BUFS, adapter->max_rx_sgl_size);
  dev_info->rx_desc_lim.nb_mtu_seg_max =
      RTE_MIN(ENA_PKT_MAX_BUFS, adapter->max_rx_sgl_size);

  dev_info->tx_desc_lim.nb_max = adapter->max_tx_ring_size;
  dev_info->tx_desc_lim.nb_min = ENA_MIN_RING_DESC;
  dev_info->tx_desc_lim.nb_seg_max =
      RTE_MIN(ENA_PKT_MAX_BUFS, adapter->max_tx_sgl_size);
  dev_info->tx_desc_lim.nb_mtu_seg_max =
      RTE_MIN(ENA_PKT_MAX_BUFS, adapter->max_tx_sgl_size);

  dev_info->default_rxportconf.ring_size =
      RTE_MIN(ENA_DEFAULT_RING_SIZE, dev_info->rx_desc_lim.nb_max);
  dev_info->default_txportconf.ring_size =
      RTE_MIN(ENA_DEFAULT_RING_SIZE, dev_info->tx_desc_lim.nb_max);

  return 0;
}

/**********************************************************************
 * datapath
 * ********************************************************************/

static inline void ena_init_rx_mbuf(rte_mbuf *mbuf, uint16_t len) {
  mbuf->refcnt = 1;
  mbuf->data_len = len;
  mbuf->next = NULL;
}

static rte_mbuf *ena_rx_mbuf(struct ena_ring *rx_ring,
                             struct ena_com_rx_buf_info *ena_bufs,
                             uint32_t descs, uint16_t *next_to_clean,
                             uint8_t offset) {
  rte_mbuf *mbuf;
  rte_mbuf *mbuf_head;
  struct ena_rx_buffer *rx_info;
  int rc;
  uint16_t ntc, len, req_id, buf = 0;

  if (unlikely(descs == 0))
    return NULL;
  ntc = *next_to_clean;

  len = ena_bufs[buf].len;
  req_id = ena_bufs[buf].req_id;

  rx_info = &rx_ring->rx_buffer_info[req_id];

  mbuf = rx_info->mbuf;
  ena_init_rx_mbuf(mbuf, len);

  /* Fill the mbuf head with the data specific for 1st segment. */
  mbuf_head = mbuf;
  mbuf_head->nb_segs = descs;
  // mbuf_head->port = rx_ring->port_id;
  mbuf_head->pkt_len = len;
  mbuf_head->data_offset += offset;

  rx_info->mbuf = NULL;
  rx_ring->empty_rx_reqs[ntc] = req_id;
  ntc = ENA_IDX_NEXT_MASKED(ntc, rx_ring->size_mask);

  while (--descs) {
    ++buf;
    len = ena_bufs[buf].len;
    req_id = ena_bufs[buf].req_id;

    rx_info = &rx_ring->rx_buffer_info[req_id];

    if (unlikely(len == 0)) {
      /*
       * Some devices can pass descriptor with the length 0.
       * To avoid confusion, the PMD is simply putting the
       * descriptor back, as it was never used. We'll avoid
       * mbuf allocation that way.
       */
      rc =
          ena_add_single_rx_desc(rx_ring->ena_com_io_sq, rx_info->mbuf, req_id);
      if (unlikely(rc != 0)) {
        /* Free the mbuf in case of an error. */
        rte_mbuf_raw_free(rx_info->mbuf);
      } else {
        /*
         * If there was no error, just exit the loop as
         * 0 length descriptor is always the last one.
         */
        break;
      }
    } else {
      /* Create an mbuf chain. */
      mbuf->next = rx_info->mbuf;
      mbuf = mbuf->next;

      ena_init_rx_mbuf(mbuf, len);
      mbuf_head->pkt_len += len;
    }

    /*
     * Mark the descriptor as depleted and perform necessary
     * cleanup.
     * This code will execute in two cases:
     *  1. Descriptor len was greater than 0 - normal situation.
     *  2. Descriptor len was 0 and we failed to add the descriptor
     *     to the device. In that situation, we should try to add
     *     the mbuf again in the populate routine and mark the
     *     descriptor as urx_free_threshsed up by the device.
     */
    rx_info->mbuf = NULL;
    rx_ring->empty_rx_reqs[ntc] = req_id;
    ntc = ENA_IDX_NEXT_MASKED(ntc, rx_ring->size_mask);
  }

  *next_to_clean = ntc;

  return mbuf_head;
}

static void ena_update_hints(struct ena_adapter *adapter,
                             struct ena_admin_ena_hw_hints *hints) {
  if (hints->admin_completion_tx_timeout)
    adapter->ena_dev.admin_queue.completion_timeout =
        hints->admin_completion_tx_timeout * 1000;

  if (hints->mmio_read_timeout)
    /* convert to usec */
    adapter->ena_dev.mmio_read.reg_read_to = hints->mmio_read_timeout * 1000;

  if (hints->driver_watchdog_timeout) {
    if (hints->driver_watchdog_timeout == ENA_HW_HINTS_NO_TIMEOUT)
      adapter->keep_alive_timeout = ENA_HW_HINTS_NO_TIMEOUT;
    else
      // Convert msecs to ticks
      adapter->keep_alive_timeout =
          (hints->driver_watchdog_timeout * rte_get_timer_hz()) / 1000;
  }
}

static void ena_tx_map_mbuf(ena_ring *tx_ring, ena_tx_buffer *tx_info,
                            rte_mbuf *mbuf, void **push_header,
                            uint16_t *header_len) {
  struct ena_com_buf *ena_buf;
  uint16_t delta, seg_len, push_len;

  delta = 0;
  seg_len = mbuf->data_len;

  tx_info->mbuf = mbuf;
  ena_buf = tx_info->bufs;

  if (tx_ring->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
    /*
     * Tx header might be (and will be in most cases) smaller than
     * tx_max_header_size. But it's not an issue to send more data
     * to the device, than actually needed if the mbuf size is
     * greater than tx_max_header_size.
     */
    push_len = RTE_MIN(mbuf->pkt_len, tx_ring->tx_max_header_size);
    *header_len = push_len;

    if (likely(push_len <= seg_len)) {
      /* If the push header is in the single segment, then
       * just point it to the 1st mbuf data.
       */
      *push_header = rte_pktmbuf_mtod(mbuf, uint8_t *);
    } else {
      /* If the push header lays in the several segments, copy
       * it to the intermediate buffer.
       */
      rte_pktmbuf_read(mbuf, 0, push_len, tx_ring->push_buf_intermediate_buf);
      *push_header = tx_ring->push_buf_intermediate_buf;
      delta = push_len - seg_len;
    }
  } else {
    *push_header = NULL;
    *header_len = 0;
    push_len = 0;
  }

  if (seg_len > push_len) {
    ena_buf->paddr = mbuf->iova + push_len;
    ena_buf->len = seg_len - push_len;
    ena_buf++;
    tx_info->num_of_bufs++;
  }

  while ((mbuf = mbuf->next) != nullptr) {
    seg_len = mbuf->data_len;

    /* Skip mbufs if whole data is pushed as a header */
    if (delta > seg_len) {
      delta -= seg_len;
      continue;
    }

    ena_buf->paddr = mbuf->iova + delta;
    ena_buf->len = seg_len - delta;
    ena_buf++;
    tx_info->num_of_bufs++;

    delta = 0;
  }
}

static int ena_xmit_mbuf(ena_ring *tx_ring, rte_mbuf *mbuf) {
  struct ena_tx_buffer *tx_info;
  struct ena_com_tx_ctx ena_tx_ctx = {{0}};
  uint16_t next_to_use;
  uint16_t header_len;
  uint16_t req_id;
  void *push_header;
  int nb_hw_desc;
  int rc;

  /* Checking for space for 2 additional metadata descriptors due to
   * possible header split and metadata descriptor
   */
  if (!ena_com_sq_have_enough_space(tx_ring->ena_com_io_sq,
                                    mbuf->nb_segs + 2)) {
    ena_log_raw(DBG, "Not enough space in the tx queue");
    return ENA_COM_NO_MEM;
  }

  next_to_use = tx_ring->next_to_use;

  req_id = tx_ring->empty_tx_reqs[next_to_use];
  tx_info = &tx_ring->tx_buffer_info[req_id];
  tx_info->num_of_bufs = 0;

  ena_tx_map_mbuf(tx_ring, tx_info, mbuf, &push_header, &header_len);

  ena_tx_ctx.ena_bufs = tx_info->bufs;
  ena_tx_ctx.push_header = push_header;
  ena_tx_ctx.num_bufs = tx_info->num_of_bufs;
  ena_tx_ctx.req_id = req_id;
  ena_tx_ctx.header_len = header_len;

  /* Set Tx offloads flags, if applicable */
  ena_tx_mbuf_prepare(mbuf, &ena_tx_ctx, tx_ring->offloads,
                      tx_ring->disable_meta_caching);

  if (unlikely(
          ena_com_is_doorbell_needed(tx_ring->ena_com_io_sq, &ena_tx_ctx))) {
    ena_log_raw(DBG,
                "LLQ Tx max burst size of queue %d achieved, writing doorbell "
                "to send burst",
                tx_ring->id);
    ena_com_write_tx_sq_doorbell(tx_ring->ena_com_io_sq);
    tx_ring->tx_stats.doorbells++;
    tx_ring->pkts_without_db = false;
  }

  /* prepare the packet's descriptors to dma engine */
  rc = ena_com_prepare_tx(tx_ring->ena_com_io_sq, &ena_tx_ctx, &nb_hw_desc);
  if (unlikely(rc)) {
    ena_log_raw(ERR, "Failed to prepare Tx buffers, rc: %d", rc);
    ++tx_ring->tx_stats.prepare_ctx_err;
    ena_trigger_reset(tx_ring->adapter, ENA_REGS_RESET_DRIVER_INVALID_STATE);
    return rc;
  }

  tx_info->tx_descs = nb_hw_desc;
  tx_info->timestamp = rte_get_timer_cycles();

  tx_ring->tx_stats.cnt++;
  tx_ring->tx_stats.bytes += mbuf->pkt_len;

  tx_ring->next_to_use = ENA_IDX_NEXT_MASKED(next_to_use, tx_ring->size_mask);

  return 0;
}

static int ena_tx_cleanup(void *txp, uint32_t free_pkt_cnt) {
  rte_mbuf *pkts_to_clean[ENA_CLEANUP_BUF_THRESH];
  struct ena_ring *tx_ring = (struct ena_ring *)txp;
  size_t mbuf_cnt = 0;
  size_t pkt_cnt = 0;
  unsigned int total_tx_descs = 0;
  unsigned int total_tx_pkts = 0;
  uint16_t cleanup_budget;
  uint64_t hw_timestamp = 0;
  uint16_t next_to_clean = tx_ring->next_to_clean;
  bool fast_free = tx_ring->offloads & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

  /*
   * If free_pkt_cnt is equal to 0, it means that the user requested
   * full cleanup, so attempt to release all Tx descriptors
   * (ring_size - 1 -> size_mask)
   */
  cleanup_budget = (free_pkt_cnt == 0) ? tx_ring->size_mask : free_pkt_cnt;

  while (likely(total_tx_pkts < cleanup_budget)) {
    rte_mbuf *mbuf;
    struct ena_tx_buffer *tx_info;
    uint16_t req_id;

    if (ena_com_tx_comp_metadata_get(tx_ring->ena_com_io_cq, &req_id,
                                     &hw_timestamp) != 0)
      break;

    if (unlikely(validate_tx_req_id(tx_ring, req_id) != 0))
      break;

    /* Get Tx info & store how many descs were processed  */
    tx_info = &tx_ring->tx_buffer_info[req_id];
    tx_info->timestamp = 0;

    mbuf = tx_info->mbuf;
    if (fast_free) {
      pkts_to_clean[pkt_cnt++] = mbuf;
      mbuf_cnt += mbuf->nb_segs;
      if (mbuf_cnt >= ENA_CLEANUP_BUF_THRESH) {
        rte_pktmbuf_free_bulk(pkts_to_clean, pkt_cnt);
        mbuf_cnt = 0;
        pkt_cnt = 0;
      }
    } else {
      rte_pktmbuf_free(mbuf);
    }

    tx_info->mbuf = NULL;
    tx_ring->empty_tx_reqs[next_to_clean] = req_id;

    total_tx_descs += tx_info->tx_descs;
    total_tx_pkts++;

    /* Put back descriptor to the ring for reuse */
    next_to_clean = ENA_IDX_NEXT_MASKED(next_to_clean, tx_ring->size_mask);
  }

  if (likely(total_tx_descs > 0)) {
    /* acknowledge completion of sent packets */
    tx_ring->next_to_clean = next_to_clean;
    ena_com_comp_ack(tx_ring->ena_com_io_sq, total_tx_descs);
  }

  if (mbuf_cnt != 0)
    rte_pktmbuf_free_bulk(pkts_to_clean, pkt_cnt);

  /* Notify completion handler that full cleanup was performed */
  if (free_pkt_cnt == 0 || total_tx_pkts < cleanup_budget)
    tx_ring->last_cleanup_ticks = rte_get_timer_cycles();

  return total_tx_pkts;
}

/******************************************************************
 * Interface
 * ****************************************************************/

int ena_eth_dev::mtu_set(uint16_t mtu) {
  ena_adapter *adapter;
  ena_com_dev *ena_dev;
  int rc = 0;
  adapter = get<ena_adapter>();
  ena_dev = &adapter->ena_dev;

  rc = ena_com_set_dev_mtu(ena_dev, mtu);
  if (rc)
    ena_log_raw(ERR, "Could not set MTU: %d", mtu);
  else
    ena_log_raw(NOTICE, "MTU set to: %d", mtu);

  return rc;
}

int ena_eth_dev::start() {
  ena_adapter *adapter = get<ena_adapter>();
  uint64_t ticks __attribute__((unused));
  int rc = 0;
  uint16_t i;

  rc = ena_queue_start_all(this, ENA_RING_TYPE_RX);
  if (rc)
    return rc;

  rc = ena_queue_start_all(this, ENA_RING_TYPE_TX);
  if (rc)
    goto err_start_tx;

  if (data.dev_conf.rxmode.mq_mode & RTE_ETH_MQ_RX_RSS_FLAG) {
    rc = ena_rss_configure(adapter);
    if (rc)
      goto err_rss_init;
  }

  ena_stats_restart(this);

  adapter->timestamp_wd = rte_get_timer_cycles();
  adapter->keep_alive_timeout = ENA_DEVICE_KALIVE_TIMEOUT;

  ticks = rte_get_timer_hz();

  // disable for now
  // rte_timer_reset(&adapter->timer_wd, ticks, ena_timer_wd_callback, this);

  ++adapter->dev_stats.dev_start;
  adapter->state = ENA_ADAPTER_STATE_RUNNING;

  for (i = 0; i < data.nb_rx_queues; i++)
    data.rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
  for (i = 0; i < data.nb_tx_queues; i++)
    data.tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
  return 0;

err_rss_init:
  ena_queue_stop_all(this, ENA_RING_TYPE_TX);
err_start_tx:
  ena_queue_stop_all(this, ENA_RING_TYPE_RX);
  return rc;
}

int ena_eth_dev::stop() {
  ena_adapter *adapter = get<ena_adapter>();
  ena_com_dev *ena_dev = &adapter->ena_dev;
  uint16_t i;
  int rc;
  // rte_timer_stop_sync(&adapter->timer_wd);
  ena_collect_intr_threads(adapter);
  ena_queue_stop_all(this, ENA_RING_TYPE_TX);
  ena_queue_stop_all(this, ENA_RING_TYPE_RX);

  if (adapter->trigger_reset) {
    rc = ena_com_dev_reset(ena_dev, adapter->reset_reason);
    if (rc)
      ena_log_raw(ERR, "Device reset failed, rc: %d", rc);
  }

  ++adapter->dev_stats.dev_stop;
  adapter->state = ENA_ADAPTER_STATE_STOPPED;

  data.dev_started = 0;

  for (i = 0; i < data.nb_rx_queues; i++)
    data.rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
  for (i = 0; i < data.nb_tx_queues; i++)
    data.tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;

  return 0;
}

int ena_eth_dev::tx_queue_setup(uint16_t queue_idx, uint16_t nb_desc,
                                unsigned int socket_id,
                                const struct rte_eth_txconf *tx_conf) {
  ena_ring *txq = NULL;
  ena_adapter *adapter = get<ena_adapter>();
  unsigned int i;
  uint16_t dyn_thresh;

  txq = &adapter->tx_ring[queue_idx];

  if (txq->configured) {
    ena_log_raw(CRIT, "API violation. Queue[%d] is already configured",
                queue_idx);
    return ENA_COM_FAULT;
  }

  if (!rte_is_power_of_2(nb_desc)) {
    ena_log_raw(ERR, "Unsupported size of Tx queue: %d is not a power of 2.",
                nb_desc);
    return -EINVAL;
  }

  if (nb_desc > adapter->max_tx_ring_size) {
    ena_log_raw(ERR, "Unsupported size of Tx queue (max size: %d)",
                adapter->max_tx_ring_size);
    return -EINVAL;
  }

  txq->port_id = data.port_id;
  txq->next_to_clean = 0;
  txq->next_to_use = 0;
  txq->ring_size = nb_desc;
  txq->size_mask = nb_desc - 1;
  txq->numa_socket_id = socket_id;
  txq->pkts_without_db = false;
  txq->last_cleanup_ticks = 0;

  int size = sizeof(struct ena_tx_buffer) * txq->ring_size;
  txq->tx_buffer_info =
      static_cast<ena_tx_buffer *>(aligned_alloc(RTE_CACHE_LINE_SIZE, size));
  if (!txq->tx_buffer_info) {
    ena_log_raw(ERR, "Failed to allocate memory for Tx buffer info");
    return -ENOMEM;
  }
  bzero(txq->tx_buffer_info, size);

  size = sizeof(uint16_t) * txq->ring_size;
  txq->empty_tx_reqs = static_cast<uint16_t *>(calloc(1, size));
  if (!txq->empty_tx_reqs) {
    ena_log_raw(ERR, "Failed to allocate memory for empty Tx requests");
    rte_free(txq->tx_buffer_info);
    return -ENOMEM;
  }

  size = txq->tx_max_header_size;
  txq->push_buf_intermediate_buf = static_cast<uint8_t *>(calloc(1, size));
  if (!txq->push_buf_intermediate_buf) {
    ena_log_raw(ERR, "Failed to alloc push buffer for LLQ");
    rte_free(txq->tx_buffer_info);
    rte_free(txq->empty_tx_reqs);
    return -ENOMEM;
  }

  for (i = 0; i < txq->ring_size; i++)
    txq->empty_tx_reqs[i] = i;

  txq->offloads = tx_conf->offloads | data.dev_conf.txmode.offloads;

  /* Check if caller provided the Tx cleanup threshold value. */
  if (tx_conf->tx_free_thresh != 0) {
    txq->tx_free_thresh = tx_conf->tx_free_thresh;
  } else {
    dyn_thresh = txq->ring_size - txq->ring_size / ENA_REFILL_THRESH_DIVIDER;
    txq->tx_free_thresh =
        RTE_MAX(dyn_thresh, txq->ring_size - ENA_REFILL_THRESH_PACKET);
  }

  txq->missing_tx_completion_threshold =
      RTE_MIN(txq->ring_size / 2, ENA_DEFAULT_MISSING_COMP);

  /* Store pointer to this queue in upper layer */
  txq->configured = 1;
  data.tx_queues[queue_idx] = txq;

  return 0;
}

int ena_eth_dev::rx_queue_setup(uint16_t qid, uint16_t nb_desc,
                                unsigned int socket_id,
                                const rte_eth_rxconf *rx_conf, rte_mempool *mp,
                                irq_handler_cb_t handler) {
  ena_adapter *adapter = get<ena_adapter>();
  struct ena_ring *rxq = NULL;
  size_t buffer_size;
  int i;
  uint16_t dyn_thresh;

  rxq = &adapter->rx_ring[qid];
  if (rxq->configured) {
    ena_log_raw(CRIT, "API violation. Queue[%d] is already configured", qid);
    return ENA_COM_FAULT;
  }

  if (!rte_is_power_of_2(nb_desc)) {
    ena_log_raw(ERR, "Unsupported size of Rx queue: %d is not a power of 2.",
                nb_desc);
    return -EINVAL;
  }

  if (nb_desc > adapter->max_rx_ring_size) {
    ena_log_raw(ERR, "Unsupported size of Rx queue (max size: %d)",
                adapter->max_rx_ring_size);
    return -EINVAL;
  }

  /* ENA isn't supporting buffers smaller than 1400 bytes */
  buffer_size = mp->get_data_size();
  if (buffer_size < ENA_RX_BUF_MIN_SIZE) {
    ena_log_raw(ERR, "Unsupported size of Rx buffer: %zu (min size: %d)",
                buffer_size, ENA_RX_BUF_MIN_SIZE);
    return -EINVAL;
  }

  rxq->port_id = data.port_id;
  rxq->next_to_clean = 0;
  rxq->next_to_use = 0;
  rxq->ring_size = nb_desc;
  rxq->size_mask = nb_desc - 1;
  rxq->numa_socket_id = socket_id;
  rxq->mb_pool = mp;
  rxq->handler = handler;

  rxq->rx_buffer_info = static_cast<ena_rx_buffer *>(
      aligned_alloc(RTE_CACHE_LINE_SIZE, sizeof(ena_rx_buffer) * nb_desc));
  if (!rxq->rx_buffer_info) {
    ena_log_raw(ERR, "Failed to allocate memory for Rx buffer info");
    return -ENOMEM;
  }
  bzero(rxq->rx_buffer_info, sizeof(ena_rx_buffer) * nb_desc);
  rxq->rx_refill_buffer = static_cast<rte_mbuf **>(
      aligned_alloc(RTE_CACHE_LINE_SIZE, sizeof(rte_mbuf *) * nb_desc));
  if (!rxq->rx_refill_buffer) {
    ena_log_raw(ERR, "Failed to allocate memory for Rx refill buffer");
    rte_free(rxq->rx_buffer_info);
    rxq->rx_buffer_info = NULL;
    return -ENOMEM;
  }

  rxq->empty_rx_reqs = static_cast<uint16_t *>(
      aligned_alloc(RTE_CACHE_LINE_SIZE, sizeof(uint16_t) * nb_desc));
  if (!rxq->empty_rx_reqs) {
    ena_log_raw(ERR, "Failed to allocate memory for empty Rx requests");
    rte_free(rxq->rx_buffer_info);
    rxq->rx_buffer_info = NULL;
    rte_free(rxq->rx_refill_buffer);
    rxq->rx_refill_buffer = NULL;
    return -ENOMEM;
  }

  for (i = 0; i < nb_desc; i++)
    rxq->empty_rx_reqs[i] = i;

  rxq->offloads = rx_conf->offloads | data.dev_conf.rxmode.offloads;

  if (rx_conf->rx_free_thresh != 0) {
    rxq->rx_free_thresh = rx_conf->rx_free_thresh;
  } else {
    dyn_thresh = rxq->ring_size / ENA_REFILL_THRESH_DIVIDER;
    rxq->rx_free_thresh =
        RTE_MIN(dyn_thresh, (uint16_t)(ENA_REFILL_THRESH_PACKET));
  }

  /* Store pointer to this queue in upper layer */
  rxq->configured = 1;
  data.rx_queues[qid] = rxq;

  return 0;
}

TRACEPOINT(trace_ena_eth_dev_tx_burst, "qid=%x, tx_pkts=%y, nb_pkts=%z",
           uint16_t, rte_mbuf **, uint16_t);
TRACEPOINT(trace_ena_eth_dev_tx_burst_ret, "");
uint16_t tx_burst(rte_eth_dev *dev, uint16_t qid, rte_mbuf **tx_pkts,
                  uint16_t nb_pkts) {
  trace_ena_eth_dev_tx_burst(qid, tx_pkts, nb_pkts);
  if (qid >= dev->data.nb_tx_queues)
    return 0;
  ena_ring *tx_ring = static_cast<ena_ring *>(dev->data.tx_queues[qid]);
  int available_desc;
  uint16_t sent_idx = 0;

#ifdef RTE_ETHDEV_DEBUG_TX
  /* Check adapter state */
  if (unlikely(tx_ring->adapter->state != ENA_ADAPTER_STATE_RUNNING)) {
    ena_log_raw(ALERT, "Trying to xmit pkts while device is NOT running");
    return 0;
  }
#endif

  available_desc = ena_com_free_q_entries(tx_ring->ena_com_io_sq);
  if (available_desc < tx_ring->tx_free_thresh)
    ena_tx_cleanup((void *)tx_ring, 0);

  for (sent_idx = 0; sent_idx < nb_pkts; sent_idx++) {
    if (ena_xmit_mbuf(tx_ring, tx_pkts[sent_idx]))
      break;
    tx_ring->pkts_without_db = true;
    rte_prefetch0(tx_pkts[ENA_IDX_ADD_MASKED(sent_idx, 4, tx_ring->size_mask)]);
  }

  /* If there are ready packets to be xmitted... */
  if (likely(tx_ring->pkts_without_db)) {
    /* ...let HW do its best :-) */
    ena_com_write_tx_sq_doorbell(tx_ring->ena_com_io_sq);
    tx_ring->tx_stats.doorbells++;
    tx_ring->pkts_without_db = false;
  }

  tx_ring->tx_stats.available_desc =
      ena_com_free_q_entries(tx_ring->ena_com_io_sq);
  tx_ring->tx_stats.tx_poll++;

  return sent_idx;
}

TRACEPOINT(trace_ena_eth_dev_rx_burst, "qid=%x, rx_pkts=%y, nb_pkts=%z",
           uint16_t, rte_mbuf **, uint16_t);
TRACEPOINT(trace_ena_eth_dev_rx_burst_ret, "");
uint16_t ena_rx_burst(rte_eth_dev *dev, uint16_t qid, rte_mbuf **rx_pkts,
                      uint16_t nb_pkts) {
  trace_ena_eth_dev_rx_burst(qid, rx_pkts, nb_pkts);
  if (qid >= dev->data.nb_rx_queues)
    return 0;
  ena_ring *rx_ring = static_cast<ena_ring *>(dev->data.rx_queues[qid]);
  unsigned int free_queue_entries;
  uint16_t next_to_clean = rx_ring->next_to_clean;
  enum ena_regs_reset_reason_types reset_reason;
  uint16_t descs_in_use;
  rte_mbuf *mbuf;
  uint16_t completed;
  struct ena_com_rx_ctx ena_rx_ctx;
  int i, rc = 0;

#ifdef RTE_ETHDEV_DEBUG_RX
  /* Check adapter state */
  if (unlikely(rx_ring->adapter->state != ENA_ADAPTER_STATE_RUNNING)) {
    ena_log_raw(ALERT, "Trying to receive pkts while device is NOT running");
    return 0;
  }
#endif

  descs_in_use =
      rx_ring->ring_size - ena_com_free_q_entries(rx_ring->ena_com_io_sq) - 1;
  nb_pkts = RTE_MIN(descs_in_use, nb_pkts);

  for (completed = 0; completed < nb_pkts; completed++) {
    ena_rx_ctx.max_bufs = rx_ring->sgl_size;
    ena_rx_ctx.ena_bufs = rx_ring->ena_bufs;
    ena_rx_ctx.descs = 0;
    ena_rx_ctx.pkt_offset = 0;
    /* receive packet context */
    rc = ena_com_rx_pkt(rx_ring->ena_com_io_cq, rx_ring->ena_com_io_sq,
                        &ena_rx_ctx);
    if (unlikely(rc)) {
      ena_log_raw(ERR, "Failed to get the packet from the device, rc: %d", rc);
      switch (rc) {
      case ENA_COM_NO_SPACE:
        ++rx_ring->rx_stats.bad_desc_num;
        reset_reason = ENA_REGS_RESET_TOO_MANY_RX_DESCS;
        break;
      case ENA_COM_FAULT:
        ++rx_ring->rx_stats.bad_desc;
        reset_reason = ENA_REGS_RESET_RX_DESCRIPTOR_MALFORMED;
        break;
      case ENA_COM_EIO:
        ++rx_ring->rx_stats.bad_req_id;
        reset_reason = ENA_REGS_RESET_INV_RX_REQ_ID;
        break;
      default:
        ++rx_ring->rx_stats.unknown_error;
        reset_reason = ENA_REGS_RESET_DRIVER_INVALID_STATE;
        break;
      }
      ena_trigger_reset(rx_ring->adapter, reset_reason);
      return 0;
    }

    mbuf = ena_rx_mbuf(rx_ring, ena_rx_ctx.ena_bufs, ena_rx_ctx.descs,
                       &next_to_clean, ena_rx_ctx.pkt_offset);
    if (unlikely(mbuf == NULL)) {
      for (i = 0; i < ena_rx_ctx.descs; ++i) {
        rx_ring->empty_rx_reqs[next_to_clean] = rx_ring->ena_bufs[i].req_id;
        next_to_clean = ENA_IDX_NEXT_MASKED(next_to_clean, rx_ring->size_mask);
      }
      break;
    }

    ena_log_io(adapter->pdev, INFO,
               "rx: q %d got packet from ena. descs #: %d l3 proto %d l4 proto "
               "%d hash: %x",
               qid, ena_rx_ctx.descs, ena_rx_ctx.l3_proto, ena_rx_ctx.l4_proto,
               ena_rx_ctx.hash);

    /* fill mbuf attributes if any */
    ena_rx_mbuf_prepare(rx_ring, mbuf, &ena_rx_ctx);

    if (unlikely(mbuf->ol_flags &
                 (RTE_MBUF_F_RX_IP_CKSUM_BAD | RTE_MBUF_F_RX_L4_CKSUM_BAD)))
      rte_atomic64_inc(&rx_ring->adapter->drv_stats->ierrors);

    rx_pkts[completed] = mbuf;
    rx_ring->rx_stats.bytes += mbuf->pkt_len;
  }

  rx_ring->rx_stats.cnt += completed;
  rx_ring->next_to_clean = next_to_clean;

  free_queue_entries = ena_com_free_q_entries(rx_ring->ena_com_io_sq);

  /* Burst refill to save doorbells, memory barriers, const interval */
  if (free_queue_entries >= rx_ring->rx_free_thresh) {
    ena_populate_rx_queue(rx_ring, free_queue_entries);
  }
  trace_ena_eth_dev_rx_burst_ret();
  return completed;
}

/*
 * stats
 */

static void ena_copy_customer_metrics(struct ena_adapter *adapter,
                                      uint64_t *buf, size_t num_metrics) {
  struct ena_com_dev *ena_dev = &adapter->ena_dev;
  int rc;

  if (ena_com_get_cap(ena_dev, ENA_ADMIN_CUSTOMER_METRICS)) {
    if (num_metrics != ENA_STATS_ARRAY_METRICS) {
      ena_log_raw(ERR,
                  "Detected discrepancy in the number of customer metrics");
      return;
    }
    rte_spinlock_lock(&adapter->admin_lock);
    rc = ena_com_get_customer_metrics(&adapter->ena_dev, (char *)buf,
                                      num_metrics * sizeof(uint64_t));
    rte_spinlock_unlock(&adapter->admin_lock);
    if (rc != 0) {
      ena_log_raw(WARN, "Failed to get customer metrics, rc: %d", rc);
      return;
    }

  } else if (ena_com_get_cap(ena_dev, ENA_ADMIN_ENI_STATS)) {
    if (num_metrics != ENA_STATS_ARRAY_METRICS_LEGACY) {
      ena_log_raw(ERR, "Detected discrepancy in the number of legacy metrics");
      return;
    }

    rte_spinlock_lock(&adapter->admin_lock);
    rc = ena_com_get_eni_stats(&adapter->ena_dev,
                               (struct ena_admin_eni_stats *)buf);
    rte_spinlock_unlock(&adapter->admin_lock);
    if (rc != 0) {
      ena_log_raw(WARN, "Failed to get ENI metrics, rc: %d", rc);
      return;
    }
  }
}

/*********************************************************************
 *  platform functions implementations
 *********************************************************************/

int ena_dma_alloc(rte_eth_dev_data *dmadev, bus_size_t size,
                  ena_mem_handle_t *dma, int mapflags, bus_size_t alignment,
                  int domain) {
  auto pa = mem::frames::alloc(size, mem::mapping::page_size);
  if (pa == mem::frames::no_memory) {
    ena_log(pdev, ERR, "no memory for a DMA buffer!", 1);
    dma->vaddr = 0;
    dma->paddr = 0;
    return ENA_COM_NO_MEM;
  }
  dma->vaddr = (caddr_t)mem::map_phys(pa, size);

  /* Match dma_alloc_coherent() semantics on Linux/FreeBSD: hand back
   * zeroed memory. ena_com's admin CQ polls the phase bit of each entry
   * and treats a match as a real completion — uninitialised bytes trip
   * that and the first bogus completion flips the queue to
   * running_state=false, after which every admin command returns
   * -ENODEV. */
  memset(dma->vaddr, 0, size);

  dma->paddr = pa;

  return (0);
}

static void ena_free_pci_resources(ena_adapter *adapter) {
  if (adapter->regs)
    adapter->regs->unmap();
  if (adapter->dev_mem)
    adapter->dev_mem->unmap();
}

bool ena_probe(pci::device *pdev) {
  ena_vendor_info_t *ent = ena_vendor_info_array;
  while (ent->vendor_id != 0) {
    if (pdev->get_id() == hw_device_id(ent->vendor_id, ent->device_id)) {
      ena_log_raw(INFO, "vendor=%x device=%x\n", ent->vendor_id,
                  ent->device_id);

      return true;
    }
    ent++;
  }
  return false;
}

int ena_attach(pci::device *dev, ena_adapter **_adapter) {
  struct ena_adapter *adapter;
  struct ena_com_dev_get_features_ctx get_feat_ctx;
  struct ena_calc_queue_size_ctx calc_queue_ctx = {0};
  struct ena_llq_configurations llq_config;
  struct ena_com_dev *ena_dev;
  ena_eth_dev *edev;
  uint32_t max_num_io_queues;
  int rc;

  adapter = aligned_new<ena_adapter>();
  *_adapter = adapter;
  adapter->dev = dev;
  ena_dev = &adapter->ena_dev;
  edev = static_cast<ena_eth_dev *>(calloc(1, sizeof(ena_eth_dev)));
  adapter->edev = edev;
  new (adapter->edev) ena_eth_dev(adapter);
  // rte_timer_init(&adapter->timer_wd);

  eth_os::register_port(adapter->edev);
  const char *queue_type_str;
  static int adapters_found;
  bool disable_meta_caching;
  bool use_large_llq_hdr;

  adapter->regs = dev->get_bar(ENA_REGS_BAR + 1);
  adapter->dev_mem = dev->get_bar(ENA_MEM_BAR + 1);
  if (!adapter->regs) {
    ena_log_raw(CRIT, "Failed to access registers BAR(%d)", ENA_REGS_BAR);
    return -ENXIO;
  }
  adapter->regs->map();
  ena_dev->reg_bar =
      static_cast<u8 *>(const_cast<void *>(adapter->regs->get_mmio()));
  /* Pass device data as rx_free_thresha pointer which can be passed to the IO
   * functions by the ena_com (for example - the memory allocation).
   */
  ena_dev->dmadev = &edev->data;
  adapter->id_number = adapters_found;

  snprintf(adapter->name, ENA_NAME_MAX_LEN, "ena_%d", adapter->id_number);

  /* Assign default devargs values */
  adapter->missing_tx_completion_to = ENA_TX_TIMEOUT;
  // LLQ is ENA's TX fast path: the descriptor and inline header go straight
  // into the device BAR, mapped write-combining (ena_device_init). Uncached
  // that cost ~3.5 us a frame; host-memory queues instead (2026-09-17) made
  // the device's completion rate the ceiling, ~25 Gbps on 8 and 16 queues.
  adapter->llq_header_policy = ENA_LLQ_POLICY_RECOMMENDED;

  rc = ena_com_allocate_customer_metrics_buffer(ena_dev);
  if (rc != 0) {
    ena_log_raw(CRIT, "Failed to allocate customer metrics buffer");
    goto err;
  }

  /* device specific initialization routine */
  rc = ena_device_init(adapter, dev, &get_feat_ctx);
  if (rc) {
    ena_log_raw(CRIT, "Failed to init ENA device");
    goto err_metrics_delete;
  }

  use_large_llq_hdr =
      ena_use_large_llq_hdr(adapter, get_feat_ctx.llq.entry_size_recommended);
  set_default_llq_configurations(&llq_config, &get_feat_ctx.llq,
                                 use_large_llq_hdr);
  rc = ena_set_queues_placement_policy(adapter, ena_dev, &get_feat_ctx.llq,
                                       &llq_config);
  if (unlikely(rc)) {
    ena_log_raw(CRIT, "Failed to set placement policy");
    goto err_metrics_delete;
  }

  if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_HOST) {
    queue_type_str = "Regular";
  } else {
    queue_type_str = "Low latency";
    ena_log_raw(INFO, "LLQ entry size %uB",
                llq_config.llq_ring_entry_size_value);
  }
  ena_log_raw(INFO, "Placement policy: %s", queue_type_str);
  calc_queue_ctx.ena_dev = ena_dev;
  calc_queue_ctx.get_feat_ctx = &get_feat_ctx;

  max_num_io_queues = ena_calc_max_io_queue_num(ena_dev, &get_feat_ctx);
  rc = ena_calc_io_queue_size(&calc_queue_ctx, use_large_llq_hdr);
  if (unlikely((rc != 0) || (max_num_io_queues == 0))) {
    rc = -EFAULT;
    goto err_device_destroy;
  }

  adapter->max_tx_ring_size = calc_queue_ctx.max_tx_queue_size;
  adapter->max_rx_ring_size = calc_queue_ctx.max_rx_queue_size;
  adapter->max_tx_sgl_size = calc_queue_ctx.max_tx_sgl_size;
  adapter->max_rx_sgl_size = calc_queue_ctx.max_rx_sgl_size;
  adapter->max_num_io_queues = max_num_io_queues;

  if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
    disable_meta_caching =
        !!(get_feat_ctx.llq.accel_mode.u.get.supported_flags &
           BIT(ENA_ADMIN_DISABLE_META_CACHING));
  } else {
    disable_meta_caching = false;
  }

  rte_spinlock_init(&adapter->admin_lock);
  aenq_handlers.handlers[static_cast<int>(ENA_ADMIN_LINK_CHANGE)] =
      ena_update_on_link_change;
  aenq_handlers.handlers[static_cast<int>(ENA_ADMIN_NOTIFICATION)] =
      ena_notification;
  aenq_handlers.handlers[static_cast<int>(ENA_ADMIN_KEEP_ALIVE)] =
      ena_keep_alive;
  aenq_handlers.handlers[static_cast<int>(ENA_ADMIN_CONF_NOTIFICATIONS)] =
      ena_suboptimal_configuration;
  aenq_handlers.unimplemented_handler = unimplemented_aenq_handler;
  rc = ena_enable_msix_and_set_admin_interrupts(adapter);
  if (unlikely(rc != 0)) {
    ena_log(pdev, ERR, "Failed to enable and set the admin interrupts");
    goto err_device_destroy;
  }
  /* prepare ring structures */
  ena_init_rings(adapter, disable_meta_caching);

  ena_config_debug_area(adapter);

  /* Set max MTU for this device */
  adapter->max_mtu = get_feat_ctx.dev_attr.max_mtu;

  ena_set_offloads(&adapter->offloads, &get_feat_ctx.offload);

  /* Copy MAC address and point DPDK to it */
  memcpy(adapter->mac_addr, get_feat_ctx.dev_attr.mac_addr,
         sizeof(adapter->mac_addr));
  memcpy(edev->data.mac_addr.addr.data(), adapter->mac_addr,
         sizeof(adapter->mac_addr));

  rc = ena_com_rss_init(ena_dev);
  if (unlikely(rc != 0)) {
    ena_log_raw(ERR, "Failed to initialize RSS in ENA device");
    goto err_delete_debug_area;
  }

  adapter->drv_stats =
      static_cast<ena_driver_stats *>(calloc(1, sizeof(*adapter->drv_stats)));
  if (!adapter->drv_stats) {
    ena_log_raw(ERR, "Failed to allocate memory for adapter statistics");
    rc = -ENOMEM;
    goto err_rss_destroy;
  }

  adapters_found++;
  adapter->state = ENA_ADAPTER_STATE_INIT;
  adapter->edev->tx_burst = tx_burst;
  adapter->edev->rx_burst = ena_rx_burst;
  return 0;

err_rss_destroy:
  ena_com_rss_destroy(ena_dev);
err_delete_debug_area:
  ena_com_delete_debug_area(ena_dev);

err_device_destroy:
  ena_com_delete_host_info(ena_dev);
  ena_com_admin_destroy(ena_dev);
err_metrics_delete:
  ena_com_delete_customer_metrics_buffer(ena_dev);
err:
  return rc;
}

int ena_detach(ena_adapter *adapter) {
  ena_com_dev *ena_dev = &adapter->ena_dev;
  int ret = 0;
  ena_log_raw(INFO, "Detaching\n");

  if (adapter->state == ENA_ADAPTER_STATE_CLOSED)
    return 0;

  if (adapter->state == ENA_ADAPTER_STATE_RUNNING)
    ret = adapter->edev->stop();
  adapter->state = ENA_ADAPTER_STATE_CLOSED;
  /* Stop timer service */
  // rte_timer_stop_sync(&adapter->timer_wd);

  ena_rx_queue_release_all(adapter->edev);
  ena_tx_queue_release_all(adapter->edev);

  free(adapter->drv_stats);
  adapter->drv_stats = NULL;

  ena_com_set_admin_running_state(ena_dev, false);

  ena_com_rss_destroy(ena_dev);

  ena_com_delete_debug_area(ena_dev);
  ena_com_delete_host_info(ena_dev);

  ena_com_abort_admin_commands(ena_dev);
  ena_com_wait_for_abort_completion(ena_dev);
  ena_com_admin_destroy(ena_dev);
  ena_com_mmio_reg_read_request_destroy(ena_dev);
  ena_com_delete_customer_metrics_buffer(ena_dev);
  ena_com_delete_host_info(ena_dev);

  ena_free_irqs(adapter);
  ena_free_pci_resources(adapter);
  adapter->edev->~ena_eth_dev();
  free(adapter);

  return ret;
}
/*********************************************************************
 *
 *  MSIX & Interrupt Service routine
 *
 **********************************************************************/
/**
 * ena_intr_msix_mgmnt - MSIX Interrupt Handler for admin/async queue
 * @arg: interrupt number
 **/

static void ena_rx_cleanup(ena_ring *rxq, rte_eth_dev *dev,
                           irq_handler_cb_t handler) {
  static constexpr unsigned kDefaultBurstSize = 32;
  static constexpr unsigned kDefaultCleanupBudget = 1024;
  rte_mbuf *rxb[kDefaultBurstSize];
  while (1) {
    sched::thread::current()->wait_for([rxq] {
      return rxq->rx_pkts_ready.load(std::memory_order_acquire) ||
             rxq->should_stop.load(std::memory_order_acquire);
    });
    if (rxq->should_stop.load(std::memory_order_acquire))
      return;
    unsigned rx_n = 0;
    do {
      auto rx_nb = ena_rx_burst(dev, rxq->id, rxb, kDefaultBurstSize);
      if (!rx_nb)
        break;
      rx_n += rx_nb;
      handler(rxb, rx_nb);
    } while (rx_n < kDefaultCleanupBudget);
    rxq->rx_pkts_ready.store(false, std::memory_order_release);
    ena_rx_queue_intr_set(rxq, true);
  }
}

/* Re-arm (unmask) a single RX queue's interrupt at the device. */
static void ena_rx_queue_intr_set(ena_ring *rx_ring, bool unmask,
                                  u32 rx_delay) {
  struct ena_eth_io_intr_reg intr_reg;
  ena_com_update_intr_reg(&intr_reg, 0, rx_delay, unmask, rx_delay != 0,
                          /*lost_interrupt*/ false);
  ena_com_unmask_intr(rx_ring->ena_com_io_cq, &intr_reg);
}

static void ena_intr_msix_mgmnt(void *arg) {
  struct ena_adapter *adapter = static_cast<ena_adapter *>(arg);
  ena_com_admin_q_comp_intr_handler(&adapter->ena_dev);
  if (likely(adapter->state == ENA_ADAPTER_STATE_RUNNING))
    ena_com_aenq_intr_handler(&adapter->ena_dev, arg);
}

static int ena_enable_msix(ena_adapter *adapter) {
  pci::device *dev = adapter->dev;
  int msix_vecs = ENA_ADMIN_MSIX_VEC;
  ena_log(dev, DBG, "trying to enable MSI-X, vectors: %d", msix_vecs);
  dev->set_bus_master(true);
  dev->msix_enable();
  assert(dev->is_msix());
  if (msix_vecs > dev->msix_get_num_entries()) {
    if (dev->msix_get_num_entries() < ENA_ADMIN_MSIX_VEC + 1) {
      ena_log(dev, ERR, "Not enough MSI-X vectors allocated: %d",
              dev->msix_get_num_entries());
      dev->msix_disable();
      return ENOSPC;
    }
    ena_log(dev, ERR,
            "Enable only %d MSI-x (out of %d), reduce "
            "the number of queues",
            dev->msix_get_num_entries(), msix_vecs);
    msix_vecs = dev->msix_get_num_entries();
  }
  adapter->msix_vecs = msix_vecs;
  return (0);
}

static void ena_handle_msix(ena_ring *rx_ring) {
  rx_ring->interrupts++;
  rx_ring->rx_pkts_ready.store(true, std::memory_order_release);
  auto *t = rx_ring->intr_thread;
  if (t)
    t->wake_with_irq_disabled();
}

static int ena_request_io_irq(struct ena_adapter *adapter) {
  assert(adapter->edev->data.dev_conf.intr_conf.rxq);
  interrupt_manager msi(adapter->dev);
  auto *dev = adapter->dev;
  adapter->msix_vecs += adapter->edev->data.nb_rx_queues;
  auto msix_vecs = adapter->msix_vecs;
  assert(dev->is_msix());
  if (msix_vecs > dev->msix_get_num_entries()) {
    ena_log(dev, ERR, "Not enough MSI-X vectors allocated: %d",
            dev->msix_get_num_entries());
    dev->msix_disable();
    return ENOSPC;
  }

  int vec_num = adapter->msix_vecs - 1;
  unsigned nb_rx = adapter->edev->data.nb_rx_queues;
  if ((int)nb_rx > vec_num)
    nb_rx = vec_num;

  std::vector<msix_vector *> assigned = msi.request_vectors(vec_num);
  if ((int)assigned.size() != vec_num) {
    msi.free_vectors(assigned);
    ena_log(adapter->dev, ERR, "could not request %d I/O irq vectors", vec_num);
    return (ENXIO);
  }

  for (unsigned idx = 0; idx < nb_rx; idx++) {
    int entry = ENA_IO_IRQ_IDX(idx);
    auto vec = assigned[idx];
    auto queue = &adapter->rx_ring[idx];
    if (!msi.assign_isr(vec, [queue]() { ena_handle_msix(queue); })) {
      msi.free_vectors(assigned);
      ena_log(adapter->dev, ERR, "could not assign I/O irq vector isr: %d",
              entry);
      return (ENXIO);
    }

    if (!msi.setup_entry(entry, vec)) {
      msi.free_vectors(assigned);
      ena_log(adapter->dev, ERR, "could not setup I/O irq vector entry: %d",
              entry);
      return (ENXIO);
    }

    ena_irq *irq = &adapter->irq_tbl[entry];
    irq->vector = entry;
    irq->mvec = vec;
    irq->data = queue;
    irq->requested = true;
    irq->cpu = idx % sched::cpus.size();

    std::atomic_thread_fence(std::memory_order_seq_cst);
    vec->set_affinity(sched::cpus[irq->cpu]);

    ena_log(adapter->dev, INFO, "pinned MSIX vector on queue %d - cpu %d", idx,
            irq->cpu);
  }

  std::atomic_thread_fence(std::memory_order_seq_cst);
  msi.unmask_interrupts(assigned);

  /* Arm each queue so the device will raise its RX interrupt. */
  for (unsigned idx = 0; idx < nb_rx; idx++)
    ena_rx_queue_intr_set(&adapter->rx_ring[idx], true);

  return 0;
}

static int ena_request_mgmnt_irq(ena_adapter *adapter) {
  interrupt_manager msi(adapter->dev);

  std::vector<msix_vector *> assigned = msi.request_vectors(1);
  if (assigned.size() != 1) {
    msi.free_vectors(assigned);
    ena_log(adapter->dev, ERR, "could not request MGMNT irq vector: %d",
            ENA_MGMNT_IRQ_IDX);
    return (ENXIO);
  }

  auto vec = assigned[0];
  // should be pinned
  if (!msi.assign_isr(vec, [adapter]() { ena_intr_msix_mgmnt(adapter); })) {
    msi.free_vectors(assigned);
    ena_log(adapter->dev, ERR, "could not assign MGMNT irq vector isr: %d",
            ENA_MGMNT_IRQ_IDX);
    return (ENXIO);
  }

  if (!msi.setup_entry(ENA_MGMNT_IRQ_IDX, vec)) {
    msi.free_vectors(assigned);
    ena_log(adapter->dev, ERR, "could not setup MGMNT irq vector entry: %d",
            ENA_MGMNT_IRQ_IDX);
    return (ENXIO);
  }

  ena_irq *irq = &adapter->irq_tbl[ENA_MGMNT_IRQ_IDX];
  irq->vector = ENA_MGMNT_IRQ_IDX;
  irq->mvec = vec;
  irq->data = adapter;
  irq->requested = true;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  vec->set_affinity(sched::current_cpu);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  msi.unmask_interrupts(assigned);

  return 0;
}

static void ena_disable_msix(struct ena_adapter *adapter) {
  adapter->dev->msix_disable();
}

static void ena_free_irqs(ena_adapter *adapter) {
  if (adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].mvec)
    delete adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].mvec;
  ena_disable_msix(adapter);
}

static int ena_enable_msix_and_set_admin_interrupts(ena_adapter *adapter) {
  auto *ena_dev = &adapter->ena_dev;
  int rc;

  rc = ena_enable_msix(adapter);
  if (unlikely(rc != 0)) {
    ena_log(adapter->pdev, ERR, "Error with MSI-X enablement");
    return (rc);
  }

  rc = ena_request_mgmnt_irq(adapter);
  if (unlikely(rc != 0)) {
    ena_log(adapter->pdev, ERR, "Cannot setup mgmnt queue intr");
    goto err_disable_msix;
  }

  ena_com_set_admin_polling_mode(ena_dev, false);

  ena_com_admin_aenq_enable(ena_dev);

  return (0);

err_disable_msix:
  ena_disable_msix(adapter);

  return (rc);
}

static bool ena_use_large_llq_hdr(struct ena_adapter *adapter,
                                  uint8_t recommended_entry_size) {
  if (adapter->llq_header_policy == ENA_LLQ_POLICY_LARGE) {
    return true;
  } else if (adapter->llq_header_policy == ENA_LLQ_POLICY_RECOMMENDED) {
    ena_log_raw(INFO, "Recommended device entry size policy %u",
                recommended_entry_size);
    if (recommended_entry_size == ENA_ADMIN_LIST_ENTRY_SIZE_256B)
      return true;
  }
  return false;
}

int ena_dev_configure(rte_eth_dev *dev) {
  auto *adapter = dev->get<ena_adapter>();
  adapter->last_tx_comp_qid = 0;

  adapter->missing_tx_completion_budget =
      RTE_MIN(ENA_MONITORED_TX_QUEUES, dev->data.nb_tx_queues);
  adapter->tx_cleanup_stall_delay = adapter->missing_tx_completion_to / 2;
  return 0;
}

int ena_eth_dev::get_dev_info(rte_eth_dev_info *info) {
  return ena_infos_get(this, info);
}

int ena_eth_dev::drv_configure() { return ena_dev_configure(this); }

void ena_eth_dev::get_stats(rte_eth_stats *stats) {
  ena_stats_get(this, stats);
}
