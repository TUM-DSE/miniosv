//! Unit tests for the pure-logic parts, driven by test/os-mininet.cc (make app=test):
//! no_std, no `cargo test` target, and the guest's real allocator is where a bounds bug shows.

use alloc::vec;
use alloc::vec::Vec;

use alloc::boxed::Box;

use crate::http::{BodySink, BufferSink, ContentRange, ParseError, ResponseParser};

/// Discards the body; the parser tests only look at the head.
struct NullSink;

impl BodySink for NullSink {
    fn write(&mut self, _data: &[u8]) {}
}

/// Accumulates failures so one run reports every broken case.
pub struct Report {
    pub failures: u32,
    pub checks: u32,
    verbose: bool,
}

impl Report {
    fn new(verbose: bool) -> Self {
        Self { failures: 0, checks: 0, verbose }
    }

    fn check(&mut self, ok: bool, what: &str) -> bool {
        self.checks += 1;
        if !ok {
            self.failures += 1;
            println!("  FAIL {}", what);
        } else if self.verbose {
            println!("  ok   {}", what);
        }
        ok
    }

    fn eq_u64(&mut self, got: u64, want: u64, what: &str) -> bool {
        let ok = got == want;
        self.checks += 1;
        if !ok {
            self.failures += 1;
            println!("  FAIL {}: got {} want {}", what, got, want);
        } else if self.verbose {
            println!("  ok   {}", what);
        }
        ok
    }
}

/// A sink over a heap buffer with guard bytes, so an out-of-bounds write is detected.
struct Guarded {
    mem: Vec<u8>,
    off: usize,
    cap: usize,
}

const GUARD: usize = 64;
const GUARD_BYTE: u8 = 0xA5;

impl Guarded {
    fn new(cap: usize) -> Self {
        let mem = vec![GUARD_BYTE; cap + 2 * GUARD];
        Self { mem, off: GUARD, cap }
    }

    fn sink(&mut self) -> BufferSink {
        unsafe { BufferSink::new(self.mem.as_mut_ptr().add(self.off), self.cap) }
    }

    /// True when neither guard band was touched.
    fn intact(&self) -> bool {
        self.mem[..self.off].iter().all(|&b| b == GUARD_BYTE)
            && self.mem[self.off + self.cap..].iter().all(|&b| b == GUARD_BYTE)
    }

    fn body(&self) -> &[u8] {
        &self.mem[self.off..self.off + self.cap]
    }
}

fn test_buffer_sink(r: &mut Report) {
    println!("-- BufferSink");

    // Exact fit: everything stored, nothing overflowed, guards untouched.
    {
        let mut g = Guarded::new(8);
        {
            let mut s = g.sink();
            s.write(b"abcdefgh");
            r.eq_u64(BodySink::written(&s), 8, "exact fit stores all 8");
            r.check(!BodySink::overflowed(&s), "exact fit does not flag overflow");
        }
        r.check(g.intact(), "exact fit leaves guard bands intact");
        r.check(g.body() == b"abcdefgh", "exact fit stores the right bytes");
    }

    // One byte too many: the excess is dropped, not written past the end.
    {
        let mut g = Guarded::new(4);
        {
            let mut s = g.sink();
            s.write(b"abcde");
            r.eq_u64(BodySink::written(&s), 4, "overlong write stores only cap");
            r.check(BodySink::overflowed(&s), "overlong write flags overflow");
        }
        r.check(g.intact(), "overlong write does not touch guard bands");
        r.check(g.body() == b"abcd", "overlong write keeps the first cap bytes");
    }

    // Many small writes that cross cap partway through one of them.
    {
        let mut g = Guarded::new(10);
        {
            let mut s = g.sink();
            for _ in 0..5 {
                s.write(b"xyz"); // 15 bytes offered into a 10-byte buffer
            }
            r.eq_u64(BodySink::written(&s), 10, "repeated writes stop at cap");
            r.check(BodySink::overflowed(&s), "repeated writes flag overflow");
        }
        r.check(g.intact(), "repeated writes do not touch guard bands");
    }

    // Writes after full must be no-ops: `cap - written` is unsigned.
    {
        let mut g = Guarded::new(3);
        {
            let mut s = g.sink();
            s.write(b"abc");
            s.write(b"defghijklmnop");
            s.write(b"q");
            r.eq_u64(BodySink::written(&s), 3, "writes past full stay at cap");
        }
        r.check(g.intact(), "writes past full do not touch guard bands");
    }

    // A zero-capacity sink (the HEAD path) must absorb writes harmlessly.
    {
        let mut g = Guarded::new(0);
        {
            let mut s = g.sink();
            s.write(b"anything");
            r.eq_u64(BodySink::written(&s), 0, "zero-cap sink stores nothing");
            r.check(BodySink::overflowed(&s), "zero-cap sink flags overflow");
        }
        r.check(g.intact(), "zero-cap sink does not touch guard bands");
    }

    // An empty write must not move the cursor or flag overflow.
    {
        let mut g = Guarded::new(4);
        let mut s = g.sink();
        s.write(b"");
        r.eq_u64(BodySink::written(&s), 0, "empty write stores nothing");
        r.check(!BodySink::overflowed(&s), "empty write does not flag overflow");
    }
}

