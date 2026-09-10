//! Learning the interface configuration: DHCP for the address and route, then
//! ARP for the gateway's MAC. Both run on queue 0 before any worker exists.

use core::ffi::c_void;
use core::ptr;

use smoltcp::iface::{Config, Interface, SocketSet, SocketStorage};
use smoltcp::socket::dhcpv4;
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, IpCidr, Ipv4Address};

use crate::arp;
use crate::clock::{MonoClock, ITER_BUDGET};
use crate::device::DpdkDevice;
use crate::error::Error;
use crate::ffi::{rte_pktmbuf_pool, shim_mbuf_alloc_tx, shim_mbuf_free, shim_mbuf_rx_burst_n, shim_mbuf_tx};

fn acquire(
    iface: &mut Interface,
    dev: &mut DpdkDevice,
    sockets: &mut SocketSet<'_>,
    handle: smoltcp::iface::SocketHandle,
    clk: &MonoClock,
) -> Result<(smoltcp::wire::Ipv4Cidr, Ipv4Address), Error> {
    println!("DHCP: requesting lease...");
    let mut iter: u64 = 0;
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
                return Ok((a, router));
            }
            Some(dhcpv4::Event::Deconfigured) => println!("DHCP: deconfigured"),
            None => {}
        }
        iter = iter.wrapping_add(1);
        if iter > ITER_BUDGET / 10 {
            println!("DHCP: timeout after {} ms", now_ms);
            return Err(Error::DhcpTimeout);
        }
    }
}

/// DHCP plus gateway ARP on queue 0.
pub(crate) fn learn_network(
    pool: *mut rte_pktmbuf_pool,
    mac: [u8; 6],
) -> Result<([u8; 4], u8, [u8; 4], [u8; 6]), Error> {
    let clk = MonoClock::new();

    // Scoped so the device's &mut is released before the raw ARP below.
    let (ip, prefix, gw) = {
        // The DHCP and ARP path accepts every packet: nothing here is steered.
        let mut dev = DpdkDevice::new(0, pool, None, None);
        let config = Config::new(EthernetAddress(mac).into());
        let mut iface = Interface::new(config, &mut dev, Instant::from_millis(clk.elapsed_ms()));

        let mut storage = [SocketStorage::EMPTY; 1];
        let mut sockets = SocketSet::new(&mut storage[..]);
        let handle = sockets.add(dhcpv4::Socket::new());
        let (cidr, gw) = acquire(&mut iface, &mut dev, &mut sockets, handle, &clk)?;
        (cidr.address(), cidr.prefix_len(), gw)
    };

    // Raw ARP, bypassing smoltcp, so the one reply can seed every worker.
    let req = arp::request(mac, ip.octets(), gw.octets());
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
        let _ = shim_mbuf_tx(0, 0, handle, n as u16);
    }

    let mut iter: u64 = 0;
    loop {
        let mut handle = [ptr::null_mut::<c_void>(); 1];
        let mut data = [ptr::null::<u8>(); 1];
        let mut len = [0u16; 1];
        let got = unsafe {
            shim_mbuf_rx_burst_n(0, 0, handle.as_mut_ptr(), data.as_mut_ptr(), len.as_mut_ptr(), 1)
        };
        if got == 1 {
            let slice = unsafe { core::slice::from_raw_parts(data[0], len[0] as usize) };
            let hw = arp::parse_reply_from(slice, gw.octets());
            unsafe { shim_mbuf_free(handle[0]) };
            if let Some(hw) = hw {
                println!(
                    "gateway MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]
                );
                return Ok((ip.octets(), prefix, gw.octets(), hw));
            }
        }
        iter = iter.wrapping_add(1);
        if iter > ITER_BUDGET / 20 {
            println!("FAIL: gateway ARP timed out");
            return Err(Error::ArpTimeout);
        }
    }
}
