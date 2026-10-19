/* SPDX-License-Identifier: BSD-3-Clause */

// The iavf port as a minidpdk rte_eth_dev: DPDK's eth_dev_ops become the virtual methods, its interrupt thread a
// management thread on MSI-X vector 0, and its Rx interrupt mode a thread per queue that hands bursts to a callback.

#ifndef IAVF_ETHDEV_HH
#define IAVF_ETHDEV_HH

#include <atomic>
#include <vector>

#include <drivers/pci-device.hh>
#include <osv/msi.hh>
#include <osv/sched.hh>

#include "iavf.h"
#include "iavf_rxtx.h"

class iavf_eth_dev : public rte_eth_dev {
public:
	explicit iavf_eth_dev(iavf_adapter *adapter) : rte_eth_dev(adapter), adapter(adapter) {}
	~iavf_eth_dev() override = default;

	int mtu_set(uint16_t mtu) override;
	int start() override;
	int stop() override;
	int tx_queue_setup(uint16_t qid, uint16_t nb_desc, unsigned int socket_id,
			   const struct rte_eth_txconf *tx_conf) override;
	int rx_queue_setup(uint16_t qid, uint16_t nb_desc, unsigned int socket_id,
			   const struct rte_eth_rxconf *rx_conf, rte_mempool *mp,
			   irq_handler_cb_t handler = {}) override;
	int drv_configure() override;
	void get_stats(rte_eth_stats *stats) override;
	int get_dev_info(rte_eth_dev_info *info) override;
	int rss_reta_update(rte_eth_rss_reta_entry64 *reta, uint16_t reta_size) override;
	int rss_reta_query(rte_eth_rss_reta_entry64 *reta, uint16_t reta_size) override;
	int rss_hash_conf_get(rte_eth_rss_conf *rss_conf) override;
	int rx_intr_enable(uint16_t qid) override;
	int rx_intr_disable(uint16_t qid) override;

	iavf_adapter *adapter;
	pci::device *pdev = nullptr;
	pci::bar *regs = nullptr;

	// The bursts iavf_set_rx_function / iavf_set_tx_function picked.
	iavf_burst_t rx_fn = nullptr;
	iavf_burst_t tx_fn = nullptr;

	// Vector 0: mailbox replies and PF events, read on a thread since the base code sleeps.
	sched::thread *mgmt_thread = nullptr;
	std::atomic<bool> mgmt_pending{false};
	std::atomic<bool> mgmt_stop{false};
	std::vector<msix_vector *> vectors;  // [0] is vector 0, then one per Rx queue in interrupt mode
};

// Interrupt-mode receive: the queue's vector wakes the thread, which drains the ring into the callback.
struct iavf_rxq_intr {
	irq_handler_cb_t handler;
	sched::thread *thread = nullptr;
	std::atomic<bool> ready{false};
	std::atomic<bool> stop{false};
	uint16_t vector = 0;  // VF vector the queue is mapped to
	uint64_t interrupts = 0;
};

static inline iavf_eth_dev *iavf_edev(struct rte_eth_dev *dev)
{
	return static_cast<iavf_eth_dev *>(dev);
}

// Whether the caller is the vector-0 thread, which DPDK calls the interrupt thread.
bool iavf_in_mgmt_thread(struct rte_eth_dev *dev);

bool iavf_probe(pci::device *dev);
int iavf_attach(pci::device *dev, iavf_eth_dev **edev);

#endif /* IAVF_ETHDEV_HH */