/// Feed a whole response through the parser in one call.
fn parse_all(bytes: &[u8], sink: &mut dyn BodySink) -> (ResponseParser, Result<(), ParseError>) {
    let mut p = ResponseParser::new();
    let rc = p.feed(bytes, sink);
    (p, rc)
}

fn test_response_parser(r: &mut Report) {
    println!("-- ResponseParser");

    // The ordinary case: a 206 with a body in the same buffer as the head.
    {
        let mut g = Guarded::new(5);
        let mut s = g.sink();
        let (p, rc) = parse_all(
            b"HTTP/1.1 206 Partial Content\r\nContent-Length: 5\r\n\
              Content-Range: bytes 0-4/100\r\nETag: \"abc\"\r\n\r\nhello",
            &mut s,
        );
        r.check(rc.is_ok(), "206 parses");
        r.eq_u64(p.status() as u64, 206, "206 status");
        r.eq_u64(p.body_bytes(), 5, "206 body_bytes counts body only, not head");
        r.eq_u64(BodySink::written(&s), 5, "206 body reaches the sink");
        r.check(p.headers_done(), "206 marks headers done");
        r.check(p.head().content_length == Some(5), "206 content-length");
        r.check(
            p.head().content_range == Some(ContentRange { first: 0, last: 4, total: Some(100) }),
            "206 content-range",
        );
        r.check(p.head().etag.as_deref() == Some("\"abc\""), "206 etag");
    }

    // The CRLFCRLF terminator split across feeds.
    for split in 1..46usize {
        let msg = b"HTTP/1.1 206 Partial\r\nContent-Length: 3\r\n\r\nabc";
        if split >= msg.len() {
            break;
        }
        let mut g = Guarded::new(3);
        let mut s = g.sink();
        let mut p = ResponseParser::new();
        let a = p.feed(&msg[..split], &mut s);
        let b = p.feed(&msg[split..], &mut s);
        let ok = a.is_ok()
            && b.is_ok()
            && p.status() == 206
            && p.body_bytes() == 3
            && BodySink::written(&s) == 3
            && g.intact();
        if !r.check(ok, "head split across two feeds parses identically") && split < 4 {
            println!("    (split at {})", split);
        }
    }

    // A body delivered in several records must accumulate, not reset.
    {
        let mut g = Guarded::new(9);
        let mut s = g.sink();
        let mut p = ResponseParser::new();
        let _ = p.feed(b"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nabc", &mut s);
        let _ = p.feed(b"def", &mut s);
        let _ = p.feed(b"ghi", &mut s);
        r.eq_u64(p.body_bytes(), 9, "body across three feeds accumulates");
        r.check(g.body() == b"abcdefghi", "body across three feeds is in order");
        r.check(g.intact(), "multi-feed body does not touch guard bands");
    }

    // More body than promised must not write past the buffer.
    {
        let mut g = Guarded::new(4);
        let mut s = g.sink();
        let (p, _) = parse_all(
            b"HTTP/1.1 206 Partial\r\nContent-Length: 4\r\n\r\nAAAABBBBCCCC",
            &mut s,
        );
        r.eq_u64(p.body_bytes(), 12, "over-long body is counted in full");
        r.eq_u64(BodySink::written(&s), 4, "over-long body is stored only up to cap");
        r.check(BodySink::overflowed(&s), "over-long body flags overflow");
        r.check(g.intact(), "over-long body does not touch guard bands");
    }

    // Chunked is rejected rather than guessed at: guessing truncates silently.
    {
        let mut sink = NullSink;
        let (_, rc) = parse_all(
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
            &mut sink,
        );
        r.check(rc == Err(ParseError::Chunked), "chunked is rejected");
    }

    // Malformed status lines.
    for (bytes, what) in [
        (&b"nonsense\r\n\r\n"[..], "non-HTTP status line rejected"),
        (&b"HTTP/1.1 20 OK\r\n\r\n"[..], "two-digit status rejected"),
        (&b"HTTP/1.1 2O6 OK\r\n\r\n"[..], "non-digit status rejected"),
    ] {
        let mut sink = NullSink;
        let (_, rc) = parse_all(bytes, &mut sink);
        r.check(rc == Err(ParseError::BadStatusLine), what);
    }

    // A head that never terminates must be bounded, not buffered forever.
    {
        let mut sink = NullSink;
        let mut p = ResponseParser::new();
        let filler = vec![b'x'; 8 * 1024];
        let mut rc = Ok(());
        for _ in 0..16 {
            rc = p.feed(&filler, &mut sink);
            if rc.is_err() {
                break;
            }
        }
        r.check(rc == Err(ParseError::HeadTooLong), "endless head is bounded");
    }

    // Header parsing details that decide whether httpfs sees a range at all.
    {
        let mut sink = NullSink;
        let (p, rc) = parse_all(
            b"HTTP/1.1 206 Partial\r\ncOnTeNt-LeNgTh: 7\r\n\
              Content-Range: bytes 10-16/*\r\n\r\nabcdefg",
            &mut sink,
        );
        r.check(rc.is_ok(), "case-insensitive header names parse");
        r.check(p.head().content_length == Some(7), "mixed-case content-length read");
        r.check(
            p.head().content_range == Some(ContentRange { first: 10, last: 16, total: None }),
            "content-range with * total",
        );
    }

    // 204 and 304 carry no body; the head is still parsed.
    {
        let mut sink = NullSink;
        let (p, rc) = parse_all(b"HTTP/1.1 204 No Content\r\n\r\n", &mut sink);
        r.check(rc.is_ok(), "204 parses");
        r.eq_u64(p.body_bytes(), 0, "204 has no body");
        r.check(p.head().content_length.is_none(), "204 has no content-length");
    }
}

