// extern "C" bridge to minidpdk; only integers and opaque pointers cross into Rust.
#pragma once
#include <cstdint>

extern "C" {

int shim_get_dev_info(uint16_t port_id, uint16_t *max_rx_queues,
                       uint16_t *max_tx_queues);

void *shim_pktmbuf_pool_create(const char *name, uint32_t n,
                                uint32_t cache_size, uint16_t priv_size,
                                uint16_t data_room_size);

int shim_eth_dev_configure(uint16_t port_id, uint16_t nb_rx_q,
                            uint16_t nb_tx_q);

void shim_adjust_nb_rx_tx_desc(uint16_t port_id, uint16_t *nb_rx_desc,
                                uint16_t *nb_tx_desc);

int shim_rx_queue_setup(uint16_t port_id, uint16_t queue_id,
                         uint16_t nb_desc, void *mempool);
int shim_tx_queue_setup(uint16_t port_id, uint16_t queue_id,
                         uint16_t nb_desc);

int shim_dev_start(uint16_t port_id);

void shim_macaddr_get(uint16_t port_id, uint8_t *addr_bytes);

// Zero-copy TX: a writable data area; the handle goes to shim_mbuf_tx_burst or shim_mbuf_free.
uint8_t *shim_mbuf_alloc_tx(void *pool, uint16_t queue_id, void **out_handle,
                             uint16_t *out_cap);

uint16_t shim_mbuf_tx_burst(uint16_t port_id, uint16_t queue_id, void **handles,
                            const uint16_t *lens, uint16_t n);
void shim_mbuf_free(void *handle);

// [ipackets, opackets, ibytes, obytes, imissed, ierrors, oerrors, rx_nombuf]
int shim_eth_stats(uint16_t port_id, uint64_t *out, uint16_t n);

int shim_rss_hash_key(uint16_t port_id, uint8_t *out_key, uint16_t out_len);
int shim_rss_reta_size(uint16_t port_id);
int shim_rss_reta(uint16_t port_id, uint16_t *out, uint16_t out_entries);

uint16_t shim_mbuf_rx_burst_n(uint16_t port_id, uint16_t queue_id,
                               void **out_handles, const uint8_t **out_data,
                               uint16_t *out_lens, uint16_t max);

void *shim_thread_spawn(void (*fn)(void *), void *arg, int cpu_id);
void shim_thread_join(void *handle);

// Park until *flag is nonzero; the waker stores the flag, then unparks the handle.
void *shim_thread_current(void);
void shim_thread_park(const unsigned int *flag);
void shim_thread_unpark(void *handle);

uint64_t shim_time_seconds(void);
uint64_t shim_time_ns(void);

uint64_t shim_cpu_count(void);

void *shim_malloc(uint64_t size);
void  shim_free(void *ptr);
void *shim_realloc(void *ptr, uint64_t size);
void *shim_aligned_alloc(uint64_t align, uint64_t size);

}  // extern "C"
