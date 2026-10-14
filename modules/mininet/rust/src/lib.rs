/*!
mininet: smoltcp + rustls over minidpdk, no socket layer. RSS steering is a
function of the 4-tuple and we pick the source port, so each worker owns one
queue outright: [`Stack::up`] once at boot, then one pinned [`Worker`] per queue.
Queues come from every port the drivers registered -- one per NIC, so one per
ENI on EC2 -- because a NIC caps queues well below an instance's core count.
*/

#![no_std]
#![allow(non_camel_case_types)]

extern crate alloc;

#[macro_use]
pub mod print;

mod allocator;
mod capi;
mod arp;
mod clock;
mod conn;
mod device;
mod dhcp;
mod dns;
mod endpoint;
mod error;
mod ffi;
mod http;
mod nic;
mod rss;
#[cfg(feature = "selftest")]
mod selftest;
mod service;
pub mod stats;
pub mod thread;
mod tls;
mod worker;

pub use clock::MonoClock;

/// Cpus the kernel runs on; workers are pinned from 0 upwards.
pub fn cpu_count() -> usize {
    unsafe { ffi::shim_cpu_count() as usize }
}
pub use conn::{Conn, Step};
pub use endpoint::Endpoint;
pub use error::Error;
pub use http::{BodySink, BufferSink, ContentRange, ResponseHead};
pub use nic::{eth_stats, NicStats};
pub use service::{GetResult, Service, ServiceConfig};
pub use worker::{Worker, WorkerConfig, WorkerHandle};

use alloc::vec::Vec;
use core::panic::PanicInfo;

pub struct Config<'a> {
    /// RSS queues, and so workers: at least one, at most every queue of every
    /// port. 0 asks for all of them.
    pub queues: u16,
    /// RX descriptors per queue to ask for; 0 is the default of 4096, and the
    /// device clamps what it cannot give.
    pub rx_desc: u16,
    /// The one host dialled, if known: on the local subnet it is resolved on
    /// queue 0 in the gateway's place, since ARP replies steer nowhere else.
    pub peer: Option<[u8; 4]>,
    /// A name to resolve at boot; each worker then dials one of its addresses.
    pub resolve: Option<&'a str>,
}

/// Learned once on queue 0: DHCP and ARP replies are not steered by RSS.
#[derive(Clone, Copy)]
pub struct Netif {
    pub mac: [u8; 6],
    pub ip: [u8; 4],
    pub prefix_len: u8,
    pub gateway_ip: [u8; 4],
    pub gateway_mac: [u8; 6],
    /// DHCP's resolver, for the worker that refreshes the name.
    pub dns: [u8; 4],
}

/// One started port: its queues' mempools, its steering model and its lease.
/// Nothing is shared with another port -- a second NIC has its own MAC, its
/// own address and its own RSS table, so it is a second independent domain.
struct Port {
    id: u16,
    pools: Vec<nic::PktPool>,
    rss: rss::Rss,
    netif: Netif,
    queues: u16,
}

/// The started ports, each with a lease and a known next hop; owns the mempools.
pub struct Stack {
    ports: Vec<Port>,
    queues: u16,
}

impl Stack {
    /// Start as many ports as it takes to give `cfg.queues` workers a queue of
    /// their own, each port with its own RSS model, lease and next hop. Ports
    /// are filled in order, so asking for no more than the first port's queues
    /// brings up only that one and behaves exactly as a single-NIC image did.
    pub fn up(cfg: &Config<'_>) -> Result<Stack, Error> {
        let n_ports = nic::count();
        let available: u16 = (0..n_ports).map(|p| nic::clamp_queues(p, u16::MAX)).sum();
        // One cpu stays with the application: a worker never yields its own.
        let cpus = unsafe { crate::ffi::shim_cpu_count() } as u16;
        let ceiling = core::cmp::min(available.max(1), cpus.saturating_sub(1).max(1));
        // 0 asks for everything the hardware and the cpus allow.
        let want = if cfg.queues == 0 { ceiling } else { cfg.queues.max(1) };
        let queues = core::cmp::min(want, ceiling);
        if queues != want {
            println!(
                "clamping workers {} -> {} ({} port(s), {} queues, {} cpus)",
                want, queues, n_ports, available, cpus
            );
        }
        if n_ports > 1 {
            println!("{} ports registered, {} queues available", n_ports, available);
        }

        let mut ports: Vec<Port> = Vec::new();
        let mut left = queues;
        for id in 0..n_ports {
            if left == 0 {
                break;
            }
            let n = core::cmp::min(left, nic::clamp_queues(id, left));
            if n == 0 {
                continue;
            }
            let (pools, mac) = nic::probe_and_open(id, n, cfg.rx_desc)?;
            let rss = rss::Rss::load(id, n)?;
            let netif = dhcp::learn_network(id, &pools, mac, cfg.peer, cfg.resolve)?;
            ports.push(Port { id, pools, rss, netif, queues: n });
            left -= n;
        }
        if ports.is_empty() {
            return Err(Error::NoDevice);
        }
        // The harness reads the worker count off this line; the per-port detail
        // is in the "rss p<n>:" lines above.
        println!("rss: {} queues over {} port(s)", queues - left, ports.len());
        Ok(Stack { queues: queues - left, ports })
    }

    pub fn queues(&self) -> u16 {
        self.queues
    }

    /// Ports this stack started; a worker index spans all of them.
    pub fn ports(&self) -> u16 {
        self.ports.len() as u16
    }

    /// A `Send` ticket for one worker: the nth queue across the started ports,
    /// in order. `None` past what the devices granted.
    pub fn handle(&self, worker: u16) -> Option<WorkerHandle> {
        let mut n = worker;
        for p in &self.ports {
            if n < p.queues {
                return Some(WorkerHandle {
                    port: p.id,
                    queue_id: n,
                    pool: p.pools[n as usize].as_ptr(),
                    netif: p.netif,
                    rss: p.rss,
                });
            }
            n -= p.queues;
        }
        None
    }
}

/// Print, then the kernel's abort: a halted guest is a verdict the harness
/// sees, a worker spinning on its cpu with every submitter parked is not.
#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    println!("mininet: PANIC: {}", info.message());
    if let Some(loc) = info.location() {
        println!("mininet: at {}:{}", loc.file(), loc.line());
    }
    unsafe { ffi::shim_abort(c"mininet: panic".as_ptr()) }
}

/// Named by the eh_frame rustc emits; there is no unwinding.
#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}
