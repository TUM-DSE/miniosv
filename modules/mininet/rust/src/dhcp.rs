//! Learning the interface configuration: DHCP for the address and route, DNS
//! for the peer if asked, then ARP for the gateway's MAC; before any worker exists.

use core::ffi::c_void;
use core::ptr;

use smoltcp::iface::{Config, Interface, SocketSet, SocketStorage};
use smoltcp::socket::dhcpv4;
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, IpCidr, Ipv4Address};

use crate::arp;
use crate::clock::MonoClock;
use crate::device::DpdkDevice;
use crate::dns;
use crate::error::Error;
use crate::ffi::{shim_mbuf_alloc_tx, shim_mbuf_free, shim_mbuf_rx_burst_n, shim_mbuf_tx_burst};
use crate::nic::PktPool;
use crate::Netif;

/// Wall-clock limits on the two things boot waits for. DHCP on EC2 answers
/// within a second; a fresh ENI's first ARP can take a few.
const DHCP_TIMEOUT_MS: i64 = 30_000;
const ARP_TIMEOUT_MS: i64 = 10_000;

fn acquire(
    iface: &mut Interface,
    dev: &mut DpdkDevice,
    sockets: &mut SocketSet<'_>,
    handle: smoltcp::iface::SocketHandle,
    clk: &MonoClock,
) -> Result<(smoltcp::wire::Ipv4Cidr, Ipv4Address, [u8; 4]), Error> {
    println!("DHCP: requesting lease...");
    loop {
        let now_ms = clk.elapsed_ms();
        iface.poll(Instant::from_millis(now_ms), dev, sockets);

        match sockets.get_mut::<dhcpv4::Socket>(handle).poll() {
            Some(dhcpv4::Event::Configured(cfg)) => {
                let a = cfg.address;
                let o = a.address().octets();
                println!(
                    "DHCP: address {}.{}.{}.{}/{}",
                    o[0],
                    o[1],
                    o[2],
                    o[3],
                    a.prefix_len()
                );
                let router = cfg.router.unwrap_or(Ipv4Address::new(0, 0, 0, 0));
                let r = router.octets();
                println!("DHCP: gateway {}.{}.{}.{}", r[0], r[1], r[2], r[3]);
                iface.update_ip_addrs(|addrs| {
                    let _ = addrs.push(IpCidr::Ipv4(a));
                });
                if let Some(gw) = cfg.router {
                    let _ = iface.routes_mut().add_default_ipv4_route(gw);
                }
                let dns = cfg.dns_servers.first().map(|d| d.octets()).unwrap_or([0; 4]);
                println!("DHCP: resolver {}.{}.{}.{}", dns[0], dns[1], dns[2], dns[3]);
                return Ok((a, router, dns));
            }
            Some(dhcpv4::Event::Deconfigured) => println!("DHCP: deconfigured"),
            None => {}
        }
        if now_ms > DHCP_TIMEOUT_MS {
            println!("DHCP: timeout after {} ms", now_ms);
            return Err(Error::DhcpTimeout);
        }
    }
}

/// DHCP, DNS if asked, then one raw ARP for the next hop: the gateway, or an
/// on-link peer in its place. Every port has its own lease -- one IP per ENI.
pub(crate) fn learn_network(
    port: u16,
    pools: &[PktPool],
    mac: [u8; 6],
    peer: Option<[u8; 4]>,
    resolve: Option<&str>,
) -> Result<Netif, Error> {
    let clk = MonoClock::new();
    let pool = pools[0].as_ptr();
    let mut netif = Netif {
        mac,
        ip: [0; 4],
        prefix_len: 0,
        gateway_ip: [0; 4],
        gateway_mac: [0; 6],
        dns: [0; 4],
    };

    // Scoped so the devices' &mut are released before the raw ARP below.
    {
        // Nothing here is steered: accept everything.
        let mut devs: alloc::vec::Vec<DpdkDevice> = (0..pools.len() as u16)
            .map(|q| DpdkDevice::new(port, q, pools[q as usize].as_ptr(), None, None))
            .collect();
        let config = Config::new(EthernetAddress(mac).into());
        let mut iface = Interface::new(config, &mut devs[0], Instant::from_millis(clk.elapsed_ms()));

        let mut storage = [SocketStorage::EMPTY; 1];
        let mut sockets = SocketSet::new(&mut storage[..]);
        let handle = sockets.add(dhcpv4::Socket::new());
        let (cidr, gw, resolver) = acquire(&mut iface, &mut devs[0], &mut sockets, handle, &clk)?;
        if let Some(host) = resolve {
            if resolver == [0; 4] {
                println!("FAIL: DHCP offered no resolver to ask for {}", host);
                return Err(Error::Dns);
            }
            dns::resolve(&mut iface, &mut devs, resolver, host, pools.len(), &clk)?;
        }
        netif.dns = resolver;
        let hop = match peer.map(Ipv4Address::from_octets) {
            Some(p) if cidr.contains_addr(&p) => p,
            _ => gw,
        };
        netif.ip = cidr.address().octets();
        netif.prefix_len = cidr.prefix_len();
        netif.gateway_ip = hop.octets();
    }

    // Raw ARP, bypassing smoltcp, so the one reply can seed every worker.
    let req = arp::request(mac, netif.ip, netif.gateway_ip);
    unsafe {
        let mut handle: *mut c_void = ptr::null_mut();
        let mut cap: u16 = 0;
        let data = shim_mbuf_alloc_tx(pool, 0, &mut handle, &mut cap);
        if data.is_null() || handle.is_null() {
            println!("FAIL: no mbuf for ARP");
            return Err(Error::NoMemory);
        }
        let n = core::cmp::min(req.len(), cap as usize);
        core::ptr::copy_nonoverlapping(req.as_ptr(), data, n);
        let _ = shim_mbuf_tx_burst(port, 0, &mut handle, &(n as u16), 1);
    }

    let arp_start_ms = clk.elapsed_ms();
    loop {
        let mut handle = [ptr::null_mut::<c_void>(); 1];
        let mut data = [ptr::null::<u8>(); 1];
        let mut len = [0u16; 1];
        let got = unsafe {
            shim_mbuf_rx_burst_n(port, 0, handle.as_mut_ptr(), data.as_mut_ptr(), len.as_mut_ptr(), 1)
        };
        if got == 1 {
            let slice = unsafe { core::slice::from_raw_parts(data[0], len[0] as usize) };
            let hw = arp::parse_reply_from(slice, netif.gateway_ip);
            unsafe { shim_mbuf_free(handle[0]) };
            if let Some(hw) = hw {
                println!(
                    "gateway MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]
                );
                netif.gateway_mac = hw;
                return Ok(netif);
            }
        }
        if clk.elapsed_ms() - arp_start_ms > ARP_TIMEOUT_MS {
            println!("FAIL: gateway ARP timed out after {} ms", ARP_TIMEOUT_MS);
            return Err(Error::ArpTimeout);
        }
    }
}
