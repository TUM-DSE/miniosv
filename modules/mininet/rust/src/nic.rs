//! Bringing port 0 up, and reading its counters back. Nothing here is undone:
//! the port and its pools live as long as the image.

use alloc::vec::Vec;
use core::fmt::Write;

use crate::device::RX_BURST;
use crate::error::Error;
use crate::ffi::{
    rte_pktmbuf_pool, shim_adjust_nb_rx_tx_desc, shim_dev_start, shim_eth_dev_configure,
    shim_eth_stats, shim_get_dev_info, shim_macaddr_get, shim_pktmbuf_pool_create,
    shim_rx_queue_setup, shim_tx_queue_setup, PORT,
};
use crate::print::BufWriter;

/// One queue's mempool.
pub(crate) struct PktPool(*mut rte_pktmbuf_pool);

impl PktPool {
    pub(crate) fn as_ptr(&self) -> *mut rte_pktmbuf_pool {
        self.0
    }
}

/// ENA caps io-queue count per instance size and rejects a queue past it.
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

/// Configure and start port 0 with `n_queues` RX+TX queues and per-queue mempools.
pub(crate) fn probe_and_open(n_queues: u16, rx_desc: u16) -> Result<(Vec<PktPool>, [u8; 6]), Error> {
    const DATA_ROOM_SIZE: u16 = 1536;
    // 1024 descriptors dropped 3.3% of inbound frames (imissed) between polls.
    const DESC_NUM: u16 = 4096;
    const CACHE: u32 = 64;
    let rx_want = if rx_desc == 0 { DESC_NUM } else { rx_desc };
    // Every simultaneous holder: RX ring, TX ring awaiting reclaim, cache, RX burst.
    let per_queue_size: u32 = (rx_want as u32) + (DESC_NUM as u32) + CACHE + RX_BURST as u32 + 512;

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
    let (mut rx_desc, mut tx_desc) = (rx_want, DESC_NUM);
    unsafe { shim_adjust_nb_rx_tx_desc(PORT, &mut rx_desc, &mut tx_desc) };
    // What was granted, not what was asked.
    if rx_desc != rx_want || tx_desc != DESC_NUM {
        println!(
            "descriptors: asked rx {} tx {}, got rx {} tx {} (device clamp)",
            rx_want, DESC_NUM, rx_desc, tx_desc
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

/// Device counters. A SYN-ACK dropped for want of a descriptor (imissed) is invisible above.
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