/// The arithmetic `Conn` uses to decide a response is finished.
fn test_completion_rule(r: &mut Report) {
    println!("-- completion rule");

    // Complete when body_bytes reaches content_length, and not before.
    let mut g = Guarded::new(6);
    let mut s = g.sink();
    let mut p = ResponseParser::new();
    let _ = p.feed(b"HTTP/1.1 206 Partial\r\nContent-Length: 6\r\n\r\nabc", &mut s);
    let want = p.head().content_length.unwrap_or(0);
    r.check(p.body_bytes() < want, "half a body is not complete");
    let _ = p.feed(b"def", &mut s);
    r.check(p.body_bytes() >= want, "a full body is complete");
    r.check(g.intact(), "completion path does not touch guard bands");
}

/// A connection that ends without a response is a failure, not an empty success.
fn test_close_without_response(r: &mut Report) {
    println!("-- close without a response");

    // Nothing at all arrived.
    {
        let mut p = ResponseParser::new();
        r.check(!p.headers_done(), "a parser fed nothing has no head");
        r.eq_u64(p.status() as u64, 0, "no head means status 0");
        r.eq_u64(p.body_bytes(), 0, "no head means no body");
        let mut sink = NullSink;
        let _ = p.feed(b"", &mut sink);
        r.check(!p.headers_done(), "an empty feed still has no head");
    }

    // A head cut off mid-way is not a head.
    {
        let mut sink = NullSink;
        let mut p = ResponseParser::new();
        let rc = p.feed(b"HTTP/1.1 206 Partial\r\nContent-Length: 5\r\n", &mut sink);
        r.check(rc.is_ok(), "a partial head parses so far without error");
        r.check(!p.headers_done(), "a partial head is not done");
        r.eq_u64(p.status() as u64, 0, "a partial head has no status yet");
    }

    // A 204 has a head, so a close after it is a real completion.
    {
        let mut sink = NullSink;
        let mut p = ResponseParser::new();
        let _ = p.feed(b"HTTP/1.1 204 No Content\r\n\r\n", &mut sink);
        r.check(p.headers_done(), "a bodyless response still finishes its head");
        r.eq_u64(p.status() as u64, 204, "204 status is readable");
    }
}

