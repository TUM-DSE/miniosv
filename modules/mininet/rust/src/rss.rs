//! The RSS steering model.
//!
//! The RX queue a flow lands on is a pure function of its 4-tuple:
//!
//! ```text
//! queue = reta[ toeplitz(key, src_ip || dst_ip || sport || dport) & (len - 1) ]
//! ```
//!
//! The key and table are read from the device at startup rather than assumed.
//! This exact form was validated against 200k live packets (100% agreement,
//! against 48-50% for every other key orientation and tuple order tried), and
//! the Toeplitz implementation against the canonical Microsoft RSS vectors.
//!
//! Because we choose our own source port, the function can be inverted: pick
//! ports whose return traffic provably lands on the queue we are polling. That
//! is what lets a worker own a queue outright, with no shared state and no
//! cross-queue delivery.

use alloc::vec;
use alloc::vec::Vec;

use crate::error::Error;
use crate::ffi::{shim_rss_hash_key, shim_rss_reta, PORT};

pub(crate) const EPH_BASE: u16 = 49152;
pub(crate) const EPH_LEN: usize = 16384;
const OWNED_BITMAP_BYTES: usize = EPH_LEN / 8;

const MAX_KEY: usize = 64;
const MAX_RETA: usize = 512;

/// The device's steering configuration, copied out once at startup.
///
/// A value rather than a global: it is read on every inbound packet by every
/// worker, and a `Copy` of ~1 KiB per worker buys freedom from shared mutable
/// state that the hot path would otherwise have to reason about.
#[derive(Clone, Copy)]
pub(crate) struct Rss {
    key: [u8; MAX_KEY],
    key_len: usize,
    reta: [u16; MAX_RETA],
    reta_len: usize,
    /// One queue receives everything, so there is nothing to predict. The PMD
    /// does not configure RSS for a single queue, so the key and table would
    /// be unreadable anyway.
    single_queue: bool,
}

impl Rss {
    pub(crate) fn load(n_queues: u16) -> Result<Rss, Error> {
        let mut rss = Rss {
            key: [0; MAX_KEY],
            key_len: 0,
            reta: [0; MAX_RETA],
            reta_len: 0,
            single_queue: false,
        };

        if n_queues == 1 {
            rss.single_queue = true;
            // Same shape as the line below: the harness reads worker count off it.
            println!("rss: {} queues, steering is trivial", n_queues);
            return Ok(rss);
        }

        let mut key = [0u8; MAX_KEY];
        let klen = unsafe { shim_rss_hash_key(PORT, key.as_mut_ptr(), key.len() as u16) };
        if klen <= 0 {
            println!("FAIL: RSS hash key unavailable (rc={})", klen);
            return Err(Error::RssUnavailable);
        }
        let klen = klen as usize;
        rss.key[..klen].copy_from_slice(&key[..klen]);
        rss.key_len = klen;

        let mut reta = [0u16; MAX_RETA];
        let n = unsafe { shim_rss_reta(PORT, reta.as_mut_ptr(), reta.len() as u16) };
        if n <= 0 {
            println!("FAIL: RSS indirection table unavailable (rc={})", n);
            return Err(Error::RssUnavailable);
        }
        let n = n as usize;
        if n & (n - 1) != 0 {
            println!("FAIL: RSS table size {} is not a power of two", n);
            return Err(Error::RssUnavailable);
        }
        rss.reta[..n].copy_from_slice(&reta[..n]);
        rss.reta_len = n;

        println!(
            "rss: {} queues, {}-entry table, {}-byte key",
            n_queues, n, klen
        );
        Ok(rss)
    }

    /// Predicted RX queue for a 12-byte tuple in wire order. `u16::MAX` if the
    /// configuration could not be read.
    fn predict(&self, tuple: &[u8; 12]) -> u16 {
        if self.single_queue {
            return 0;
        }
        if self.key_len == 0 || self.reta_len == 0 {
            return u16::MAX;
        }
        let h = toeplitz(&self.key[..self.key_len], tuple) as usize;
        self.reta[h & (self.reta_len - 1)]
    }

