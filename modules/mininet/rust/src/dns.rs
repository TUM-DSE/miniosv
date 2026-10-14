//! One A query at boot, answered for every worker at once: the ENA does not
//! steer UDP by port (replies land on queue 0), so boot polls every queue and
//! the workers share out the addresses. Afterwards the queue-0 worker asks
//! again every minute, as curl's resolver cache would, and a worker adopts a
//! new address at its next dial. A records and in-reply CNAMEs only.

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, AtomicUsize, Ordering};

use smoltcp::iface::{Interface, SocketHandle, SocketSet, SocketStorage};
use smoltcp::socket::udp;
use smoltcp::time::Instant;
use smoltcp::wire::{IpAddress, IpEndpoint, Ipv4Address};

use crate::clock::MonoClock;
use crate::device::DpdkDevice;
use crate::error::Error;

const MAX_PACKET: usize = 512;
/// The VPC resolver ignores a fresh instance for 4-6 s after its lease.
const TIMEOUT_MS: i64 = 15_000;
const RETRY_MS: i64 = 1000;
const MAX_QUERIES: usize = 3;
const PORT: u16 = 40053;
pub(crate) const MAX_PEERS: usize = 16;
/// curl's DNS cache lifetime.
const REFRESH_MS: i64 = 60_000;

/// The addresses resolved, shared by every worker and read at each dial.
const NO_PEER: AtomicU32 = AtomicU32::new(0);
static PEERS: [AtomicU32; MAX_PEERS] = [NO_PEER; MAX_PEERS];
static N_PEERS: AtomicUsize = AtomicUsize::new(0);

pub(crate) fn publish(found: &[[u8; 4]]) {
    for (slot, a) in PEERS.iter().zip(found) {
        slot.store(u32::from_be_bytes(*a), Ordering::Relaxed);
    }
    N_PEERS.store(found.len().min(MAX_PEERS), Ordering::Release);
}

/// The address worker `i` dials; `None` before anything was resolved.
pub(crate) fn peer_for(i: usize) -> Option<[u8; 4]> {
    let n = N_PEERS.load(Ordering::Acquire);
    (n > 0).then(|| PEERS[i % n].load(Ordering::Relaxed).to_be_bytes())
}

fn snapshot() -> Vec<[u8; 4]> {
    let n = N_PEERS.load(Ordering::Acquire);
    (0..n).map(|i| PEERS[i].load(Ordering::Relaxed).to_be_bytes()).collect()
}

/// A recursive A query for `host`; the length written.
pub(crate) fn query(id: u16, host: &str, out: &mut [u8]) -> Option<usize> {
    if out.len() < 12 {
        return None;
    }
    out[0..2].copy_from_slice(&id.to_be_bytes());
    out[2..12].copy_from_slice(&[0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0]); // RD; one question
    let mut p = 12;
    for label in host.trim_end_matches('.').split('.') {
        if label.is_empty() || label.len() > 63 || p + 1 + label.len() + 5 > out.len() {
            return None;
        }
        out[p] = label.len() as u8;
        out[p + 1..p + 1 + label.len()].copy_from_slice(label.as_bytes());
        p += 1 + label.len();
    }
    out[p..p + 5].copy_from_slice(&[0, 0, 1, 0, 1]); // root; A; IN
    Some(p + 5)
}

/// A NOERROR reply to `id`: its A records join `out`, deduplicated.
pub(crate) fn parse(id: u16, pkt: &[u8], out: &mut Vec<[u8; 4]>) -> bool {
    if pkt.len() < 12 || pkt[0..2] != id.to_be_bytes() || pkt[2] & 0x80 == 0 || pkt[3] & 0x0f != 0 {
        return false;
    }
    let (qd, an) = (u16::from_be_bytes([pkt[4], pkt[5]]), u16::from_be_bytes([pkt[6], pkt[7]]));
    let mut p = 12;
    for _ in 0..qd {
        p = match skip_name(pkt, p) {
            Some(p) => p + 4,
            None => return false,
        };
    }
    for _ in 0..an {
        let Some(name_end) = skip_name(pkt, p) else { return false };
        let Some(fixed) = pkt.get(name_end..name_end + 10) else { return false };
        let rdlen = u16::from_be_bytes([fixed[8], fixed[9]]) as usize;
        let Some(rdata) = pkt.get(name_end + 10..name_end + 10 + rdlen) else { return false };
        if fixed[..4] == [0, 1, 0, 1] && rdlen == 4 {
            let a = [rdata[0], rdata[1], rdata[2], rdata[3]];
            if !out.contains(&a) {
                out.push(a);
            }
        }
        p = name_end + 10 + rdlen;
    }
    true
}

fn skip_name(pkt: &[u8], mut p: usize) -> Option<usize> {
    loop {
        let len = *pkt.get(p)? as usize;
        if len == 0 {
            return Some(p + 1);
        }
        if len & 0xc0 == 0xc0 {
            return Some(p + 2);
        }
        p += 1 + len;
    }
}