fn test_queue(r: &mut Report) {
    println!("-- request queue");
    let (got, dups, total) = crate::service::queue_selftest();
    r.eq_u64(got, total, "lock-free queue delivers every push");
    r.eq_u64(dups, 0, "lock-free queue delivers each push once");
    r.check(crate::service::queue_fifo_selftest(), "one producer's pushes pop in order");
}

/// Microsoft's RSS verification key.
const RSS_KEY: [u8; 40] = [
    0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
    0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
    0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
];
const PEER: [u8; 4] = [52, 95, 154, 1];
const US: [u8; 4] = [10, 0, 1, 7];

fn test_rss(r: &mut Report) {
    println!("-- RSS steering");
    use crate::rss::{Rss, EPH_BASE, EPH_LEN};

    // The vector the spec publishes: 66.9.149.187:2794 -> 161.142.100.80:1766.
    let mut tuple = [0u8; 12];
    tuple[0..4].copy_from_slice(&[66, 9, 149, 187]);
    tuple[4..8].copy_from_slice(&[161, 142, 100, 80]);
    tuple[8..10].copy_from_slice(&2794u16.to_be_bytes());
    tuple[10..12].copy_from_slice(&1766u16.to_be_bytes());
    r.eq_u64(crate::rss::toeplitz(&RSS_KEY, &tuple) as u64, 0x51cc_c178, "Toeplitz matches the verification vector");

    // Four queues over an eight-entry table: the queues partition the ephemeral range.
    let rss = Rss::synthetic(&RSS_KEY, &[0, 1, 2, 3, 0, 1, 2, 3]);
    let owned: Vec<_> = (0..4).map(|q| rss.owned_ports(PEER, 443, US, q)).collect();
    let total: u32 = owned.iter().map(|o| o.count()).sum();
    r.eq_u64(total as u64, EPH_LEN as u64, "the queues own every ephemeral port between them");
    let exclusive = (0..EPH_LEN)
        .map(|i| EPH_BASE + i as u16)
        .all(|port| owned.iter().filter(|o| o.contains(port)).count() == 1);
    r.check(exclusive, "each port steers to exactly one queue");
    r.check(owned.iter().all(|o| o.count() > EPH_LEN as u32 / 8), "the hash spreads the ports");
    r.check(owned[0].contains(68), "ports below the ephemeral range are never filtered");

    let picked = owned[1].spread(64);
    r.eq_u64(picked.len() as u64, 64, "spread returns what was asked");
    r.check(picked.windows(2).all(|w| w[0] < w[1]), "spread is ascending, so distinct");
    r.check(picked.iter().all(|&p| owned[1].contains(p)), "spread picks only owned ports");
    r.check(owned[1].spread(0).is_empty(), "spread(0) is empty");
    r.eq_u64(owned[1].spread(1 << 20).len() as u64, owned[1].count() as u64, "spread caps at the ports owned");

    let one = Rss::synthetic(&[], &[]);
    r.eq_u64(one.owned_ports(PEER, 443, US, 0).count() as u64, EPH_LEN as u64, "one queue owns everything");
}

