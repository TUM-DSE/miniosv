#ifndef NET_ETH_DEF_H
#define NET_ETH_DEF_H

#include "api/minidpdk/mem.hh"
#include <api/minidpdk/rss.hh>
#include <atomic>
#include <minidpdk/bit.hh>
#include <minidpdk/net.hh>
#include <cstdint>
#include <cstring>
#include <features.h>
#include <osv/types.h>
#include <vector>

struct rte_eth_dev;

struct eth_os {
  std::vector<rte_eth_dev *> ifs;
  static eth_os instance;
  static __inline rte_eth_dev *get_eth_for_port(uint16_t port) {
    if (instance.ifs.size() <= port)
      return nullptr;
    return instance.ifs[port];
  }
  static inline void register_port(rte_eth_dev *dev) {
    instance.ifs.push_back(dev);
  }
};
inline eth_os eth_os::instance;

#define RTE_ETHER_CRC_LEN 4
#define RTE_ETHER_HDR_LEN 14
#define RTE_ETHDEV_QUEUE_STAT_CNTRS 16

enum queue_state {
  RTE_ETH_QUEUE_STATE_STOPPED = 0,
  RTE_ETH_QUEUE_STATE_STARTED
};
#define RTE_ETH_MQ_RX_RSS_FLAG RTE_BIT32(0)
#define RTE_ETH_MQ_RX_DCB_FLAG RTE_BIT32(1)
#define RTE_ETH_MQ_RX_VMDQ_FLAG RTE_BIT32(2)

enum rte_eth_rx_mq_mode {
  RTE_ETH_MQ_RX_NONE = 0,

  RTE_ETH_MQ_RX_RSS = RTE_ETH_MQ_RX_RSS_FLAG,
  RTE_ETH_MQ_RX_DCB = RTE_ETH_MQ_RX_DCB_FLAG,
  RTE_ETH_MQ_RX_DCB_RSS = RTE_ETH_MQ_RX_RSS_FLAG | RTE_ETH_MQ_RX_DCB_FLAG,

  RTE_ETH_MQ_RX_VMDQ_ONLY = RTE_ETH_MQ_RX_VMDQ_FLAG,
  RTE_ETH_MQ_RX_VMDQ_RSS = RTE_ETH_MQ_RX_RSS_FLAG | RTE_ETH_MQ_RX_VMDQ_FLAG,
  RTE_ETH_MQ_RX_VMDQ_DCB = RTE_ETH_MQ_RX_VMDQ_FLAG | RTE_ETH_MQ_RX_DCB_FLAG,
  RTE_ETH_MQ_RX_VMDQ_DCB_RSS =
      RTE_ETH_MQ_RX_RSS_FLAG | RTE_ETH_MQ_RX_DCB_FLAG | RTE_ETH_MQ_RX_VMDQ_FLAG,
};

enum rte_eth_tx_mq_mode {
  RTE_ETH_MQ_TX_NONE = 0,
  RTE_ETH_MQ_TX_DCB,
  RTE_ETH_MQ_TX_VMDQ_DCB,
  RTE_ETH_MQ_TX_VMDQ_ONLY,
};

struct rte_eth_stats {
  std::atomic<uint64_t> ipackets;
  std::atomic<uint64_t> opackets;
  std::atomic<uint64_t> ibytes;
  std::atomic<uint64_t> obytes;
  std::atomic<uint64_t> imissed;
  std::atomic<uint64_t> ierrors;
  std::atomic<uint64_t> oerrors;
  std::atomic<uint64_t> rx_nombuf;
  uint64_t q_ipackets[RTE_ETHDEV_QUEUE_STAT_CNTRS];
  uint64_t q_opackets[RTE_ETHDEV_QUEUE_STAT_CNTRS];
  uint64_t q_ibytes[RTE_ETHDEV_QUEUE_STAT_CNTRS];
  uint64_t q_obytes[RTE_ETHDEV_QUEUE_STAT_CNTRS];
  uint64_t q_errors[RTE_ETHDEV_QUEUE_STAT_CNTRS];
};

