//! The C ABI behind `modules/mininet/mininet.hh`. One stack per image, so it
//! lives in a global.

use alloc::boxed::Box;
use core::ffi::{c_char, c_int, c_void, CStr};
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::endpoint::Endpoint;
use crate::error::Error;
use crate::service::{Service, ServiceConfig};
use crate::{Config, Stack};

/// Mirrored in mininet.hh.
const OK: c_int = 0;
const E_NO_DEVICE: c_int = -1;
const E_NO_MEMORY: c_int = -2;
const E_RSS: c_int = -3;
const E_DHCP: c_int = -4;
const E_ARP: c_int = -5;
const E_NO_PORTS: c_int = -6;
const E_CONNECT: c_int = -7;
const E_SYN_TIMEOUT: c_int = -8;
const E_TLS: c_int = -9;
const E_BAD_RESPONSE: c_int = -10;
const E_BUFFER_TOO_SMALL: c_int = -11;
const E_NOT_UP: c_int = -12;
const E_BAD_ARGUMENT: c_int = -13;

fn code(e: Error) -> c_int {
    match e {
        Error::NoDevice => E_NO_DEVICE,
        Error::NoMemory => E_NO_MEMORY,
        Error::RssUnavailable => E_RSS,
        Error::DhcpTimeout => E_DHCP,
        Error::ArpTimeout => E_ARP,
        Error::NoPorts => E_NO_PORTS,
        Error::ConnectRejected => E_CONNECT,
        Error::SynTimeout => E_SYN_TIMEOUT,
        Error::Tls => E_TLS,
        Error::BadResponse => E_BAD_RESPONSE,
        Error::BufferTooSmall => E_BUFFER_TOO_SMALL,
    }
}

struct Global {
    _stack: Stack,
    svc: Service,
    host: alloc::vec::Vec<u8>,
}

static GLOBAL: AtomicPtr<Global> = AtomicPtr::new(ptr::null_mut());

/// Mirrors `mininet::config`.
#[repr(C)]
pub struct mininet_config {
    pub host: *const c_char,
    pub address: *const c_char,
    pub tls: c_int,
    pub workers: u32,
    pub conns_per_worker: u32,
}

/// Mirrors `mininet::response`.
pub const ETAG_MAX: usize = 128;
pub const DATE_MAX: usize = 64;

#[repr(C)]
pub struct mininet_response {
    pub status: u32,
    pub content_length: u64,
    pub bytes: u64,
    pub has_range: u32,
    pub range_first: u64,
    pub range_last: u64,
    pub range_total: u64,
    pub etag: [c_char; ETAG_MAX],
    pub last_modified: [c_char; DATE_MAX],
}

pub(crate) fn set_cstr(dst: &mut [c_char], src: Option<&str>) {
    dst.fill(0);
    let src = match src {
        Some(s) => s.as_bytes(),
        None => return,
    };
    let n = core::cmp::min(src.len(), dst.len() - 1);
    for i in 0..n {
        dst[i] = src[i] as c_char;
    }
}

unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