fn test_arp(r: &mut Report) {
    println!("-- ARP frames");
    let (gw_mac, gw_ip, our_mac) = ([2, 0, 0, 0, 0, 1], [10, 0, 1, 1], [2, 0, 0, 0, 0, 2]);
    let reply = crate::arp::synthetic_reply(gw_mac, gw_ip, our_mac, US);
    r.eq_u64(reply.len() as u64, 42, "a synthetic reply is one minimal ARP frame");
    r.check(crate::arp::parse_reply_from(&reply, gw_ip) == Some(gw_mac), "the synthetic reply resolves the gateway");
    r.check(reply[0..6] == our_mac, "the synthetic reply is addressed to us");
    r.check(crate::arp::parse_reply_from(&reply, [10, 0, 1, 2]).is_none(), "a reply for another address is ignored");
    let req = crate::arp::request(our_mac, US, gw_ip);
    r.check(req[0..6] == [0xff; 6] && req[20..22] == [0, 1], "a request is a broadcast with opcode 1");
    r.check(crate::arp::parse_reply_from(&req, gw_ip).is_none(), "a request is not a reply");
    r.check(crate::arp::parse_reply_from(&reply[..41], gw_ip).is_none(), "a truncated frame is not a reply");
}

fn test_abi_helpers(r: &mut Report) {
    println!("-- C ABI helpers");
    use crate::capi::{parse_ipv4, set_cstr};
    r.check(parse_ipv4("3.5.216.240") == Some([3, 5, 216, 240]), "a dotted quad parses");
    r.check(parse_ipv4("0.0.0.0") == Some([0; 4]), "all zeros parse");
    for (s, what) in [
        ("256.1.1.1", "an octet over 255 is rejected"),
        ("1.2.3", "three octets are rejected"),
        ("1.2.3.4.5", "five octets are rejected"),
        ("1..2.3", "an empty octet is rejected"),
        ("a.b.c.d", "letters are rejected"),
        ("", "an empty address is rejected"),
    ] {
        r.check(parse_ipv4(s).is_none(), what);
    }

    let as_bytes = |d: &[core::ffi::c_char]| d.iter().map(|&c| c as u8).collect::<Vec<u8>>();
    let mut dst = [0x7f as core::ffi::c_char; 8];
    set_cstr(&mut dst, Some("abc"));
    r.check(as_bytes(&dst) == b"abc\0\0\0\0\0", "a short value is NUL-terminated and the rest cleared");
    set_cstr(&mut dst, Some("0123456789"));
    r.check(as_bytes(&dst) == b"0123456\0", "a long value is cut to fit, with its NUL");
    set_cstr(&mut dst, None);
    r.check(as_bytes(&dst) == [0; 8], "no value clears the field");
}

fn test_histogram(r: &mut Report) {
    println!("-- latency histogram");
    let d = crate::stats::dist_of(&[]);
    r.check(d.n == 0 && d.us_avg == 0 && d.us_p50 == 0 && d.us_max == 0, "an empty distribution is all zeros");

    let d = crate::stats::dist_of(&[1_000_000; 10]);
    r.eq_u64(d.n, 10, "ten samples are counted");
    r.eq_u64(d.us_avg, 1000, "the average of ten 1 ms samples");
    r.eq_u64(d.us_max, 1000, "the max of ten 1 ms samples");
    r.check(d.us_p50 >= 512 && d.us_p50 <= 1000, "p50 lands in the 1 ms bucket");

    let ms: Vec<u64> = (1..=100u64).map(|i| i * 1_000_000).collect();
    let d = crate::stats::dist_of(&ms);
    r.eq_u64(d.us_max, 100_000, "the max is exact");
    r.check(d.us_p50 <= d.us_p90 && d.us_p90 <= d.us_max, "percentiles are ordered");
    r.check(d.us_p50 >= 32_000 && d.us_p50 <= 64_000, "p50 of 1..100 ms sits in the 32-64 ms bucket");
    r.check(d.us_p90 >= 64_000, "p90 of 1..100 ms sits in the top bucket");
}

