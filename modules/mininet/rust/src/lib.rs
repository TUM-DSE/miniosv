/*!
mininet: a userspace network stack for miniOSv.

smoltcp for TCP/IP and rustls for TLS, driven directly over `minidpdk` through
the C++ shim in `modules/mininet/shim`. There is no socket layer, no VFS and no
libc networking underneath -- the stack polls the NIC queues itself, the way
`modules/miniext` drives NVMe itself.

The shape of the thing follows from one property of the hardware: which RX queue
a flow lands on is a pure function of its 4-tuple, and we control the source
port. [`Stack::up`] reads the device's Toeplitz key and indirection table, so a
[`Worker`] can pick source ports whose *return* traffic provably steers to the
queue it is polling. That makes each worker shared-nothing -- its own queue, its
own mempool, its own `Interface`, its own sockets -- with no locking on the data
path at all.

Usage is two phases. Once, at boot:

```ignore
let stack = Stack::up(&Config { queues: 8 })?;
```

which configures and starts port 0, acquires a DHCP lease, and ARPs the gateway.
Then one thread per queue, each pinned to its own CPU:

```ignore
let mut w = stack.worker(queue_id, &WorkerConfig::default())?;
w.connect(0, &Request { endpoint: &ep, head: &get, discard_ciphertext: false })?;
while !w.poll() {}
```

There is no resolver: an [`Endpoint`] carries the address the caller already
knows. See `PLAN_duckdb_net.md` for where that goes next.
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
mod selftest;
mod service;
pub mod stats;
pub mod thread;
mod tls;
mod worker;

pub use clock::MonoClock;
pub use conn::{Conn, Step};
pub use endpoint::{Endpoint, Request};
pub use error::Error;
pub use http::{BodySink, BufferSink, ContentRange, NullSink, ResponseHead};
pub use nic::{eth_qstats, eth_stats, NicStats};
pub use service::{GetResult, Service, ServiceConfig};
pub use worker::{Worker, WorkerConfig, WorkerHandle};

use alloc::vec::Vec;
use core::panic::PanicInfo;

/// What [`Stack::up`] asks the device for.
pub struct Config {
    /// RSS queues, and therefore workers, to request. Clamped to what the
    /// device advertises -- ENA caps io-queue count per instance size, and
    /// setting up a queue beyond the maximum is a hard reject, so asking for
    /// more than exists has to be caught here rather than at queue setup.
    pub queues: u16,
}

impl Default for Config {
    fn default() -> Self {
        Self { queues: 1 }
    }
}

/// The interface parameters every worker inherits. Learned once, on queue 0,
/// because DHCP and ARP replies are not steered by RSS and would land on an
/// arbitrary queue if each worker asked for itself.
#[derive(Clone, Copy)]
pub struct Netif {
    pub mac: [u8; 6],
    pub ip: [u8; 4],
    pub prefix_len: u8,
    pub gateway_ip: [u8; 4],
    pub gateway_mac: [u8; 6],
}

/// A configured and running port 0, with a lease and a known gateway.
///
/// Holds the per-queue mempools, so dropping it frees them -- which is why it
/// outlives every [`Worker`] by construction rather than by convention.
pub struct Stack {
    pools: Vec<nic::PktPool>,
    rss: rss::Rss,
    netif: Netif,
    queues: u16,
}

impl Stack {
    /// Configure and start port 0, read the RSS steering model off the device,
    /// then acquire a lease and resolve the gateway's MAC.
    ///
    /// Everything here happens on queue 0 before any worker exists.
    pub fn up(cfg: &Config) -> Result<Stack, Error> {
        let queues = nic::clamp_queues(cfg.queues);
        if queues != cfg.queues {
            println!("clamping workers {} -> {} (device max)", cfg.queues, queues);
        }

        let (pools, mac) = nic::probe_and_open(queues)?;
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

    /// Queues the device actually granted, which is the number of workers that
    /// can run.
    pub fn queues(&self) -> u16 {
        self.queues
    }

    pub fn netif(&self) -> Netif {
        self.netif
    }

    /// A `Send` ticket for one queue. Workers are built on the thread that will
    /// poll them -- the `Interface` and sockets must not migrate -- so the
    /// handle is what crosses the thread boundary, not the [`Worker`].
    ///
    /// Returns `None` for a queue the device did not grant.
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

    pub fn worker(&self, queue_id: u16, cfg: &WorkerConfig) -> Result<Worker, Error> {
        let handle = self.handle(queue_id).ok_or(Error::NoDevice)?;
        Worker::new(handle, cfg)
    }

    /// The pools go with `self`.
    pub fn down(self) {
        nic::dev_stop();
    }
}

/// OSv's `write(2)` is all the console there is; a panic message that never
/// reaches it leaves a hung VM and nothing to read. Print, then stop.
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

/// The kernel is built without unwinding, but the personality routine is still
/// named by the eh_frame rustc emits.
#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}