/// Every address `host` has, up to `want`, with all queues polled; published.
pub(crate) fn resolve(
    iface: &mut Interface,
    devs: &mut [DpdkDevice],
    resolver: [u8; 4],
    host: &str,
    want: usize,
    clk: &MonoClock,
) -> Result<Vec<[u8; 4]>, Error> {
    let (mut rx_meta, mut rx_buf) = ([udp::PacketMetadata::EMPTY; 1], [0u8; MAX_PACKET]);
    let (mut tx_meta, mut tx_buf) = ([udp::PacketMetadata::EMPTY; 1], [0u8; MAX_PACKET]);
    let mut sock = udp::Socket::new(
        udp::PacketBuffer::new(&mut rx_meta[..], &mut rx_buf[..]),
        udp::PacketBuffer::new(&mut tx_meta[..], &mut tx_buf[..]),
    );
    sock.bind(PORT).map_err(|_| Error::Dns)?;
    let mut storage = [SocketStorage::EMPTY; 1];
    let mut sockets = SocketSet::new(&mut storage[..]);
    let h = sockets.add(sock);
    let to = IpEndpoint::new(IpAddress::Ipv4(Ipv4Address::from_octets(resolver)), 53);

    let begin = clk.elapsed_ms();
    let mut found: Vec<[u8; 4]> = Vec::new();
    let mut msg = [0u8; MAX_PACKET];
    let (mut sent, mut replies) = (0u32, 0u32);
    for attempt in 0..MAX_QUERIES {
        let id = (clk.elapsed_ns() as u16) ^ (0x1357u16.wrapping_mul(attempt as u16 + 1));
        let n = query(id, host, &mut msg).ok_or(Error::Dns)?;
        let known = found.len();
        let start = clk.elapsed_ms();
        let mut next_send = start;
        let mut answered = false;
        'attempt: loop {
            let now = clk.elapsed_ms();
            if now >= start + TIMEOUT_MS {
                break;
            }
            if now >= next_send {
                if sockets.get_mut::<udp::Socket>(h).send_slice(&msg[..n], to).is_ok() {
                    sent += 1;
                }
                next_send = now + RETRY_MS;
            }
            for dev in devs.iter_mut() {
                iface.poll(Instant::from_millis(now), dev, &mut sockets);
                dev.flush_tx();
                if let Ok((data, _)) = sockets.get_mut::<udp::Socket>(h).recv() {
                    replies += 1;
                    if parse(id, data, &mut found) {
                        answered = true;
                        break 'attempt;
                    }
                }
            }
        }
        if !answered || found.len() >= want || (attempt > 0 && found.len() == known) {
            break;
        }
    }
    if found.is_empty() {
        println!(
            "FAIL: DNS: no address for {} from {}.{}.{}.{}: {} queries sent, {} datagrams back",
            host, resolver[0], resolver[1], resolver[2], resolver[3], sent, replies
        );
        return Err(Error::Dns);
    }
    println!("DNS: {} address(es) for {} after {} ms", found.len(), host, clk.elapsed_ms() - begin);
    for a in &found {
        println!("DNS: {} -> {}.{}.{}.{}", host, a[0], a[1], a[2], a[3]);
    }
    publish(&found);
    Ok(found)
}

/// The queue-0 worker's periodic query: a UDP socket in its set, a datagram a
/// minute, the table replaced when the answer changed.
pub(crate) struct Refresher {
    handle: SocketHandle,
    to: IpEndpoint,
    host: String,
    next_ms: i64,
    pending: Option<(u16, i64)>,
    found: Vec<[u8; 4]>,
    last: Vec<[u8; 4]>,
}

impl Refresher {
    pub(crate) fn new(sockets: &mut SocketSet<'static>, resolver: [u8; 4], host: &str, now_ms: i64) -> Option<Self> {
        let buf = || {
            udp::PacketBuffer::new(
                Box::leak(alloc::vec![udp::PacketMetadata::EMPTY; 1].into_boxed_slice()),
                Box::leak(alloc::vec![0u8; MAX_PACKET].into_boxed_slice()),
            )
        };
        let mut sock = udp::Socket::new(buf(), buf());
        sock.bind(PORT).ok()?;
        Some(Self {
            handle: sockets.add(sock),
            to: IpEndpoint::new(IpAddress::Ipv4(Ipv4Address::from_octets(resolver)), 53),
            host: host.into(),
            next_ms: now_ms + REFRESH_MS,
            pending: None,
            found: Vec::new(),
            last: snapshot(),
        })
    }

    pub(crate) fn step(&mut self, sockets: &mut SocketSet<'_>, now_ms: i64) {
        let s = sockets.get_mut::<udp::Socket>(self.handle);
        match self.pending {
            Some((id, deadline)) => {
                if let Ok((data, _)) = s.recv() {
                    if parse(id, data, &mut self.found) && !self.found.is_empty() {
                        let changed = self.found != self.last;
                        if changed {
                            publish(&self.found);
                            self.last = core::mem::take(&mut self.found);
                        }
                        println!("DNS: refreshed {}: {} address(es){}", self.host, self.last.len(), if changed { ", changed" } else { "" });
                        self.pending = None;
                    }
                } else if now_ms >= deadline {
                    self.pending = None; // the old answer stands
                }
            }
            None if now_ms >= self.next_ms => {
                let id = now_ms as u16 ^ 0x2468;
                let mut msg = [0u8; MAX_PACKET];
                if let Some(n) = query(id, &self.host, &mut msg) {
                    let _ = s.send_slice(&msg[..n], self.to);
                }
                self.found.clear();
                self.pending = Some((id, now_ms + 3000));
                self.next_ms = now_ms + REFRESH_MS;
            }
            None => {}
        }
    }
}
