//! One worker: one RSS queue, one interface, one set of connections.
//!
//! A worker is shared-nothing. It owns its queue's mempool, its own
//! `Interface`, its own sockets, and a set of source ports whose return
//! traffic the NIC provably steers to it. Nothing on its data path is shared
//! with another worker, so nothing on it locks.
//!
//! Because the port partition is a function of the *peer's* address as well as
//! ours, a worker is built for one [`Endpoint`] and stays with it.

use alloc::boxed::Box;
use alloc::sync::Arc;
use alloc::vec::Vec;

use rustls::client::ClientConfig;
use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet, SocketStorage};
use smoltcp::socket::tcp;
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, IpCidr, Ipv4Address};

use crate::arp;
use crate::clock::MonoClock;
use crate::conn::{Conn, Step};
use crate::device::DpdkDevice;
use crate::endpoint::{Endpoint, Request};
use crate::error::Error;
use crate::ffi::rte_pktmbuf_pool;
use crate::rss::{Rss, EPH_LEN};
use crate::tls;
use crate::Netif;

/// How many source ports a slot rotates through before repeating one.
const PORT_ROTATION: usize = 8;

/// How a worker is sized, and who it talks to.
pub struct WorkerConfig {
    /// The server this worker's connections dial. Fixed for the worker's life:
    /// the source ports it may use are derived from this address.
    pub peer: Endpoint,
    /// Connection slots. Capped by how many usable source ports steer here.
    pub conns: usize,
    /// Per-socket receive buffer. The dominant memory cost of the stack:
    /// `conns * workers * rx_buffer`.
    pub rx_buffer: usize,
    pub tx_buffer: usize,
}

impl WorkerConfig {
    pub fn new(peer: Endpoint) -> Self {
        Self {
            peer,
            conns: 1,
            rx_buffer: 4 * 1024 * 1024,
            tx_buffer: 32 * 1024,
        }
    }
}

/// A `Send` ticket for one queue.
///
/// Workers are built on the thread that polls them -- an `Interface` and its
/// sockets must not migrate between CPUs -- so this is what crosses the thread
/// boundary. The pool pointer is owned by the [`crate::Stack`], which outlives
/// every worker.
#[derive(Clone, Copy)]
pub struct WorkerHandle {
    pub(crate) queue_id: u16,
    pub(crate) pool: *mut rte_pktmbuf_pool,
    pub(crate) netif: Netif,
    pub(crate) rss: Rss,
}

// SAFETY: the mempool is per-queue and only ever touched by the one worker
// that owns this handle; the shim's pool is internally locked in any case.
unsafe impl Send for WorkerHandle {}

impl WorkerHandle {
    /// Also the CPU a caller should pin the worker's thread to.
    pub fn queue_id(&self) -> u16 {
        self.queue_id
    }
}

pub struct Worker {
    queue_id: u16,
    iface: Interface,
    dev: DpdkDevice,
    sockets: SocketSet<'static>,
    handles: Vec<SocketHandle>,
    conns: Vec<Option<Conn>>,
    ports: Vec<u16>,
    rotation: Vec<u16>,
    next_port: usize,
    peer: Endpoint,
    tls_config: Arc<ClientConfig>,
    clk: MonoClock,
}

impl Worker {
    pub fn new(h: WorkerHandle, cfg: &WorkerConfig) -> Result<Worker, Error> {
        let clk = MonoClock::new();
        let netif = h.netif;

        let owned = h
            .rss
            .owned_ports(cfg.peer.ip, cfg.peer.port, netif.ip, h.queue_id);
        let ports = owned.spread(cfg.conns);
        // A slot that serves request after request cannot keep reusing one
        // port: even aborted, dialling the same 4-tuple again immediately
        // risks the peer still holding the old connection. Rotating through a
        // wider set costs 2 bytes each and removes the question.
        let rotation = owned.spread(cfg.conns * PORT_ROTATION);
        if ports.is_empty() {
            println!("FAIL: q{}: no ephemeral port steers here", h.queue_id);
            return Err(Error::NoPorts);
        }
        if ports.len() < cfg.conns {
            println!(
                "q{}: only {} usable ports for {} connections",
                h.queue_id,
                ports.len(),
                cfg.conns
            );
        }
        println!(
            "q{}: {} of {} ephemeral ports steer here; using {}",
            h.queue_id,
            owned.count(),
            EPH_LEN,
            ports.len()
        );

        let ip = Ipv4Address::from_octets(netif.ip);
        let gw = Ipv4Address::from_octets(netif.gateway_ip);

        // Seed the neighbour cache from the reply queue 0 already got: an ARP
        // exchange from here would not be steered back to this queue.
        let synth = arp::synthetic_reply(netif.gateway_mac, netif.gateway_ip, netif.mac, netif.ip);
        let mut dev = DpdkDevice::new(h.queue_id, h.pool, Some(owned), Some(synth));

        let config = Config::new(EthernetAddress(netif.mac).into());
        let mut iface = Interface::new(config, &mut dev, Instant::from_millis(clk.elapsed_ms()));
        iface.update_ip_addrs(|addrs| {
            let _ = addrs.push(IpCidr::new(ip.into(), netif.prefix_len));
        });
        let _ = iface.routes_mut().add_default_ipv4_route(gw);

        // Socket storage and buffers are leaked so their 'static lifetime
        // satisfies SocketSet's borrow. A worker lives for the life of the
        // program, so there is nothing to reclaim -- but this is why dropping
        // one does not give the memory back.
        let slots = ports.len();
        let storage: &'static mut [SocketStorage<'static>] = Box::leak(
            (0..slots)
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
            // Delayed ACKs cost a round trip per window at these rates.
            sock.set_ack_delay(None);
            handles.push(sockets.add(sock));
        }