fn test_port_filter(r: &mut Report) {
    println!("-- inbound port filter");
    use crate::device::DpdkDevice;
    use crate::rss::{Rss, EPH_BASE};
    let rss = Rss::synthetic(&RSS_KEY, &[0, 1, 0, 1]);
    let owned = rss.owned_ports(PEER, 443, US, 1);
    let mine = owned.spread(1)[0];
    let other = (EPH_BASE..u16::MAX).find(|&p| !owned.contains(p)).unwrap_or(EPH_BASE);
    let dev = DpdkDevice::new(1, core::ptr::null_mut(), Some(owned), None);
    let frame = |proto: u8, dst_port: u16| -> [u8; 54] {
        let mut f = [0u8; 54];
        f[12..14].copy_from_slice(&[0x08, 0x00]);
        f[14] = 0x45;
        f[23] = proto;
        f[26..30].copy_from_slice(&PEER);
        f[30..34].copy_from_slice(&US);
        f[34..36].copy_from_slice(&443u16.to_be_bytes());
        f[36..38].copy_from_slice(&dst_port.to_be_bytes());
        f
    };
    let before = crate::stats::snapshot().misrouted_drops;
    r.check(dev.accepts(&frame(6, mine)), "TCP to an owned port is accepted");
    r.check(!dev.accepts(&frame(6, other)), "TCP to another queue's port is dropped");
    r.eq_u64(crate::stats::snapshot().misrouted_drops - before, 1, "the drop is counted");
    r.check(dev.accepts(&frame(17, other)), "UDP is not filtered");
    r.check(dev.accepts(&frame(6, 67)), "TCP below the ephemeral range is not filtered");
    let mut arp = frame(6, other);
    arp[12..14].copy_from_slice(&[0x08, 0x06]);
    r.check(dev.accepts(&arp), "non-IPv4 is not filtered");
    r.check(dev.accepts(&frame(6, other)[..40]), "a runt frame is left to the stack");
    let setup = DpdkDevice::new(0, core::ptr::null_mut(), None, None);
    r.check(setup.accepts(&frame(6, other)), "queue 0 in the setup phase accepts everything");
}

/// A client `Conn` and a scripted server on one smoltcp interface over its loopback device.
mod loopback {
    use alloc::boxed::Box;
    use alloc::vec::Vec;

