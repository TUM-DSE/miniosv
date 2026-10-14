//! One worker: one RSS queue of one port, one interface, one set of
//! connections, and the source ports whose return traffic steers to that
//! queue. Shared-nothing, across ports as well as queues.

use alloc::boxed::Box;
use alloc::sync::Arc;
use alloc::vec::Vec;

use rustls::client::ClientConfig;
use smoltcp::iface::{Config, Interface, PollResult, SocketHandle, SocketSet, SocketStorage};
use smoltcp::socket::tcp;
use smoltcp::time::{Duration, Instant};
use smoltcp::wire::{EthernetAddress, IpCidr, Ipv4Address};

use crate::arp;
use crate::clock::MonoClock;
use crate::conn::{Conn, ConnBufs};
use crate::device::DpdkDevice;
use crate::dns;
use crate::endpoint::Endpoint;
use crate::error::Error;
use crate::http::BodySink;
use crate::ffi::rte_pktmbuf_pool;
use crate::rss::{Rss, EPH_LEN};
use crate::tls;
use crate::Netif;

const PORT_ROTATION: usize = 8;
/// smoltcp forgets a neighbour after 60 s, and only queue 0 hears ARP replies.
const RESEED_MS: i64 = 30_000;

pub struct WorkerConfig {
    pub peer: Endpoint,
    pub conns: usize,
    pub rx_buffer: usize,
    pub tx_buffer: usize,
    /// Dial the boot-resolved address for this queue, not `peer.ip`.
    pub resolve: bool,
}

impl WorkerConfig {
    pub fn new(peer: Endpoint) -> Self {
        Self {
            peer,
            conns: 1,
            rx_buffer: 4 * 1024 * 1024,
            tx_buffer: 32 * 1024,
            resolve: false,
        }
    }
}

/// A `Send` ticket for one queue; the worker itself is built on its own thread.
#[derive(Clone, Copy)]
pub struct WorkerHandle {
    pub(crate) port: u16,
    pub(crate) queue_id: u16,
    pub(crate) pool: *mut rte_pktmbuf_pool,
    pub(crate) netif: Netif,
    pub(crate) rss: Rss,
}

unsafe impl Send for WorkerHandle {}

impl WorkerHandle {
    pub fn queue_id(&self) -> u16 {
        self.queue_id
    }
}

pub struct Worker {
    port: u16,
    queue_id: u16,
    iface: Interface,
    dev: DpdkDevice,
    sockets: SocketSet<'static>,
    handles: Vec<SocketHandle>,
    conns: Vec<Option<Conn>>,
    bufs: Vec<Option<ConnBufs>>,
    rotation: Vec<u16>,
    next_port: usize,
    peer: Endpoint,
    resolve: bool,
    shift: usize,
    want_conns: usize,
    netif: Netif,
    rss: Rss,
    refresher: Option<dns::Refresher>,
    synth: Vec<u8>,
    next_seed_ms: i64,
    tls_config: Arc<ClientConfig>,
    clk: MonoClock,
    last_poll_ns: u64,
    last_busy_ns: u64,
    last_iface_ns: u64,
    last_active: bool,
    last_dev: (u64, u64),
}

impl Worker {
    pub fn new(h: WorkerHandle, cfg: &WorkerConfig) -> Result<Worker, Error> {
        let clk = MonoClock::new();
        let netif = h.netif;
        let mut peer = cfg.peer.clone();
        if cfg.resolve {
            peer.ip = dns::peer_for(h.queue_id as usize).ok_or(Error::Dns)?;
            let a = peer.ip;
            println!("p{}q{}: {} -> {}.{}.{}.{}", h.port, h.queue_id, peer.host, a[0], a[1], a[2], a[3]);
        }

        let owned = h
            .rss
            .owned_ports(peer.ip, peer.port, netif.ip, h.queue_id);
        let slots = owned.spread(cfg.conns).len();
        let rotation = owned.spread(cfg.conns * PORT_ROTATION);
        if slots == 0 {
            println!("FAIL: p{}q{}: no ephemeral port steers here", h.port, h.queue_id);
            return Err(Error::NoPorts);
        }
        if slots < cfg.conns {
            println!("p{}q{}: only {} usable ports for {} connections", h.port, h.queue_id, slots, cfg.conns);
        }
        println!(
            "p{}q{}: {} of {} ephemeral ports steer here; using {}",
            h.port,
            h.queue_id,
            owned.count(),
            EPH_LEN,
            slots
        );

        let ip = Ipv4Address::from_octets(netif.ip);
        let gw = Ipv4Address::from_octets(netif.gateway_ip);

        // ARP replies would not steer to this queue; seed the cache instead.
        let synth = arp::synthetic_reply(netif.gateway_mac, netif.gateway_ip, netif.mac, netif.ip);
        let mut dev = DpdkDevice::new(h.port, h.queue_id, h.pool, Some(owned), Some(synth.clone()));

        let config = Config::new(EthernetAddress(netif.mac).into());
        let mut iface = Interface::new(config, &mut dev, Instant::from_millis(clk.elapsed_ms()));
        iface.update_ip_addrs(|addrs| {
            let _ = addrs.push(IpCidr::new(ip.into(), netif.prefix_len));
        });
        let _ = iface.routes_mut().add_default_ipv4_route(gw);

        let storage: &'static mut [SocketStorage<'static>] = Box::leak(
            (0..slots + 1)
                .map(|_| SocketStorage::EMPTY)
                .collect::<Vec<SocketStorage<'static>>>()
                .into_boxed_slice(),
        );
        let mut sockets: SocketSet<'static> = SocketSet::new(storage);

