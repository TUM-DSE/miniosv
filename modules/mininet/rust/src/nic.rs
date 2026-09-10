//! Bringing port 0 up, and reading its counters back.

use alloc::vec::Vec;
use core::fmt::Write;

use crate::device::RX_BURST;
use crate::error::Error;
use crate::ffi::{
    rte_pktmbuf_pool, shim_adjust_nb_rx_tx_desc, shim_dev_start, shim_dev_stop,
    shim_eth_dev_configure, shim_eth_qstats, shim_eth_stats, shim_get_dev_info, shim_macaddr_get,
    shim_mempool_free, shim_pktmbuf_pool_create, shim_rx_queue_setup, shim_tx_queue_setup, PORT,
};
use crate::print::BufWriter;

/// Owns one queue's mempool. Freed when the [`crate::Stack`] holding it drops,
/// which is after every worker using it has gone.
pub(crate) struct PktPool(*mut rte_pktmbuf_pool);

impl PktPool {
    pub(crate) fn as_ptr(&self) -> *mut rte_pktmbuf_pool {
        self.0
    }
}

impl Drop for PktPool {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { shim_mempool_free(self.0) };
        }
    }
}

/// ENA VFs cap io-queue count per instance size, and `rx_queue_setup` for a
/// queue id past the maximum is a hard reject -- so the clamp has to happen
/// before anything is configured. A device that reports nothing is taken at
/// its word rather than second-guessed.
pub(crate) fn clamp_queues(requested: u16) -> u16 {
    let mut max_rx: u16 = 0;
    let mut max_tx: u16 = 0;
    unsafe { shim_get_dev_info(PORT, &mut max_rx, &mut max_tx) };
    let dev_max = core::cmp::min(max_rx, max_tx);
    if dev_max == 0 {
        requested
    } else {
        core::cmp::min(requested, dev_max)
    }
}

/// Configure and start port 0 with `n_queues` RX+TX queues, RSS-hashing the
/// TCP/IPv4 4-tuple when there is more than one.
///
/// Per-queue mempools eliminate cross-worker contention on the pool spinlock,
/// which choked throughput badly at 8 queues and made it unstable at 4 under
/// the shared-pool design.
pub(crate) fn probe_and_open(n_queues: u16) -> Result<(Vec<PktPool>, [u8; 6]), Error> {
    const DATA_ROOM_SIZE: u16 = 1536;
    // 1024 descriptors could not absorb the inbound burst while a worker
    // walked 48 connection state machines between polls: the NIC dropped 3.3%
    // of all inbound frames for want of a free descriptor (imissed), which is
    // invisible above the driver and looked like unanswered SYNs.
    const DESC_NUM: u16 = 4096;
    const CACHE: u32 = 64;
    // Size for every simultaneous holder rather than the RX ring plus a
    // guess: the RX ring pins DESC_NUM-1, the TX ring holds up to DESC_NUM
    // more awaiting reclaim, the per-core cache holds CACHE, and the RX
    // prefetch holds RX_BURST. At DESC_NUM*2 the pool fell ~100 mbufs short
    // of that worst case, and exhaustion silently discarded frames -- a
    // dropped SYN is then indistinguishable from an unanswered one.
    let per_queue_size: u32 = (DESC_NUM as u32) * 2 + CACHE + RX_BURST as u32 + 512;

    let mut pools: Vec<PktPool> = Vec::with_capacity(n_queues as usize);
    for q in 0..n_queues {
        let mut name = [0u8; 32];
        let _ = write!(&mut BufWriter::new(&mut name), "mininet-q{}\0", q);
        let raw = unsafe {
            shim_pktmbuf_pool_create(name.as_ptr(), per_queue_size, CACHE, 0, DATA_ROOM_SIZE)
        };
        if raw.is_null() {
            return Err(Error::NoMemory);
        }
        pools.push(PktPool(raw));
    }

    if unsafe { shim_eth_dev_configure(PORT, n_queues, n_queues) } != 0 {
        return Err(Error::NoDevice);
    }
    let (mut rx_desc, mut tx_desc) = (DESC_NUM, DESC_NUM);
    unsafe { shim_adjust_nb_rx_tx_desc(PORT, &mut rx_desc, &mut tx_desc) };
    // The device clamps to what it supports, so report what was actually
    // granted rather than what was asked for.
    if rx_desc != DESC_NUM || tx_desc != DESC_NUM {
        println!(
            "descriptors: asked {}, got rx {} tx {} (device clamp)",
            DESC_NUM, rx_desc, tx_desc
        );
    } else {
        println!("descriptors: rx {} tx {} per queue", rx_desc, tx_desc);
    }
    for q in 0..n_queues {
        if unsafe { shim_rx_queue_setup(PORT, q, rx_desc, pools[q as usize].0) } != 0 {
            return Err(Error::NoDevice);
        }
        if unsafe { shim_tx_queue_setup(PORT, q, tx_desc) } != 0 {
            return Err(Error::NoDevice);
        }
    }
    if unsafe { shim_dev_start(PORT) } != 0 {
        return Err(Error::NoDevice);
    }
    let mut mac = [0u8; 6];
    unsafe { shim_macaddr_get(PORT, mac.as_mut_ptr()) };
    println!(
        "port 0: MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} ({} queues)",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], n_queues
    );
    Ok((pools, mac))
}

pub(crate) fn dev_stop() {
    unsafe { shim_dev_stop(PORT) };
}

/// Device counters. `imissed` is the one that matters for an unanswered SYN: a
/// SYN-ACK the device dropped for want of a descriptor never reaches any queue,
/// so from inside the stack it is indistinguishable from a peer that never
/// replied. Without this the cause cannot be attributed at all.
#[derive(Debug, Clone, Copy, Default)]
pub struct NicStats {
    pub ipackets: u64,
    pub opackets: u64,
    pub ibytes: u64,
    pub obytes: u64,
    pub imissed: u64,
    pub ierrors: u64,
    pub oerrors: u64,
    pub rx_nombuf: u64,
}

pub fn eth_stats() -> Option<NicStats> {
    let mut st = [0u64; 8];
    if unsafe { shim_eth_stats(PORT, st.as_mut_ptr(), st.len() as u16) } != 0 {
        return None;
    }
    Some(NicStats {
        ipackets: st[0],
        opackets: st[1],
        ibytes: st[2],
        obytes: st[3],
        imissed: st[4],
        ierrors: st[5],
        oerrors: st[6],
        rx_nombuf: st[7],
    })
}

/// Per-queue inbound packet and error counts, truncated to `out.len()` queues.
/// Returns how many the device reported.
pub fn eth_qstats(ipkts: &mut [u64], errs: &mut [u64]) -> usize {
    let n = core::cmp::min(ipkts.len(), errs.len());
    let rc = unsafe { shim_eth_qstats(PORT, ipkts.as_mut_ptr(), errs.as_mut_ptr(), n as u16) };
    if rc <= 0 {
        0
    } else {
        core::cmp::min(rc as usize, n)
    }
}