    use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet};
    use smoltcp::phy::{Loopback, Medium};
    use smoltcp::socket::tcp;
    use smoltcp::time::Instant;
    use smoltcp::wire::{EthernetAddress, IpAddress, IpCidr, Ipv4Address};

    use crate::conn::{Conn, ConnBufs, Step};
    use crate::http::BodySink;

    #[derive(Clone, Copy, PartialEq, Eq)]
    pub(super) enum Server {
        /// Answer and keep the connection.
        Reply,
        /// Answer, then close: the body ends with the FIN.
        ReplyThenClose,
        /// Close without answering.
        Close,
        /// Never answer.
        Silent,
    }

    pub(super) struct Env {
        dev: Loopback,
        iface: Interface,
        sockets: SocketSet<'static>,
        server: SocketHandle,
        client: SocketHandle,
        pub(super) now_ms: i64,
    }

    impl Env {
        pub(super) fn new() -> Env {
            let mut dev = Loopback::new(Medium::Ethernet);
            let mut iface = Interface::new(
                Config::new(EthernetAddress([2, 0, 0, 0, 0, 1]).into()),
                &mut dev,
                Instant::from_millis(0),
            );
            iface.update_ip_addrs(|a| {
                let _ = a.push(IpCidr::new(IpAddress::v4(127, 0, 0, 1), 8));
            });
            let mut sockets = SocketSet::new(Vec::new());
            let mk = || {
                tcp::Socket::new(
                    tcp::SocketBuffer::new(alloc::vec![0; 65536]),
                    tcp::SocketBuffer::new(alloc::vec![0; 65536]),
                )
            };
            let server = sockets.add(mk());
            let client = sockets.add(mk());
            let _ = sockets.get_mut::<tcp::Socket>(server).listen(80);
            Env { dev, iface, sockets, server, client, now_ms: 0 }
        }

        pub(super) fn connect(&mut self, head: &[u8], sink: Box<dyn BodySink>) -> Conn {
            let dst = (Ipv4Address::new(127, 0, 0, 1), 80);
            let _ = self.sockets.get_mut::<tcp::Socket>(self.client).connect(self.iface.context(), dst, 49152);
            Conn::new(self.client, 0, 49152, None, head, sink, ConnBufs::new(), self.now_ns())
        }

        pub(super) fn now_ns(&self) -> u64 {
            self.now_ms as u64 * 1_000_000
        }

        /// One step of the connection at the current time, no packets moved.
        pub(super) fn step(&mut self, conn: &mut Conn) -> Step {
            let now = self.now_ns();
            conn.step(&mut self.sockets, now)
        }

        pub(super) fn reusable(&self, conn: &Conn) -> bool {
            conn.idle_reusable(&self.sockets)
        }

        /// Poll until the connection settles, or 2000 ms pass.
        pub(super) fn drive(&mut self, conn: &mut Conn, reply: &[u8], server: Server) -> Step {
            let mut got = Vec::new();
            let mut answered = false;
            for _ in 0..2000 {
                self.now_ms += 1;
                self.iface.poll(Instant::from_millis(self.now_ms), &mut self.dev, &mut self.sockets);
                let s = self.sockets.get_mut::<tcp::Socket>(self.server);
                if s.can_recv() {
                    let _ = s.recv(|b| {
                        got.extend_from_slice(b);
                        (b.len(), ())
                    });
                }
                if !answered && got.windows(4).any(|w| w == b"\r\n\r\n") {
                    answered = true;
                    match server {
                        Server::Silent => {}
                        Server::Close => s.close(),
                        Server::Reply | Server::ReplyThenClose => {
                            let _ = s.send_slice(reply);
                            if server == Server::ReplyThenClose {
                                s.close();
                            }
                        }
                    }
                }
                let now_ns = self.now_ns();
                let step = conn.step(&mut self.sockets, now_ns);
                if step != Step::Pending {
                    return step;
                }
            }
            Step::Pending
        }
    }
}

