//! A smoltcp `Device` over one minidpdk queue. Nothing is copied on either path.

use alloc::vec::Vec;
use core::ffi::c_void;
use core::ptr;

use smoltcp::phy::{ChecksumCapabilities, Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::time::Instant;

use crate::ffi::{rte_pktmbuf_pool, shim_mbuf_alloc_tx, shim_mbuf_free, shim_mbuf_rx_burst_n, shim_mbuf_tx_burst, PORT};
use crate::rss::OwnedPorts;
use crate::stats;

pub(crate) const MTU: usize = 1514;

/// RX drains up to this many mbufs per burst.
pub(crate) const RX_BURST: usize = 32;
const TX_BATCH: usize = 32;
/// Flush attempts before a frame is dropped because the ring never took the
/// batch: each is a doorbell plus a completion sweep, so this is milliseconds.
const TX_FULL_SPINS: u32 = 10_000;

pub(crate) struct DpdkDevice {
    pub(crate) queue_id: u16,
    pub(crate) pool: *mut rte_pktmbuf_pool,
    /// One fabricated frame, seeding the neighbour cache with the gateway's MAC.
    pub(crate) pending_synth: Option<Vec<u8>>,
    /// Ports this queue owns; `None` accepts everything (the DHCP/ARP phase on queue 0).
    /// A TCP frame for a port not owned is a prediction failure and is counted.
    pub(crate) owned_ports: Option<OwnedPorts>,
    rx_pref_handles: [*mut c_void; RX_BURST],
    rx_pref_data: [*const u8; RX_BURST],
    rx_pref_lens: [u16; RX_BURST],
    rx_pref_pos: u16,
    rx_pref_len: u16,
    rx_pkts: u64,
    tx_pkts: u64,
    /// Frames written but not yet handed to the NIC: one doorbell per flush, not per frame.
    tx_handles: [*mut c_void; TX_BATCH],
    tx_lens: [u16; TX_BATCH],
    tx_len: usize,
}

impl DpdkDevice {
    pub(crate) fn new(
        queue_id: u16,
        pool: *mut rte_pktmbuf_pool,
        owned_ports: Option<OwnedPorts>,
        pending_synth: Option<Vec<u8>>,
    ) -> Self {
        Self {
            queue_id,
            pool,
            pending_synth,
            owned_ports,
            rx_pref_handles: [ptr::null_mut(); RX_BURST],
            rx_pref_data: [ptr::null(); RX_BURST],
            rx_pref_lens: [0; RX_BURST],
            rx_pref_pos: 0,
            rx_pref_len: 0,
            rx_pkts: 0,
            tx_pkts: 0,
            tx_handles: [ptr::null_mut(); TX_BATCH],
            tx_lens: [0; TX_BATCH],
            tx_len: 0,
        }
    }

    /// Hand the batched frames to the NIC. What the ring will not take stays
    /// at the front of the batch for the next flush: a poll that ACKs a few
    /// thousand segments at once outruns a 1024-entry ring, and dropping the
    /// rest cost the sender its ACKs.
    pub fn flush_tx(&mut self) {
        if self.tx_len == 0 {
            return;
        }
        let sent = unsafe {
            shim_mbuf_tx_burst(PORT, self.queue_id, self.tx_handles.as_mut_ptr(), self.tx_lens.as_ptr(), self.tx_len as u16)
        } as usize;
        self.tx_pkts += sent as u64;
        let held = self.tx_len - sent;
        if held > 0 {
            stats::tx_held(held as u64);
            self.tx_handles.copy_within(sent..self.tx_len, 0);
            self.tx_lens.copy_within(sent..self.tx_len, 0);
        }
        self.tx_len = held;
    }

    /// Packets received and transmitted since the last call.
    pub fn take_counts(&mut self) -> (u64, u64) {
        let r = (self.rx_pkts, self.tx_pkts);
        self.rx_pkts = 0;
        self.tx_pkts = 0;
        r
    }

    /// True if the frame should be passed up.
    pub(crate) fn accepts(&self, bytes: &[u8]) -> bool {
        let owned = match &self.owned_ports {
            Some(o) => o,
            None => return true,
        };
        // Ethernet(14) + IPv4(20) + TCP(20) = 54.
        if bytes.len() < 54 {
            return true;
        }
        if bytes[12] != 0x08 || bytes[13] != 0x00 {
            return true; // not IPv4
        }
        let ihl = (bytes[14] & 0x0f) as usize * 4;
        let l4 = 14 + ihl;
        if bytes[23] != 6 /* TCP */ || l4 + 4 > bytes.len() {
            return true;
        }
        let dst_port = ((bytes[l4 + 2] as u16) << 8) | (bytes[l4 + 3] as u16);
        if owned.contains(dst_port) {
            true
        } else {
            stats::misrouted_drop();
            false
        }
    }
}

pub(crate) enum DpdkRxToken {
    Mbuf {
        handle: *mut c_void,
        data: *const u8,
        len: usize,
    },
    Synth(Vec<u8>),
}

pub(crate) struct DpdkTxToken<'a> {
    dev: &'a mut DpdkDevice,
}