        let mut conns = Vec::with_capacity(slots);
        conns.resize_with(slots, || None);

        Ok(Worker {
            queue_id: h.queue_id,
            iface,
            dev,
            sockets,
            handles,
            conns,
            ports,
            rotation,
            next_port: 0,
            peer: cfg.peer.clone(),
            tls_config: tls::client_config(),
            clk,
        })
    }

    pub fn queue_id(&self) -> u16 {
        self.queue_id
    }

    /// Connection slots this worker actually has -- `WorkerConfig::conns`
    /// unless too few source ports steer here.
    pub fn slots(&self) -> usize {
        self.handles.len()
    }

    pub fn peer(&self) -> &Endpoint {
        &self.peer
    }

    pub fn clock(&self) -> &MonoClock {
        &self.clk
    }

    /// Open `slot` and queue `req` on it. The request goes out as soon as the
    /// socket can carry it -- immediately without TLS, after the handshake
    /// with it.
    pub fn connect(&mut self, slot: usize, req: &Request<'_>) -> Result<(), Error> {
        let src_port = *self.ports.get(slot).ok_or(Error::ConnectRejected)?;
        self.connect_on(slot, src_port, req)
    }

    /// Whether this slot is idle and can take a request.
    pub fn is_free(&self, slot: usize) -> bool {
        self.conns.get(slot).map_or(false, |c| c.is_none())
    }

    /// Tear down whatever is on `slot` and make it available again.
    ///
    /// Aborts rather than closes: the response is already in hand, so there is
    /// nothing left to receive, and a RST leaves no TIME_WAIT holding the
    /// 4-tuple. A slot that had to wait out TIME_WAIT before its next request
    /// would cap request rate at a few per minute per port.
    pub fn release(&mut self, slot: usize) {
        if let Some(&handle) = self.handles.get(slot) {
            self.sockets.get_mut::<tcp::Socket>(handle).abort();
        }
        if let Some(c) = self.conns.get_mut(slot) {
            *c = None;
        }
    }

    /// Whether `slot` holds a finished connection the peer hasn't closed --
    /// reusable for a new request instead of a fresh connect. Only [`Service`]
    /// calls this; the raw benchmark API (`connect`/`release`) is unaffected.
    ///
    /// [`Service`]: crate::service::Service
    pub(crate) fn idle_reusable(&self, slot: usize) -> bool {
        self.conns
            .get(slot)
            .and_then(|c| c.as_ref())
            .map_or(false, |c| c.idle_reusable(&self.sockets))
    }

    /// Reuse the connection already open on `slot` for `req`. Only call when
    /// [`Worker::idle_reusable`] was just true for this slot.
    pub(crate) fn reuse(&mut self, slot: usize, req: &Request<'_>) -> Result<(), Error> {
        let now_ms = self.clk.elapsed_ms();
        match self.conns.get_mut(slot).and_then(|c| c.as_mut()) {
            Some(c) => {
                c.reset_for(req, now_ms);
                crate::stats::request_started(true);
                Ok(())
            }
            None => Err(Error::ConnectRejected),
        }
    }

    /// Open `slot` on the next port in the rotation. Used when a slot is
    /// serving a stream of requests rather than one fixed range.
    pub fn connect_next(&mut self, slot: usize, req: &Request<'_>) -> Result<(), Error> {
        if self.rotation.is_empty() {
            return Err(Error::NoPorts);
        }
        let src_port = self.rotation[self.next_port % self.rotation.len()];
        self.next_port = self.next_port.wrapping_add(1);
        self.connect_on(slot, src_port, req)
    }

    fn connect_on(&mut self, slot: usize, src_port: u16, req: &Request<'_>) -> Result<(), Error> {
        let handle = *self.handles.get(slot).ok_or(Error::ConnectRejected)?;
        let dst = (Ipv4Address::from_octets(self.peer.ip), self.peer.port);

        {
            let s = self.sockets.get_mut::<tcp::Socket>(handle);
            if s.connect(self.iface.context(), dst, src_port).is_err() {
                println!("q{}[{}] connect() rejected", self.queue_id, slot);
                return Err(Error::ConnectRejected);
            }
        }
        crate::stats::request_started(false);

        let conn = Conn::new(
            handle,
            self.queue_id,
            src_port,
            &self.peer,
            req,
            &self.tls_config,
            self.clk.elapsed_ms(),
        )?;
        self.conns[slot] = Some(conn);
        Ok(())
    }

    /// One iteration of the poll loop: advance the interface, then every open
    /// connection. Returns true when none of them are still in flight.
    ///
    /// A worker with no open connections is trivially done, so a caller that
    /// loops on this must open something first.
    pub fn poll(&mut self) -> bool {
        let now_ms = self.clk.elapsed_ms();
        self.iface
            .poll(Instant::from_millis(now_ms), &mut self.dev, &mut self.sockets);

        let mut all_done = true;
        for slot in self.conns.iter_mut().flatten() {
            if slot.step(&mut self.sockets, &self.clk) == Step::Pending {
                all_done = false;
            }
        }
        all_done
    }

    pub fn conn(&self, slot: usize) -> Option<&Conn> {
        self.conns.get(slot).and_then(|c| c.as_ref())
    }

    pub fn conn_mut(&mut self, slot: usize) -> Option<&mut Conn> {
        self.conns.get_mut(slot).and_then(|c| c.as_mut())
    }

    /// Every connection that was opened, in slot order.
    pub fn conns(&self) -> impl Iterator<Item = &Conn> {
        self.conns.iter().flatten()
    }
}