fn test_loopback_http(r: &mut Report) {
    println!("-- HTTP over a loopback connection");
    use crate::conn::Step;
    use crate::error::Error;
    use loopback::{Env, Server};
    const GET: &[u8] = b"GET /x HTTP/1.1\r\nHost: h\r\nRange: bytes=0-4\r\n\r\n";

    // A ranged GET, then a second request on the kept connection.
    {
        let mut env = Env::new();
        let mut g = Guarded::new(5);
        let mut conn = env.connect(GET, Box::new(g.sink()));
        let step = env.drive(
            &mut conn,
            b"HTTP/1.1 206 Partial Content\r\nContent-Length: 5\r\nContent-Range: bytes 0-4/100\r\nETag: \"e1\"\r\n\r\nhello",
            Server::Reply,
        );
        r.check(step == Step::Complete, "a ranged GET completes");
        r.eq_u64(conn.status() as u64, 206, "its status is read");
        r.eq_u64(conn.sink_written(), 5, "its body reaches the sink");
        r.check(g.body() == b"hello" && g.intact(), "the body is the bytes sent");
        r.check(conn.head().content_range.map(|c| c.total) == Some(Some(100)), "its content-range is read");
        r.check(conn.head().etag.as_deref() == Some("\"e1\""), "its etag is read");
        r.check(conn.head_ns().is_some(), "the first head byte is timed");
        r.check(env.reusable(&conn), "the connection is left open for reuse");

        let mut g2 = Guarded::new(5);
        conn.reset_for(GET, Box::new(g2.sink()), env.now_ns());
        let step = env.drive(&mut conn, b"HTTP/1.1 206 Partial Content\r\nContent-Length: 5\r\n\r\nworld", Server::Reply);
        r.check(step == Step::Complete, "a second request on the kept connection completes");
        r.check(g2.body() == b"world" && g.body() == b"hello", "each request fills its own sink");
        r.check(env.reusable(&conn), "the connection is still reusable");
    }

    // No Content-Length: the FIN ends the body.
    {
        let mut env = Env::new();
        let mut g = Guarded::new(6);
        let mut conn = env.connect(GET, Box::new(g.sink()));
        let step = env.drive(&mut conn, b"HTTP/1.1 200 OK\r\n\r\nabcdef", Server::ReplyThenClose);
        r.check(step == Step::Complete, "a body without a length completes on FIN");
        r.check(g.body() == b"abcdef", "the FIN-terminated body is complete");
        r.check(!env.reusable(&conn), "a closed connection is not reused");
    }

    // The server hangs up before answering.
    {
        let mut env = Env::new();
        let mut g = Guarded::new(4);
        let mut conn = env.connect(GET, Box::new(g.sink()));
        let step = env.drive(&mut conn, b"", Server::Close);
        r.check(step == Step::Failed(Error::BadResponse), "a close before any head is a failure");
    }

    // Nobody answers the SYN: the connection gives up on its own after the timeout.
    {
        let mut env = Env::new();
        let mut g = Guarded::new(4);
        let mut conn = env.connect(GET, Box::new(g.sink()));
        env.now_ms += 4_999;
        r.check(env.step(&mut conn) == Step::Pending, "a SYN still waits at 5 s");
        env.now_ms += 2;
        let step = env.step(&mut conn);
        r.check(step == Step::Failed(Error::SynTimeout), "past it, the SYN times out");
    }

    // HEAD: the head announces a length, no body follows, and that is complete.
    {
        let mut env = Env::new();
        let mut g = Guarded::new(0);
        let mut conn = env.connect(b"HEAD /x HTTP/1.1\r\nHost: h\r\n\r\n", Box::new(g.sink()));
        let step = env.drive(&mut conn, b"HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n", Server::Reply);
        r.check(step == Step::Complete, "HEAD completes without a body");
        r.check(conn.head().content_length == Some(1234), "HEAD reports the length");
        r.eq_u64(conn.sink_written(), 0, "HEAD writes nothing");
    }

    // 204 has no body by definition.
    {
        let mut env = Env::new();
        let mut g = Guarded::new(0);
        let mut conn = env.connect(GET, Box::new(g.sink()));
        let step = env.drive(&mut conn, b"HTTP/1.1 204 No Content\r\n\r\n", Server::Reply);
        r.check(step == Step::Complete, "204 completes");
    }

    // A body that never finishes is not mistaken for one that did.
    {
        let mut env = Env::new();
        let mut g = Guarded::new(8);
        let mut conn = env.connect(GET, Box::new(g.sink()));
        let step = env.drive(&mut conn, b"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nabc", Server::Reply);
        r.check(step == Step::Pending, "a short body stays pending");
        let step = env.drive(&mut conn, b"", Server::Silent);
        r.check(step == Step::Pending, "a silent server leaves it pending");
        r.check(g.body()[..3] == *b"abc", "what arrived is stored");
    }

    // A body larger than the buffer: the excess is dropped and the request fails.
    {
        let mut env = Env::new();
        let mut g = Guarded::new(2);
        let mut conn = env.connect(GET, Box::new(g.sink()));
        let step = env.drive(&mut conn, b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", Server::Reply);
        r.check(step == Step::Complete && conn.sink_overflowed(), "an oversize body completes with overflow flagged");
        r.check(g.body() == b"he" && g.intact(), "only the buffer's worth is stored");
    }
}

/// Runs every check. Returns the number that failed.
pub fn run(verbose: bool) -> u32 {
    let mut r = Report::new(verbose);
    test_buffer_sink(&mut r);
    test_response_parser(&mut r);
    test_completion_rule(&mut r);
    test_close_without_response(&mut r);
    test_rss(&mut r);
    test_arp(&mut r);
    test_abi_helpers(&mut r);
    test_histogram(&mut r);
    test_port_filter(&mut r);
    test_loopback_http(&mut r);
    test_queue(&mut r);
    println!(
        "mininet selftest: {} checks, {} failed",
        r.checks, r.failures
    );
    r.failures
}
