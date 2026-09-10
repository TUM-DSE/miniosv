//! The C ABI `modules/mininet/shim/shim.cc` exports.
//!
//! Every minidpdk struct is built and read inside the shim; only integers and
//! opaque pointers cross here. Keep this in sync with `shim/shim.hh` -- there
//! is no header generation, and a signature that drifts is a silent
//! miscompile rather than a link error, since the shim is C.

use core::ffi::{c_int, c_void};

/// Opaque mempool handle. One per queue: a shared pool serialises workers on
/// its spinlock, which cost most of the throughput at 8 queues.
#[repr(C)]
pub struct rte_pktmbuf_pool {
    _private: [u8; 0],
}

extern "C" {
    pub fn shim_malloc(size: u64) -> *mut u8;
    pub fn shim_free(ptr: *mut u8);
    pub fn shim_realloc(ptr: *mut u8, size: u64) -> *mut u8;
    /// Wall clock, for TLS certificate validity.
    pub fn shim_time_seconds() -> u64;
    /// Monotonic, for smoltcp's timers.
    pub fn shim_time_ns() -> u64;
    pub fn write(fd: c_int, buf: *const u8, count: usize) -> isize;

    pub fn shim_get_dev_info(port_id: u16, max_rx_queues: *mut u16, max_tx_queues: *mut u16) -> c_int;
    pub fn shim_pktmbuf_pool_create(
        name: *const u8,
        n: u32,
        cache_size: u32,
        priv_size: u16,
        data_room_size: u16,
    ) -> *mut rte_pktmbuf_pool;
    pub fn shim_mempool_free(pool: *mut rte_pktmbuf_pool);
    pub fn shim_eth_dev_configure(port_id: u16, nb_rx_q: u16, nb_tx_q: u16) -> c_int;
    pub fn shim_adjust_nb_rx_tx_desc(port_id: u16, nb_rx_desc: *mut u16, nb_tx_desc: *mut u16);
    pub fn shim_rx_queue_setup(
        port_id: u16,
        queue_id: u16,
        nb_desc: u16,
        mempool: *mut rte_pktmbuf_pool,
    ) -> c_int;
    pub fn shim_tx_queue_setup(port_id: u16, queue_id: u16, nb_desc: u16) -> c_int;
    pub fn shim_dev_start(port_id: u16) -> c_int;
    pub fn shim_dev_stop(port_id: u16);
    pub fn shim_macaddr_get(port_id: u16, addr_bytes: *mut u8);

    pub fn shim_mbuf_alloc_tx(
        pool: *mut rte_pktmbuf_pool,
        queue_id: u16,
        out_handle: *mut *mut c_void,
        out_cap: *mut u16,
    ) -> *mut u8;
    pub fn shim_mbuf_tx(port_id: u16, queue_id: u16, handle: *mut c_void, len: u16) -> c_int;
    pub fn shim_mbuf_free(handle: *mut c_void);
    pub fn shim_mbuf_rx_burst_n(
        port_id: u16,
        queue_id: u16,
        out_handles: *mut *mut c_void,
        out_data: *mut *const u8,
        out_lens: *mut u16,
        max: u16,
    ) -> u16;

    pub fn shim_rss_hash_key(port_id: u16, out_key: *mut u8, out_len: u16) -> c_int;
    pub fn shim_rss_reta(port_id: u16, out: *mut u16, out_entries: u16) -> c_int;
    pub fn shim_eth_stats(port_id: u16, out: *mut u64, n: u16) -> c_int;
    pub fn shim_eth_qstats(port_id: u16, ipkts: *mut u64, errs: *mut u64, nq: u16) -> c_int;

    pub fn shim_thread_spawn(f: extern "C" fn(*mut c_void), arg: *mut c_void, cpu_id: c_int) -> *mut c_void;
    pub fn shim_thread_join(handle: *mut c_void);
    pub fn shim_thread_current() -> *mut c_void;
    /// Blocks until `*flag` is nonzero. See shim.hh for why this is not a spin.
    pub fn shim_thread_park(flag: *const u32);
    pub fn shim_thread_unpark(handle: *mut c_void);
}

/// AWS gives a guest exactly one ENA interface, so the port is never in doubt.
pub const PORT: u16 = 0;