struct rte_eth_txconf {
  uint64_t offloads;
  uint64_t tx_free_thresh;
};

struct rte_eth_rxconf {
  enum rte_eth_rx_mq_mode mq_mode;
  uint64_t offloads;
  uint64_t rx_free_thresh;
};

struct rte_eth_adv_rxconf {
  rte_eth_rss_conf rss_conf;
};

struct rte_eth_desc_lim {
  uint16_t nb_max, nb_min, nb_seg_max;
  uint16_t nb_mtu_seg_max;
};

struct rte_eth_conf {
  rte_eth_rxconf rxmode;
  rte_eth_txconf txmode;
  rte_eth_adv_rxconf rx_adv_conf;
  struct{
      uint16_t rxq = 0;
  } intr_conf;
};

struct rte_eth_dev_portconf {
    uint16_t burst_size; 
    uint16_t ring_size; 
    uint16_t nb_queues; 
};

struct rte_eth_dev_info {
  uint16_t min_mtu;
  uint16_t max_mtu;
  uint32_t min_rx_bufsize;
  uint32_t max_rx_bufsize;
  uint32_t max_rx_pktlen;
  uint16_t max_rx_queues;
  uint16_t max_tx_queues;
  uint64_t rx_offload_capa;
  uint64_t tx_offload_capa;
  uint64_t rx_queue_offload_capa;
  uint64_t tx_queue_offload_capa;
  uint16_t reta_size;
  uint16_t max_mac_addrs;
  uint8_t hash_key_size;
  uint32_t rss_algo_capa;
  uint64_t flow_type_rss_offloads;
  rte_eth_rxconf default_rxconf;
  rte_eth_txconf default_txconf;
  uint16_t vmdq_queue_base;
  uint16_t vmdq_queue_num;
  uint16_t vmdq_pool_base;

  rte_eth_desc_lim rx_desc_lim;
  rte_eth_desc_lim tx_desc_lim;
  uint16_t nb_rx_queues;
  uint16_t nb_tx_queues;
  struct rte_eth_dev_portconf default_rxportconf;
  struct rte_eth_dev_portconf default_txportconf;
};

struct rte_eth_dev_data {
  uint16_t nb_rx_queues, nb_tx_queues;
  uint16_t port_id;
  rte_eth_conf dev_conf;
  void *data;
  int dev_started = 0;
  std::vector<enum queue_state> rx_queue_state;
  std::vector<enum queue_state> tx_queue_state;
  std::vector<void *> tx_queues;
  std::vector<void *> rx_queues;
  rte_ether_addr mac_addr;

  rte_eth_dev_data(void *data) : data(data) {}

  template <typename T> T *get() { return static_cast<T *>(data); }
};
using irq_handler_cb_t = std::function<void(rte_mbuf**, uint16_t)>;

struct rte_eth_dev {
  using tx_burst_t = uint16_t (*) (rte_eth_dev*, uint16_t, rte_mbuf**, uint16_t);  
  using rx_burst_t = uint16_t (*) (rte_eth_dev*, uint16_t, rte_mbuf**, uint16_t);
  rte_eth_dev_data data;
  template <typename T> T *get() { return static_cast<T *>(data.get<T>()); }
  rte_eth_dev(void *dev_data) : data(dev_data) {}