impl RxToken for DpdkRxToken {
    fn consume<R, F: FnOnce(&[u8]) -> R>(self, f: F) -> R {
        let mut this = core::mem::ManuallyDrop::new(self);
        match &mut *this {
            DpdkRxToken::Mbuf { handle, data, len } => {
                let slice = unsafe { core::slice::from_raw_parts(*data, *len) };
                let r = f(slice);
                unsafe { shim_mbuf_free(*handle) };
                r
            }
            DpdkRxToken::Synth(buf) => f(&buf[..]),
        }
    }
}

impl Drop for DpdkDevice {
    fn drop(&mut self) {
        self.flush_tx();
    }
}

impl Drop for DpdkRxToken {
    fn drop(&mut self) {
        if let DpdkRxToken::Mbuf { handle, .. } = *self {
            unsafe { shim_mbuf_free(handle) };
        }
    }
}

impl TxToken for DpdkTxToken<'_> {
    fn consume<R, F: FnOnce(&mut [u8]) -> R>(self, len: usize, f: F) -> R {
        // The token that comes with a received frame cannot be refused, so a
        // full batch has to be pushed into the ring here. The ring drains by
        // DMA at its own pace; wait on it briefly rather than lose the frame.
        let mut spins = 0u32;
        while self.dev.tx_len == TX_BATCH && spins < TX_FULL_SPINS {
            self.dev.flush_tx();
            spins += 1;
            core::hint::spin_loop();
        }
        if self.dev.tx_len == TX_BATCH {
            // Still full: the NIC is not taking anything. Count it as a drop
            // with the no-mbuf ones; smoltcp still wants `f` called.
            stats::tx_alloc_fail();
            let mut scratch = [0u8; MTU];
            let n = core::cmp::min(len, scratch.len());
            return f(&mut scratch[..n]);
        }
        let mut handle: *mut c_void = ptr::null_mut();
        let mut cap: u16 = 0;
        let data = unsafe { shim_mbuf_alloc_tx(self.dev.pool, self.dev.queue_id, &mut handle, &mut cap) };
        if data.is_null() || handle.is_null() {
            // Pool exhausted: smoltcp still wants `f` called; count the discarded frame.
            stats::tx_alloc_fail();
            let mut scratch = [0u8; MTU];
            let n = core::cmp::min(len, scratch.len());
            return f(&mut scratch[..n]);
        }
        let n = core::cmp::min(len, cap as usize);
        let slice = unsafe { core::slice::from_raw_parts_mut(data, n) };
        let r = f(slice);
        self.dev.tx_handles[self.dev.tx_len] = handle;
        self.dev.tx_lens[self.dev.tx_len] = n as u16;
        self.dev.tx_len += 1;
        if self.dev.tx_len == TX_BATCH {
            self.dev.flush_tx();
        }
        r
    }
}

impl Device for DpdkDevice {
    type RxToken<'a>
        = DpdkRxToken
    where
        Self: 'a;
    type TxToken<'a>
        = DpdkTxToken<'a>
    where
        Self: 'a;

    fn receive(&mut self, _t: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        if let Some(buf) = self.pending_synth.take() {
            return Some((DpdkRxToken::Synth(buf), DpdkTxToken { dev: self }));
        }
        loop {
            if self.rx_pref_pos == self.rx_pref_len {
                let got = unsafe {
                    shim_mbuf_rx_burst_n(
                        PORT,
                        self.queue_id,
                        self.rx_pref_handles.as_mut_ptr(),
                        self.rx_pref_data.as_mut_ptr(),
                        self.rx_pref_lens.as_mut_ptr(),
                        RX_BURST as u16,
                    )
                };
                self.rx_pkts += got as u64;
                if got == 0 {
                    self.flush_tx();
                    return None;
                }
                self.rx_pref_pos = 0;
                self.rx_pref_len = got;
            }
            let i = self.rx_pref_pos as usize;
            self.rx_pref_pos += 1;
            let handle = self.rx_pref_handles[i];
            let data = self.rx_pref_data[i];
            let len = self.rx_pref_lens[i] as usize;

            let ok = self.accepts(unsafe { core::slice::from_raw_parts(data, len) });
            if ok {
                return Some((DpdkRxToken::Mbuf { handle, data, len }, DpdkTxToken { dev: self }));
            }
            unsafe { shim_mbuf_free(handle) };
        }
    }

    /// `None` while the batch is full and the ring will not drain it: smoltcp
    /// then keeps the frame for its next poll instead of losing it.
    fn transmit(&mut self, _t: Instant) -> Option<Self::TxToken<'_>> {
        if self.tx_len == TX_BATCH {
            self.flush_tx();
            if self.tx_len == TX_BATCH {
                return None;
            }
        }
        Some(DpdkTxToken { dev: self })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.max_transmission_unit = MTU;
        c.medium = Medium::Ethernet;
        // The NIC handles L3/L4 checksums; don't pay the CPU cost twice.
        c.checksum = ChecksumCapabilities::ignored();
        c
    }
}