pub(crate) fn parse_ipv4(s: &str) -> Option<[u8; 4]> {
    let mut out = [0u8; 4];
    let mut parts = 0;
    for (i, field) in s.split('.').enumerate() {
        if i >= 4 || field.is_empty() {
            return None;
        }
        let mut v: u32 = 0;
        for b in field.bytes() {
            if !b.is_ascii_digit() {
                return None;
            }
            v = v * 10 + (b - b'0') as u32;
            if v > 255 {
                return None;
            }
        }
        out[i] = v as u8;
        parts += 1;
    }
    if parts == 4 {
        Some(out)
    } else {
        None
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn mininet_up(cfg: *const mininet_config) -> c_int {
    if cfg.is_null() {
        return E_BAD_ARGUMENT;
    }
    if !GLOBAL.load(Ordering::Acquire).is_null() {
        return OK;
    }
    let cfg = unsafe { &*cfg };

    let host = match unsafe { cstr(cfg.host) } {
        Some(h) if !h.is_empty() => h,
        _ => return E_BAD_ARGUMENT,
    };
    let ip = match unsafe { cstr(cfg.address) }.and_then(parse_ipv4) {
        Some(ip) => ip,
        None => return E_BAD_ARGUMENT,
    };
    let peer = Endpoint::new(ip, host, cfg.tls != 0);

    let stack = match Stack::up(&Config {
        queues: cfg.workers.min(u16::MAX as u32) as u16,
        rx_desc: 0,
    }) {
        Ok(s) => s,
        Err(e) => return code(e),
    };

    let mut scfg = ServiceConfig::new(peer);
    scfg.conns_per_worker = cfg.conns_per_worker.max(1) as usize;
    let svc = match Service::start(&stack, &scfg) {
        Ok(s) => s,
        Err(e) => return code(e),
    };

    let mut host_z = alloc::vec::Vec::with_capacity(host.len() + 1);
    host_z.extend_from_slice(host.as_bytes());
    host_z.push(0);
    let global = Box::into_raw(Box::new(Global {
        _stack: stack,
        svc,
        host: host_z,
    }));
    match GLOBAL.compare_exchange(ptr::null_mut(), global, Ordering::AcqRel, Ordering::Acquire) {
        Ok(_) => OK,
        Err(_) => OK,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn mininet_host() -> *const c_char {
    let g = GLOBAL.load(Ordering::Acquire);
    if g.is_null() {
        return ptr::null();
    }
    unsafe { (*g).host.as_ptr() as *const c_char }
}

#[unsafe(no_mangle)]
pub extern "C" fn mininet_is_up() -> c_int {
    !GLOBAL.load(Ordering::Acquire).is_null() as c_int
}

/// Blocks the calling thread, parked rather than spinning.
#[unsafe(no_mangle)]
pub extern "C" fn mininet_get(
    head: *const c_char,
    head_len: u64,
    buf: *mut c_void,
    cap: u64,
    out: *mut mininet_response,
) -> c_int {
    let g = GLOBAL.load(Ordering::Acquire);
    if g.is_null() {
        return E_NOT_UP;
    }
    if head.is_null() || (buf.is_null() && cap != 0) {
        return E_BAD_ARGUMENT;
    }
    let g = unsafe { &*g };

    // A gigabyte-plus cap is a misparsed Range header, not a large read.
    const MAX_SANE_CAP: u64 = 1 << 30;
    if cap > MAX_SANE_CAP {
        println!("FAIL: mininet_get: implausible buffer cap {} bytes", cap);
        return E_BAD_ARGUMENT;
    }

    let head = unsafe { core::slice::from_raw_parts(head as *const u8, head_len as usize) };
    let body = if cap == 0 {
        &mut [][..]
    } else {
        unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, cap as usize) }
    };

    match g.svc.get(head, body) {
        Ok(r) => {
            if !out.is_null() {
                unsafe {
                    let o = &mut *out;
                    o.status = r.status as u32;
                    o.content_length = r.content_length.unwrap_or(0);
                    o.bytes = r.written;
                    match r.content_range {
                        Some(cr) => {
                            o.has_range = 1;
                            o.range_first = cr.first;
                            o.range_last = cr.last;
                            o.range_total = cr.total.unwrap_or(0);
                        }
                        None => {
                            o.has_range = 0;
                            o.range_first = 0;
                            o.range_last = 0;
                            o.range_total = 0;
                        }
                    }
                    set_cstr(&mut o.etag, r.etag.as_deref());
                    set_cstr(&mut o.last_modified, r.last_modified.as_deref());
                }
            }
            OK
        }
        Err(e) => {
            if !out.is_null() {
                unsafe {
                    let o = &mut *out;
                    o.status = 0;
                    o.content_length = 0;
                    o.bytes = 0;
                    o.has_range = 0;
                    set_cstr(&mut o.etag, None);
                    set_cstr(&mut o.last_modified, None);
                }
            }
            code(e)
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn mininet_conn_stats() -> crate::stats::Stats {
    crate::stats::snapshot()
}

/// Run the stack's pure-logic unit tests and return how many checks failed.
///
/// Needs no NIC and no network: it is the parser, the body sink and the
/// completion rule, exercised against buffers this function allocates itself.
/// Driven by `test/os-mininet.cc` under `make app=test`.
#[cfg(feature = "selftest")]
#[unsafe(no_mangle)]
pub extern "C" fn mininet_selftest(verbose: c_int) -> u32 {
    crate::selftest::run(verbose != 0)
}

/// Never null.
#[unsafe(no_mangle)]
pub extern "C" fn mininet_strerror(rc: c_int) -> *const c_char {
    let s: &str = match rc {
        OK => "ok\0",
        E_NO_DEVICE => "no usable NIC\0",
        E_NO_MEMORY => "out of memory\0",
        E_RSS => "RSS key or indirection table unavailable\0",
        E_DHCP => "DHCP timed out\0",
        E_ARP => "gateway ARP timed out\0",
        E_NO_PORTS => "no ephemeral port steers to this queue\0",
        E_CONNECT => "connect rejected\0",
        E_SYN_TIMEOUT => "SYN timeout: no SYN-ACK\0",
        E_TLS => "TLS failure\0",
        E_BAD_RESPONSE => "malformed or unsupported HTTP response\0",
        E_BUFFER_TOO_SMALL => "response body exceeded the buffer\0",
        E_NOT_UP => "mininet is not up\0",
        E_BAD_ARGUMENT => "bad argument\0",
        _ => "unknown error\0",
    };
    s.as_ptr() as *const c_char
}