  virtual ~rte_eth_dev() = default;
  virtual int mtu_set(uint16_t mtu) = 0;
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int tx_queue_setup(uint16_t qid, uint16_t nb_desc,
                             unsigned int socket_id,
                             const struct rte_eth_txconf *tx_conf) = 0;
  virtual int rx_queue_setup(uint16_t qid, uint16_t nb_desc,
                             unsigned int socket_id,
                             const struct rte_eth_rxconf *rx_conf,
                             rte_mempool *mp, irq_handler_cb_t handler = {}) = 0;
  tx_burst_t tx_burst;
  rx_burst_t rx_burst;
  virtual int drv_configure() = 0;
  virtual void get_stats(rte_eth_stats *stats) = 0;
  int dev_configure(uint16_t nb_rx, uint16_t nb_tx, rte_eth_conf *conf) {
    data.nb_rx_queues = nb_rx;
    data.nb_tx_queues = nb_tx;
    data.dev_conf.rxmode = conf->rxmode;
    data.dev_conf.txmode = conf->txmode;
    data.dev_conf.rx_adv_conf = conf->rx_adv_conf;
    data.dev_conf.intr_conf = conf->intr_conf;
    data.tx_queues.resize(nb_tx, nullptr);
    data.rx_queues.resize(nb_rx, nullptr);
    data.tx_queue_state.resize(nb_tx, RTE_ETH_QUEUE_STATE_STOPPED);
    data.rx_queue_state.resize(nb_rx, RTE_ETH_QUEUE_STATE_STOPPED);
    return drv_configure();
  }
  virtual int get_dev_info(rte_eth_dev_info *info) = 0;
  virtual int rss_reta_update(rte_eth_rss_reta_entry64 *reta,
                              uint16_t reta_size) = 0;
  virtual int rss_reta_query(rte_eth_rss_reta_entry64 *reta,
                             uint16_t reta_size) = 0;
  // Fills rss_conf->rss_hf always, and rss_conf->rss_key when it is
  // non-null (caller supplies a buffer of hash_key_size bytes).
  virtual int rss_hash_conf_get(rte_eth_rss_conf *rss_conf) = 0;
};

__inline uint16_t rte_eth_tx_burst(uint16_t port, uint16_t qid,
                                       rte_mbuf **pkts, uint16_t cnt) {
  auto *eth_dev = eth_os::get_eth_for_port(port);
  assert(eth_dev);
  return eth_dev->tx_burst(eth_dev, qid, pkts, cnt);
}

__inline uint16_t rte_eth_rx_burst(uint16_t port, uint16_t qid,
                                       rte_mbuf **pkts, uint16_t cnt) {
  auto *eth_dev = eth_os::get_eth_for_port(port);
  assert(eth_dev);
  return eth_dev->rx_burst(eth_dev, qid, pkts, cnt);
}

__inline int rte_eth_dev_is_valid_port(uint16_t port) {
  if (port < eth_os::instance.ifs.size())
    return 1;
  return 0;
}

__inline int rte_eth_dev_info_get(uint16_t port, rte_eth_dev_info *dev_info) {
  auto *dev = eth_os::get_eth_for_port(port);
  assert(dev);
  return dev->get_dev_info(dev_info);
}

__inline int rte_eth_dev_configure(uint16_t port, uint16_t nrx, uint16_t ntx,
                                   rte_eth_conf *port_conf) {
  return eth_os::get_eth_for_port(port)->dev_configure(ntx, nrx, port_conf);
}

__inline int rte_eth_dev_adjust_nb_rx_tx_desc(uint16_t port, uint16_t *nb_rxd, uint16_t* nb_txd){
    // Clamp to what the device advertises before rounding to a power of 2.
    // c5.large's ENA takes 1024; c7i.large's caps at 512 and rx_queue_setup
    // would reject the app's over-large request without this.
    auto *dev = eth_os::get_eth_for_port(port);
    if (dev) {
        rte_eth_dev_info info;
        std::memset(&info, 0, sizeof(info));
        dev->get_dev_info(&info);
        if (info.rx_desc_lim.nb_max && *nb_rxd > info.rx_desc_lim.nb_max) {
            *nb_rxd = info.rx_desc_lim.nb_max;
        }
        if (info.tx_desc_lim.nb_max && *nb_txd > info.tx_desc_lim.nb_max) {
            *nb_txd = info.tx_desc_lim.nb_max;
        }
    }
    *nb_rxd = rte_align32prevpow2(*nb_rxd);
    *nb_txd = rte_align32prevpow2(*nb_txd);
    return 0;
}

