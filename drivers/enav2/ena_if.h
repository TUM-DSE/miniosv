#ifndef ENA_IF_H_
#define ENA_IF_H_

#include <minidpdk/dev.hh>
#include <minidpdk/mem.hh>
#include <minidpdk/rss.hh>
#include <cstdint>

class ena_eth_dev : public rte_eth_dev{
public:
    ena_eth_dev(void* data): rte_eth_dev(data) {}
    ~ena_eth_dev() override = default;
    int mtu_set(uint16_t mtu) override;
    int start() override;
    int stop() override;
    int tx_queue_setup(uint16_t qid,
			      uint16_t nb_desc, unsigned int socket_id,
			      const struct rte_eth_txconf *tx_conf) override;
    int rx_queue_setup(uint16_t qid,
			      uint16_t nb_desc, unsigned int socket_id,
			      const struct rte_eth_rxconf *rx_conf,
            rte_mempool *mp, irq_handler_cb_t handler = {}) override;
    void get_stats(rte_eth_stats *stats) override;
    int drv_configure() override;
    int get_dev_info(rte_eth_dev_info *info) override; 
    int rss_reta_update(rte_eth_rss_reta_entry64* reta, uint16_t reta_size) override;
    int rss_reta_query(rte_eth_rss_reta_entry64* reta, uint16_t reta_size) override;
    int rss_hash_conf_get(rte_eth_rss_conf* rss_conf) override;
    int rx_intr_enable(uint16_t qid) override;
    int rx_intr_disable(uint16_t qid) override;
};
#endif // !ENA_IF_H_