        let mut handles = Vec::with_capacity(slots);
        for _ in 0..slots {
            let rx: &'static mut [u8] =
                Box::leak(alloc::vec![0u8; cfg.rx_buffer].into_boxed_slice());
            let tx: &'static mut [u8] =
                Box::leak(alloc::vec![0u8; cfg.tx_buffer].into_boxed_slice());
            let mut sock = tcp::Socket::new(tcp::SocketBuffer::new(rx), tcp::SocketBuffer::new(tx));
            sock.set_ack_delay(None);
            sock.set_keep_alive(Some(Duration::from_secs(crate::conn::KEEP_ALIVE_S)));
            sock.set_timeout(Some(Duration::from_secs(crate::conn::PEER_TIMEOUT_S)));
            handles.push(sockets.add(sock));
        }
        let refresher = (cfg.resolve && h.port == 0 && h.queue_id == 0)
            .then(|| dns::Refresher::new(&mut sockets, netif.dns, &peer.host, clk.elapsed_ms()))
            .flatten();

        let mut conns = Vec::with_capacity(slots);
        conns.resize_with(slots, || None);

        let mut bufs = Vec::with_capacity(slots);
        bufs.resize_with(slots, || Some(ConnBufs::new()));

        Ok(Worker {
            port: h.port,
            queue_id: h.queue_id,
            iface,
            dev,
            sockets,
            handles,
            conns,
            bufs,
            rotation,
            next_port: 0,
            peer,
            resolve: cfg.resolve,
            shift: 0,
            want_conns: cfg.conns,
            netif,
            rss: h.rss,
            refresher,
            tls_config: tls::client_config(),
            synth,
            next_seed_ms: clk.elapsed_ms() + RESEED_MS,
            clk,
            last_poll_ns: 0,
            last_busy_ns: 0,
            last_iface_ns: 0,
            last_active: false,
            last_dev: (0, 0),
        })
    }

    pub fn slots(&self) -> usize {
        self.handles.len()
    }

    pub fn peer_ip(&self) -> [u8; 4] {
        self.peer.ip
    }

    /// The table's address for this queue; a new one gets its own port set.
    fn adopt(&mut self) {
        let Some(ip) = dns::peer_for(self.queue_id as usize + self.shift) else { return };
        if ip == self.peer.ip {
            return;
        }
        let owned = self.rss.owned_ports(ip, self.peer.port, self.netif.ip, self.queue_id);
        let rotation = owned.spread(self.want_conns * PORT_ROTATION);
        if rotation.is_empty() {
            return;
        }
        println!("p{}q{}: {} -> {}.{}.{}.{}", self.port, self.queue_id, self.peer.host, ip[0], ip[1], ip[2], ip[3]);
        self.peer.ip = ip;
        self.rotation = rotation;
        // Open connections still use the old ports: admit both.
        let mut all = owned;
        if let Some(old) = self.dev.owned_ports.take() {
            all.merge(&old);
        }
        self.dev.owned_ports = Some(all);
    }

    /// A dial that failed: the answer's next address from the next dial on.
    pub fn dial_failed(&mut self) {
        self.shift += 1;
    }

    pub fn clock(&self) -> &MonoClock {
        &self.clk
    }

    /// Tear down `slot`. Aborts rather than closes, so no TIME_WAIT holds the port.
    pub fn release(&mut self, slot: usize) {
        if let Some(&handle) = self.handles.get(slot) {
            self.sockets.get_mut::<tcp::Socket>(handle).abort();
        }
        if let Some(c) = self.conns.get_mut(slot).and_then(Option::take) {
            if let Some(b) = self.bufs.get_mut(slot) {
                *b = Some(c.into_bufs());
            }
        }
    }

    pub(crate) fn idle_reusable(&self, slot: usize) -> bool {
        self.conns
            .get(slot)
            .and_then(|c| c.as_ref())
            .map_or(false, |c| c.idle_reusable(&self.sockets))
    }

    /// Only after [`Worker::idle_reusable`] was just true.
    pub(crate) fn reuse(&mut self, slot: usize, head: &[u8], sink: Box<dyn BodySink>) -> Result<(), Error> {
        let now_ns = self.clk.elapsed_ns();
        match self.conns.get_mut(slot).and_then(|c| c.as_mut()) {
            Some(c) => {
                c.reset_for(head, sink, now_ns);
                crate::stats::request_started(true);
                Ok(())
            }
            None => Err(Error::ConnectRejected),
        }
    }

    /// Open `slot` on the next port in the rotation that no live socket holds.
    pub fn connect_next(&mut self, slot: usize, head: &[u8], sink: Box<dyn BodySink>) -> Result<(), Error> {
        if self.resolve {
            self.adopt(); // before the port: the rotation is the address's
        }
        // smoltcp does not check: a SYN on a 4-tuple the peer still has open
        // draws a challenge ACK on that connection and never a SYN-ACK, and
        // a kept-alive slot can outlive a whole turn of the rotation.
        let n = self.rotation.len();
        let mut src_port = None;
        for _ in 0..n {
            let p = self.rotation[self.next_port % n];
            self.next_port = self.next_port.wrapping_add(1);
            if !self.port_in_use(p) {
                src_port = Some(p);
                break;
            }
        }
        let Some(src_port) = src_port else {
            println!("p{}q{}[{}] every port in the rotation is open", self.port, self.queue_id, slot);
            return Err(Error::ConnectRejected);
        };
        self.connect_on(slot, src_port, head, sink)
    }

    fn port_in_use(&self, port: u16) -> bool {
        self.sockets.iter().any(|(_, s)| match s {
            smoltcp::socket::Socket::Tcp(t) => {
                t.state() != tcp::State::Closed && t.local_endpoint().map_or(false, |e| e.port == port)
            }
            _ => false,
        })
    }

    fn connect_on(&mut self, slot: usize, src_port: u16, head: &[u8], sink: Box<dyn BodySink>) -> Result<(), Error> {
        let handle = *self.handles.get(slot).ok_or(Error::ConnectRejected)?;
        let dst = (Ipv4Address::from_octets(self.peer.ip), self.peer.port);
        let dial_start_ns = self.clk.elapsed_ns();

        let tls = Conn::session(&self.peer, &self.tls_config)?;

        {
            let s = self.sockets.get_mut::<tcp::Socket>(handle);
            if s.connect(self.iface.context(), dst, src_port).is_err() {
                println!("p{}q{}[{}] connect() rejected", self.port, self.queue_id, slot);
                return Err(Error::ConnectRejected);
            }
        }
        crate::stats::request_started(false);

        let bufs = match self.conns[slot].take() {
            Some(old) => old.into_bufs(),
            None => self.bufs[slot].take().unwrap_or_else(ConnBufs::new),
        };

        let now_ns = self.clk.elapsed_ns();
        self.conns[slot] = Some(Conn::new(
            handle,
            self.port,
            self.queue_id,
            src_port,
            tls,
            head,
            sink,
            bufs,
            now_ns,
        ));
        crate::stats::conn_dialled(now_ns.saturating_sub(dial_start_ns));
        Ok(())
    }

    /// Advance the interface, then every open connection.
    pub fn poll(&mut self) {
        let start_ns = self.clk.elapsed_ns();
        self.last_poll_ns = start_ns + self.clk.epoch_ns();
        let now_ms = (start_ns / 1_000_000) as i64;
        if now_ms >= self.next_seed_ms {
            self.dev.pending_synth = Some(self.synth.clone());
            self.next_seed_ms = now_ms + RESEED_MS;
        }
        let res = self
            .iface
            .poll(Instant::from_millis(now_ms), &mut self.dev, &mut self.sockets);
        self.last_active = matches!(res, PollResult::SocketStateChanged);
        self.dev.flush_tx();
        if let Some(r) = self.refresher.as_mut() {
            r.step(&mut self.sockets, now_ms);
        }
        let now_ns = self.clk.elapsed_ns();
        self.last_iface_ns = now_ns.saturating_sub(start_ns);
        self.last_dev = self.dev.take_counts();

        for slot in self.conns.iter_mut().flatten() {
            slot.step(&mut self.sockets, now_ns);
        }
        self.last_busy_ns = self.clk.elapsed_ns().saturating_sub(start_ns);
    }

    pub fn last_poll_ns(&self) -> u64 {
        self.last_poll_ns
    }

    pub fn last_busy_ns(&self) -> u64 {
        self.last_busy_ns
    }

    pub fn last_iface_ns(&self) -> u64 {
        self.last_iface_ns
    }

    pub fn last_dev(&self) -> (u64, u64) {
        self.last_dev
    }

    pub fn last_active(&self) -> bool {
        self.last_active
    }

    pub fn conn(&self, slot: usize) -> Option<&Conn> {
        self.conns.get(slot).and_then(|c| c.as_ref())
    }

    pub fn conn_mut(&mut self, slot: usize) -> Option<&mut Conn> {
        self.conns.get_mut(slot).and_then(|c| c.as_mut())
    }
}
