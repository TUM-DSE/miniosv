/*!
mininet: smoltcp + rustls over minidpdk, no socket layer. RSS steering is a
function of the 4-tuple and we pick the source port, so each worker owns one
queue outright: [`Stack::up`] once at boot, then one pinned [`Worker`] per queue.
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

pub struct Config {
    /// RSS queues, and so workers: at least one, at most what the device advertises.
    pub queues: u16,
    /// RX descriptors per queue to ask for; 0 is the default of 4096, and the
    /// device clamps what it cannot give.
    pub rx_desc: u16,
}

/// Learned once on queue 0: DHCP and ARP replies are not steered by RSS.
#[derive(Clone, Copy)]
pub struct Netif {
    pub mac: [u8; 6],
    pub ip: [u8; 4],
    pub prefix_len: u8,
    pub gateway_ip: [u8; 4],
    pub gateway_mac: [u8; 6],
}

/// A running port 0 with a lease and a known gateway; owns the mempools.
pub struct Stack {
    pools: Vec<nic::PktPool>,
    rss: rss::Rss,
    netif: Netif,
    queues: u16,
}

impl Stack {
    /// Start port 0, read the RSS model, take a lease, resolve the gateway.
    pub fn up(cfg: &Config) -> Result<Stack, Error> {
        let want = cfg.queues.max(1);
        let queues = nic::clamp_queues(want);
        if queues != want {
            println!("clamping workers {} -> {} (device max)", want, queues);
        }

        let (pools, mac) = nic::probe_and_open(queues, cfg.rx_desc)?;
        let rss = rss::Rss::load(queues)?;
        let (ip, prefix_len, gateway_ip, gateway_mac) = dhcp::learn_network(pools[0].as_ptr(), mac)?;

        Ok(Stack {
            pools,
            rss,
            netif: Netif {
                mac,
                ip,
                prefix_len,
                gateway_ip,
                gateway_mac,
            },
            queues,
        })
    }

    pub fn queues(&self) -> u16 {
        self.queues
    }

    /// A `Send` ticket for one queue; `None` for one the device did not grant.
    pub fn handle(&self, queue_id: u16) -> Option<WorkerHandle> {
        if queue_id >= self.queues {
            return None;
        }
        Some(WorkerHandle {
            queue_id,
            pool: self.pools[queue_id as usize].as_ptr(),
            netif: self.netif,
            rss: self.rss,
        })
    }
}

/// Print, then stop: the console is all there is.
#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    println!("mininet: PANIC: {}", info.message());
    if let Some(loc) = info.location() {
        println!("mininet: at {}:{}", loc.file(), loc.line());
    }
    loop {
        core::hint::spin_loop()
    }
}

/// Named by the eh_frame rustc emits; there is no unwinding.
#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}
