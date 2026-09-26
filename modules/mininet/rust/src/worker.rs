//! One worker: one RSS queue, one interface, one set of connections, and the
//! source ports whose return traffic steers to that queue. Shared-nothing.

use alloc::boxed::Box;
use alloc::sync::Arc;
use alloc::vec::Vec;

use rustls::client::ClientConfig;
use smoltcp::iface::{Config, Interface, PollResult, SocketHandle, SocketSet, SocketStorage};
use smoltcp::socket::tcp;
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, IpCidr, Ipv4Address};

use crate::arp;
use crate::clock::MonoClock;
use crate::conn::{Conn, ConnBufs};
use crate::device::DpdkDevice;
use crate::endpoint::Endpoint;
use crate::error::Error;
use crate::http::BodySink;
use crate::ffi::rte_pktmbuf_pool;
use crate::rss::{Rss, EPH_LEN};
use crate::tls;
use crate::Netif;

const PORT_ROTATION: usize = 8;

pub struct WorkerConfig {
    pub peer: Endpoint,
    pub conns: usize,
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

/// A `Send` ticket for one queue; the worker itself is built on its own thread.
#[derive(Clone, Copy)]
pub struct WorkerHandle {
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

        let owned = h
            .rss
            .owned_ports(cfg.peer.ip, cfg.peer.port, netif.ip, h.queue_id);
        let slots = owned.spread(cfg.conns).len();
        let rotation = owned.spread(cfg.conns * PORT_ROTATION);
        if slots == 0 {
            println!("FAIL: q{}: no ephemeral port steers here", h.queue_id);
            return Err(Error::NoPorts);
        }
        if slots < cfg.conns {
            println!("q{}: only {} usable ports for {} connections", h.queue_id, slots, cfg.conns);
        }
        println!(
            "q{}: {} of {} ephemeral ports steer here; using {}",
            h.queue_id,
            owned.count(),
            EPH_LEN,
            slots
        );

        let ip = Ipv4Address::from_octets(netif.ip);
        let gw = Ipv4Address::from_octets(netif.gateway_ip);

        // ARP replies would not steer to this queue; seed the cache instead.
        let synth = arp::synthetic_reply(netif.gateway_mac, netif.gateway_ip, netif.mac, netif.ip);
        let mut dev = DpdkDevice::new(h.queue_id, h.pool, Some(owned), Some(synth));

        let config = Config::new(EthernetAddress(netif.mac).into());
        let mut iface = Interface::new(config, &mut dev, Instant::from_millis(clk.elapsed_ms()));
        iface.update_ip_addrs(|addrs| {
            let _ = addrs.push(IpCidr::new(ip.into(), netif.prefix_len));
        });
        let _ = iface.routes_mut().add_default_ipv4_route(gw);

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
            sock.set_ack_delay(None);
            handles.push(sockets.add(sock));
        }

        let mut conns = Vec::with_capacity(slots);
        conns.resize_with(slots, || None);

        let mut bufs = Vec::with_capacity(slots);
        bufs.resize_with(slots, || Some(ConnBufs::new()));

        Ok(Worker {
            queue_id: h.queue_id,
            iface,
            dev,
            sockets,
            handles,
            conns,
            bufs,
            rotation,
            next_port: 0,
            peer: cfg.peer.clone(),
            tls_config: tls::client_config(),
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

    /// Open `slot` on the next port in the rotation.
    pub fn connect_next(&mut self, slot: usize, head: &[u8], sink: Box<dyn BodySink>) -> Result<(), Error> {
        let src_port = self.rotation[self.next_port % self.rotation.len()];
        self.next_port = self.next_port.wrapping_add(1);
        self.connect_on(slot, src_port, head, sink)
    }

    fn connect_on(&mut self, slot: usize, src_port: u16, head: &[u8], sink: Box<dyn BodySink>) -> Result<(), Error> {
        let handle = *self.handles.get(slot).ok_or(Error::ConnectRejected)?;
        let dst = (Ipv4Address::from_octets(self.peer.ip), self.peer.port);
        let dial_start_ns = self.clk.elapsed_ns();

        let tls = Conn::session(&self.peer, &self.tls_config)?;

        {
            let s = self.sockets.get_mut::<tcp::Socket>(handle);
            if s.connect(self.iface.context(), dst, src_port).is_err() {
                println!("q{}[{}] connect() rejected", self.queue_id, slot);
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
        let res = self
            .iface
            .poll(Instant::from_millis(now_ms), &mut self.dev, &mut self.sockets);
        self.last_active = matches!(res, PollResult::SocketStateChanged);
        self.dev.flush_tx();
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