__inline int rte_eth_rx_queue_setup(uint16_t port, uint16_t qid,
                                    uint16_t nb_rxd, uint16_t socket_id,
                                    rte_eth_rxconf *rx_conf, rte_mempool *mp, irq_handler_cb_t handler = {}) {
  return eth_os::get_eth_for_port(port)->rx_queue_setup(qid, nb_rxd, socket_id,
                                                        rx_conf, mp, handler);
}

__inline int rte_eth_tx_queue_setup(uint16_t port, uint16_t qid,
                                    uint16_t nb_txd, uint16_t socket_id,
                                    rte_eth_txconf *tx_conf) {
  return eth_os::get_eth_for_port(port)->tx_queue_setup(qid, nb_txd, socket_id,
                                                        tx_conf);
}

__inline int rte_eth_dev_start(uint16_t port){
    return eth_os::get_eth_for_port(port)->start();
}

__inline int rte_eth_dev_stop(uint16_t port){
    return eth_os::get_eth_for_port(port)->stop();
}

__inline void rte_eth_macaddr_get(uint16_t port, rte_ether_addr* addr){
    auto *dev = eth_os::get_eth_for_port(port);
    *addr = dev->data.mac_addr;
}

__inline int rte_eth_dev_rss_reta_update(uint16_t port, rte_eth_rss_reta_entry64* reta, uint16_t reta_size){
    return eth_os::get_eth_for_port(port)->rss_reta_update(reta, reta_size);
}

__inline int rte_eth_dev_rss_reta_query(uint16_t port, rte_eth_rss_reta_entry64* reta, uint16_t reta_size){
    return eth_os::get_eth_for_port(port)->rss_reta_query(reta, reta_size);
}

__inline int rte_eth_dev_rss_hash_conf_get(uint16_t port, rte_eth_rss_conf* rss_conf){
    return eth_os::get_eth_for_port(port)->rss_hash_conf_get(rss_conf);
}

struct rte_eth_dev_tx_buffer {
  uint16_t size;
  uint16_t length;
  rte_mbuf *pkts[];

  rte_eth_dev_tx_buffer(uint16_t size) : size(size), length() {}

  static constexpr size_t memsize(uint16_t cnt) {
    return sizeof(rte_eth_dev_tx_buffer) + cnt * sizeof(rte_mbuf *);
  }
};

static inline void unsent_cb(rte_mbuf **pkts, uint16_t unsent, uint16_t port,
                             uint16_t qid) {
    rte_pktmbuf_free_bulk(pkts, unsent);
}

inline void rte_eth_tx_buffer_init(rte_eth_dev_tx_buffer *tx_buffer,
                                   uint16_t size) {
  new (tx_buffer) rte_eth_dev_tx_buffer(size);
}

inline void rte_eth_tx_buffer_flush(uint16_t port, uint16_t qid,
                                    rte_eth_dev_tx_buffer *tx_buffer) {
  auto to_send = tx_buffer->length;
  if (to_send == 0)
    return;
  auto sent = rte_eth_tx_burst(port, qid, tx_buffer->pkts, to_send);
  tx_buffer->length = 0;
  if (sent < to_send)
    unsent_cb(tx_buffer->pkts + sent, to_send - sent, port, qid);
}

inline void rte_eth_tx_buffer(uint16_t port, uint16_t qid,
                              rte_eth_dev_tx_buffer *tx_buffer, rte_mbuf *pkt) {
  tx_buffer->pkts[tx_buffer->length++] = pkt;
  if (tx_buffer->length < tx_buffer->size)
    return;
  rte_eth_tx_buffer_flush(port, qid, tx_buffer);
}

#endif