    /// Which RX queue an inbound packet for `local_port` would be steered to.
    /// The peer is the source and we are the destination: the direction the
    /// NIC hashes.
    fn queue_for_local_port(&self, peer_ip: [u8; 4], peer_port: u16, our_ip: [u8; 4], local_port: u16) -> u16 {
        let mut tuple = [0u8; 12];
        tuple[0..4].copy_from_slice(&peer_ip);
        tuple[4..8].copy_from_slice(&our_ip);
        tuple[8..10].copy_from_slice(&peer_port.to_be_bytes());
        tuple[10..12].copy_from_slice(&local_port.to_be_bytes());
        self.predict(&tuple)
    }

    /// Every ephemeral source port whose return traffic from `peer` lands on
    /// `queue_id`, as a bitmap over the ephemeral range.
    ///
    /// The set depends on the peer's address, so it is per-endpoint, not
    /// per-worker: two buckets in different regions do not share a partition.
    pub(crate) fn owned_ports(
        &self,
        peer_ip: [u8; 4],
        peer_port: u16,
        our_ip: [u8; 4],
        queue_id: u16,
    ) -> OwnedPorts {
        let mut bitmap = vec![0u8; OWNED_BITMAP_BYTES];
        let mut count = 0u32;
        for i in 0..EPH_LEN {
            let port = EPH_BASE + i as u16;
            if self.queue_for_local_port(peer_ip, peer_port, our_ip, port) == queue_id {
                bitmap[i / 8] |= 1 << (i % 8);
                count += 1;
            }
        }
        OwnedPorts { bitmap, count }
    }
}

/// The ephemeral ports one worker may use, as a bitmap the RX filter can test
/// in constant time.
pub(crate) struct OwnedPorts {
    bitmap: Vec<u8>,
    count: u32,
}

impl OwnedPorts {
    pub(crate) fn count(&self) -> u32 {
        self.count
    }

    #[inline]
    pub(crate) fn contains(&self, port: u16) -> bool {
        if port < EPH_BASE {
            return true; // non-ephemeral (DHCP etc.) is never ours to filter
        }
        let i = (port - EPH_BASE) as usize;
        match self.bitmap.get(i / 8) {
            Some(byte) => byte & (1 << (i % 8)) != 0,
            None => true,
        }
    }

    /// Up to `want` ports, spread across the range rather than taken
    /// consecutively. Adjacent ports are no more likely to collide, but
    /// spreading keeps the choice independent of any local structure in the
    /// hash.
    pub(crate) fn spread(&self, want: usize) -> Vec<u16> {
        let mut out = Vec::with_capacity(want);
        if want == 0 {
            return out;
        }
        let stride = core::cmp::max(1, (self.count as usize) / want);
        let mut seen = 0usize;
        for i in 0..EPH_LEN {
            if out.len() == want {
                break;
            }
            let port = EPH_BASE + i as u16;
            if self.contains(port) {
                if seen % stride == 0 {
                    out.push(port);
                }
                seen += 1;
            }
        }
        out
    }
}

fn toeplitz(key: &[u8], data: &[u8]) -> u32 {
    if key.len() < 4 {
        return 0;
    }
    let mut result: u32 = 0;
    let mut w = u32::from_be_bytes([key[0], key[1], key[2], key[3]]);
    let mut next_bit = 32usize;
    let total_key_bits = key.len() * 8;
    for &byte in data {
        for b in (0..8).rev() {
            if (byte >> b) & 1 == 1 {
                result ^= w;
            }
            let kb = if next_bit < total_key_bits {
                (key[next_bit / 8] >> (7 - (next_bit % 8))) & 1
            } else {
                0
            };
            w = (w << 1) | (kb as u32);
            next_bit += 1;
        }
    }
    result
}
