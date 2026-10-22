//! One connection: a TCP socket, optionally a TLS session, one request, and
//! wherever its response body goes. Driven by [`Conn::step`], which never blocks.
//!
//! Received payload stays in the frames it arrived in ([`crate::rx`]). The head is parsed here; the body goes to a
//! [`Feed`] for the requesting thread to copy, or decrypt, into its buffer, or with no feed is written here. TLS is
//! rustls until the handshake is over, then this module's own record layer ([`crate::record`]).

use alloc::boxed::Box;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::cell::Cell;

use ring::aead::LessSafeKey;
use rustls::client::{ClientConfig, ClientConnectionData, UnbufferedClientConnection};
use rustls::kernel::KernelConnection;
use rustls::pki_types::ServerName;
use rustls::unbuffered::{ConnectionState, UnbufferedStatus};
use smoltcp::iface::{SocketHandle, SocketSet};
use smoltcp::socket::tcp;

use crate::endpoint::Endpoint;
use crate::error::Error;
use crate::feed::{self, Feed, Piece};
use crate::http::{BodySink, ResponseHead, ResponseParser};
use crate::record::{self, Secret};
use crate::rx::{self, RxFrame, RxStream};
use crate::stats;
use crate::tls::TLS_BUF_CAP;

const SYN_TIMEOUT_NS: u64 = 5_000_000_000;
/// A peer gone quiet: probed every KEEP_ALIVE_S once idle, and the socket
/// aborts after PEER_TIMEOUT_S without a packet from it. S3 answers a probe
/// at once, so only a dead peer, or a path that lost its state, trips it.
pub(crate) const KEEP_ALIVE_S: u64 = 5;
pub(crate) const PEER_TIMEOUT_S: u64 = 30;
/// A TLS record at most: header, 2^14 of payload, the AEAD's expansion.
const RECORD_MAX: usize = 5 + 16384 + 256;
/// A TLS 1.3 record's payload beyond its content: the inner content type and the tag. Padding is not assumed.
const RECORD_OVERHEAD: usize = 1 + record::TAG;

/// A TLS record's length from its 5-byte header, once that much is there.
fn record_len(hdr: &[u8]) -> Option<usize> {
    (hdr.len() >= 5).then(|| 5 + usize::from(u16::from_be_bytes([hdr[3], hdr[4]])))
}

/// The slot's buffers, owned by it rather than allocated per dial. `incoming`
/// holds one TLS record the ring's wrap cut in two during the handshake, then
/// a record decrypted here.
pub(crate) struct ConnBufs {
    incoming: Vec<u8>,
    outgoing: Vec<u8>,
    head: Vec<u8>,
    rx: Box<RxStream>,
}

impl ConnBufs {
    pub(crate) fn new() -> Self {
        Self {
            incoming: Vec::with_capacity(RECORD_MAX),
            outgoing: Vec::with_capacity(TLS_BUF_CAP),
            head: Vec::with_capacity(512),
            rx: Box::new(RxStream::new(core::ptr::null())),
        }
    }
}

/// What one [`Conn::step`] concluded.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Step {
    Pending,
    Complete,
    Failed(Error),
}

/// The record layer after the handshake.
struct Records {
    rx: Secret,
    rx_key: LessSafeKey,
    rx_seq: u64,
    tx_key: LessSafeKey,
    tx_iv: [u8; 12],
    tx_seq: u64,
    kernel: KernelConnection<ClientConnectionData>,
}

pub struct Conn {
    handle: SocketHandle,
    /// rustls while handshaking; `None` on plain HTTP and once `records` took over.
    tls: Option<UnbufferedClientConnection>,
    records: Option<Records>,
    /// The handshake is over: hand the keys to `records` before anything else.
    convert: bool,
    incoming: Vec<u8>,
    outgoing: Vec<u8>,
    head: Vec<u8>,
    rx: Box<RxStream>,
    /// The socket hands payload to `rx` directly.
    direct: bool,
    /// The requesting thread reads the body from here; null: it is written here, through `sink`.
    feed: *const Feed,
    fed: bool,
    /// The record being handed to the feed: its payload still to come, length and sequence number.
    rec_left: usize,
    rec_len: usize,
    rec_seq: u64,
    rec_first: bool,
    peer_closed: bool,
    request_queued: bool,
    handshake_done: bool,
    parser: ResponseParser,
    sink: Box<dyn BodySink>,

    outcome: Option<Step>,
    connect_start_ns: u64,
    port: u16,
    queue_id: u16,
    src_port: u16,
    settled: bool,
    syn_ns: Option<u64>,
    head_ns: Option<u64>,
    is_head: bool,
}

fn is_head_request(head: &[u8]) -> bool {
    head.starts_with(b"HEAD ")
}

/// RFC 7230 3.3.3.
fn has_body(status: u16, is_head: bool) -> bool {
    !is_head && !matches!(status, 100..=199 | 204 | 304)
}

impl Conn {
    /// A fresh rustls session; `None` on a plain-HTTP peer.
    pub(crate) fn session(
        peer: &Endpoint,
        tls_config: &Arc<ClientConfig>,
    ) -> Result<Option<UnbufferedClientConnection>, Error> {
        if !peer.tls {
            return Ok(None);
        }
        let name = ServerName::try_from(peer.host.as_str())
            .map_err(|_| Error::Tls)?
            .to_owned();
        match UnbufferedClientConnection::new(tls_config.clone(), name) {
            Ok(c) => Ok(Some(c)),
            Err(e) => {
                println!("FAIL: rustls new: {:?}", e);
                Err(Error::Tls)
            }
        }
    }

    /// `frame`: the device's current frame (null without a NIC); `feed`: where the body goes (null: `sink`).
    pub(crate) fn new(
        handle: SocketHandle,
        port: u16,
        queue_id: u16,
        src_port: u16,
        tls: Option<UnbufferedClientConnection>,
        head: &[u8],
        sink: Box<dyn BodySink>,
        bufs: ConnBufs,
        now_ns: u64,
        frame: *const Cell<RxFrame>,
        feed: *const Feed,
    ) -> Conn {
        let mut rx = bufs.rx;
        *rx = RxStream::new(frame);
        Conn {
            handle,
            tls,
            records: None,
            convert: false,
            incoming: bufs.incoming,
            outgoing: bufs.outgoing,
            head: {
                let mut h = bufs.head;
                h.clear();
                h.extend_from_slice(head);
                h
            },
            rx,
            direct: false,
            feed,
            fed: false,
            rec_left: 0,
            rec_len: 0,
            rec_seq: 0,
            rec_first: false,
            peer_closed: false,
            request_queued: false,
            handshake_done: false,
            parser: ResponseParser::new(),
            sink,
            outcome: None,
            connect_start_ns: now_ns,
            port,
            queue_id,
            src_port,
            settled: false,
            syn_ns: None,
            head_ns: None,
            is_head: is_head_request(head),
        }
    }

    pub(crate) fn head_ns(&self) -> Option<u64> {
        self.head_ns
    }

    pub(crate) fn into_bufs(mut self) -> ConnBufs {
        self.incoming.clear();
        self.outgoing.clear();
        self.head.clear();
        self.rx.clear();
        ConnBufs {
            incoming: self.incoming,
            outgoing: self.outgoing,
            head: self.head,
            rx: self.rx,
        }
    }

    /// Finished successfully and still Established, so reusable.
    pub(crate) fn idle_reusable(&self, sockets: &SocketSet<'_>) -> bool {
        self.outcome == Some(Step::Complete) && sockets.get::<tcp::Socket>(self.handle).state() == tcp::State::Established
    }

    /// Reuse the socket and TLS session for a new request. `incoming` is kept:
    /// it may hold the start of a record the ring's wrap cut.
    pub(crate) fn reset_for(&mut self, head: &[u8], sink: Box<dyn BodySink>, now_ns: u64, feed: *const Feed) {
        debug_assert!(self.outgoing.is_empty(), "reuse with unsent request bytes");
        self.head.clear();
        self.head.extend_from_slice(head);
        self.request_queued = false;
        self.parser = ResponseParser::new();
        self.sink = sink;
        self.feed = feed;
        self.fed = false;
        self.outcome = None;
        self.connect_start_ns = now_ns;
        self.head_ns = None;
        self.is_head = is_head_request(&self.head);
    }

    pub fn sink_written(&self) -> u64 {
        self.sink.written()
    }

    pub fn sink_overflowed(&self) -> bool {
        self.sink.overflowed()
    }

    /// The HTTP status, or 0 until the head has been parsed.
    pub fn status(&self) -> u16 {
        self.parser.status()
    }

    pub fn head(&self) -> &ResponseHead {
        self.parser.head()
    }

    pub(crate) fn head_mut(&mut self) -> &mut ResponseHead {
        self.parser.head_mut()
    }

    pub fn outcome(&self) -> Option<Step> {
        self.outcome
    }

    /// The handshake has completed: a SYN still unanswered reads false.
    pub fn established(&self) -> bool {
        self.settled
    }

    fn finish(&mut self, step: Step) -> Step {
        self.outcome = Some(step);
        step
    }

    fn feed(&self) -> Option<&Feed> {
        unsafe { self.feed.as_ref() }
    }

    /// Hand the window back for what is done with: bytes used here, and pieces the reader is through with.
    fn release_window(&mut self, s: &mut tcp::Socket) {
        let mut n = core::mem::take(&mut self.rx.unheld);
        if let Some(f) = unsafe { self.feed.as_ref() } {
            n += f.reclaim();
        }
        if n > 0 {
            s.release_rx_held(n);
        }
    }

    /// The window back for whatever a request that ended early left behind.
    pub(crate) fn release_all(&mut self, sockets: &mut SocketSet<'_>) {
        self.rx.clear();
        let s = sockets.get_mut::<tcp::Socket>(self.handle);
        self.release_window(s);
    }

    /// Body bytes still to come, per the head; `None` until the head is in, or to the FIN.
    fn body_left(&self) -> Option<u64> {
        if !self.parser.headers_done() {
            return None;
        }
        if !has_body(self.parser.status(), self.is_head) {
            return Some(0);
        }
        self.parser.head().content_length.map(|want| want.saturating_sub(self.parser.body_bytes()))
    }

    /// One non-blocking step: drain RX, push TX, advance TLS.
    pub(crate) fn step(&mut self, sockets: &mut SocketSet<'_>, now_ns: u64) -> Step {
        let s = sockets.get_mut::<tcp::Socket>(self.handle);
        self.release_window(s);
        if let Some(done) = self.outcome {
            return done;
        }
        let state = s.state();

        if self.syn_ns.is_none()
            && matches!(state, tcp::State::SynSent | tcp::State::Established)
        {
            self.syn_ns = Some(now_ns);
        }
        let since_syn = now_ns.saturating_sub(self.syn_ns.unwrap_or(now_ns));

        if !self.settled && state == tcp::State::Established {
            self.settled = true;
            stats::conn_established(since_syn);
        }

        if state == tcp::State::SynSent
            && now_ns.saturating_sub(self.connect_start_ns) > SYN_TIMEOUT_NS
        {
            s.abort();
            #[cfg(not(feature = "selftest"))]
            println!(
                "FAIL: p{}q{} SYN timeout on port {} after {} ms — no SYN-ACK",
                self.port,
                self.queue_id,
                self.src_port,
                SYN_TIMEOUT_NS / 1_000_000
            );
            if !self.settled {
                self.settled = true;
                stats::conn_failed(since_syn);
            }
            return self.finish(Step::Failed(Error::SynTimeout));
        }

        // Closed under us -- smoltcp's timeout, or the peer's RST -- with the
        // request still open: nothing more will arrive on it.
        if state == tcp::State::Closed {
            if !self.settled {
                self.settled = true;
                stats::conn_failed(since_syn);
            }
            return self.finish(Step::Failed(Error::ConnectionLost));
        }

        if self.tls.is_none() && !self.direct {
            s.set_rx_direct(Some((rx::take_direct, &mut *self.rx as *mut RxStream as usize)));
            self.direct = true;
        }
        if self.tls.is_none() && !self.request_queued && s.may_send() {
            if let Err(step) = self.queue_request() {
                return self.finish(step);
            }
        }
        // Whatever the handshake left in the ring, then everything since, unless it bypassed the ring.
        while s.can_recv() {
            if self.tls.is_some() && !self.convert {
                let queued = s.recv_queue();
                match s.recv(|buf| match self.take(buf, buf.len() < queued) {
                    Ok(n) => (n, Ok(n)),
                    Err(step) => (0, Err(step)),
                }) {
                    Ok(Ok(n)) if n > 0 => {}
                    Ok(Err(step)) => return self.finish(step),
                    _ => break,
                }
            } else {
                let rx = &mut self.rx;
                let _ = s.recv(|buf| {
                    rx.push_copy(buf);
                    (buf.len(), ())
                });
            }
        }
        // With nothing new: the handshake's own records.
        if self.tls.is_some() && !self.convert {
            if let Err(step) = self.pump(&mut []) {
                return self.finish(step);
            }
        }
        if self.convert {
            if let Err(step) = self.into_records() {
                return self.finish(step);
            }
            let rx = &mut self.rx;
            while s.can_recv() {
                let _ = s.recv(|buf| {
                    rx.push_copy(buf);
                    (buf.len(), ())
                });
            }
            s.set_rx_direct(Some((rx::take_direct, &mut *self.rx as *mut RxStream as usize)));
            self.direct = true;
        }
        if self.records.is_some() && !self.request_queued {
            if let Err(step) = self.queue_request() {
                return self.finish(step);
            }
        }
        if !self.outgoing.is_empty() && s.can_send() {
            if let Ok(n) = s.send_slice(&self.outgoing) {
                if n > 0 {
                    self.outgoing.drain(..n);
                }
            }
        }

        let framed = if self.records.is_some() {
            self.frame_records()
        } else if self.tls.is_none() {
            self.frame_plain()
        } else {
            Ok(())
        };
        if let Err(step) = framed {
            return self.finish(step);
        }
        if self.head_ns.is_none() && self.parser.headers_done() {
            self.head_ns = Some(now_ns);
        }

        // Keep-alive: completion comes from the framing, not a FIN.
        if self.parser.headers_done() && self.outgoing.is_empty() && self.rec_left == 0 && self.body_left() == Some(0) {
            return self.finish(Step::Complete);
        }

        // FIN fallback for responses the length check could not decide.
        let s = sockets.get_mut::<tcp::Socket>(self.handle);
        let ended = self.peer_closed
            || matches!(
                s.state(),
                tcp::State::Closed | tcp::State::CloseWait | tcp::State::TimeWait
            );
        if self.handshake_done
            && self.request_queued
            && ended
            && self.outgoing.is_empty()
            && !s.can_recv()
            && self.rx.len() == 0
        {
            // Closed before any head arrived is a failure, not an empty body.
            // Not reported here: a peer that closed a connection we reused is
            // the ordinary end of a keep-alive, and service.rs re-dials once.
            // It happened ~1150 times a boot on the sf=100 suite and was
            // recovered every time, so a line here is 1150 lines of noise
            // hiding the failures that matter. service.rs reports the ones
            // that are still failures after the retry.
            // Short of the length the head promised is a failure too, not a
            // shorter body: the caller takes `bytes` as the object's.
            let short = self.body_left().map_or(false, |left| left > 0) || self.rec_left > 0;
            if !self.parser.headers_done() || short {
                return self.finish(Step::Failed(Error::BadResponse));
            }
            return self.finish(Step::Complete);
        }
        Step::Pending
    }

    /// The request onto `outgoing`: as it is, or as a record once the handshake is over.
    fn queue_request(&mut self) -> Result<(), Step> {
        match self.records.as_mut() {
            Some(r) => {
                if !record::seal(&r.tx_key, &r.tx_iv, r.tx_seq, &self.head, &mut self.outgoing) {
                    println!("FAIL: tls: request does not seal");
                    return Err(Step::Failed(Error::Tls));
                }
                r.tx_seq += 1;
            }
            None => self.outgoing.extend_from_slice(&self.head),
        }
        self.request_queued = true;
        self.handshake_done = true;
        Ok(())
    }

    /// rustls is done: its keys to this module's record layer, and what it had not read to `rx`.
    fn into_records(&mut self) -> Result<(), Step> {
        self.convert = false;
        let tls = self.tls.take().expect("converting a TLS session");
        let (secrets, kernel) = match tls.dangerous_into_kernel_connection() {
            Ok(k) => k,
            Err(e) => {
                println!("FAIL: tls: keys: {:?}", e);
                return Err(Step::Failed(Error::Tls));
            }
        };
        let (Some(rx), Some(tx)) = (Secret::from_rustls(&secrets.rx.1), Secret::from_rustls(&secrets.tx.1)) else {
            println!("FAIL: tls: not AES-GCM");
            return Err(Step::Failed(Error::Tls));
        };
        self.records = Some(Records {
            rx_key: rx.key(),
            rx,
            rx_seq: secrets.rx.0,
            tx_key: tx.key(),
            tx_iv: tx.iv,
            tx_seq: secrets.tx.0,
            kernel,
        });
        if !self.incoming.is_empty() {
            self.rx.push_copy(&self.incoming);
            self.incoming.clear();
        }
        Ok(())
    }

    /// Plain HTTP: the head parsed out of `rx`, the body handed on.
    fn frame_plain(&mut self) -> Result<(), Step> {
        while !self.parser.headers_done() {
            let Some(run) = self.rx.front() else { return Ok(()) };
            let n = run.len();
            match self.parser.feed_head(run) {
                Ok(Some(k)) => self.rx.skip(k),
                Ok(None) => self.rx.skip(n),
                Err(e) => {
                    println!("FAIL: p{}q{} port {} http: {:?}", self.port, self.queue_id, self.src_port, e);
                    return Err(Step::Failed(Error::BadResponse));
                }
            }
        }
        let mut left = self.body_left().unwrap_or(u64::MAX);
        while left > 0 {
            if self.feed().is_some_and(|f| !f.has_room()) {
                break;
            }
            let Some(run) = self.rx.cut(left.min(usize::MAX as u64) as usize) else { break };
            left -= run.len as u64;
            self.parser.count_body(run.len as u64);
            self.deliver(Piece { run, kind: feed::BODY, rec_len: 0, seq: 0 });
        }
        Ok(())
    }

    /// One piece of body to whoever writes it.
    fn deliver(&mut self, p: Piece) {
        match unsafe { self.feed.as_ref() } {
            Some(f) => {
                if !self.fed {
                    self.fed = true;
                    f.set_base(self.sink.written());
                    if let Some(r) = self.records.as_ref() {
                        f.set_secret(r.rx.clone());
                    }
                }
                f.push(p);
            }
            None => {
                self.sink.write(unsafe { core::slice::from_raw_parts(p.run.ptr, p.run.len) });
                if p.run.held {
                    self.rx.unheld += p.run.len;
                }
                p.run.owner.release();
            }
        }
    }

    /// TLS after the handshake: records up to the head decrypted here, the body's handed on whole.
    fn frame_records(&mut self) -> Result<(), Step> {
        loop {
            if self.rec_left > 0 {
                if self.feed().is_some_and(|f| !f.has_room()) {
                    return Ok(());
                }
                let Some(run) = self.rx.cut(self.rec_left) else { return Ok(()) };
                self.rec_left -= run.len;
                let kind = if self.rec_first { feed::REC_START } else { feed::REC_MORE };
                self.rec_first = false;
                self.deliver(Piece { run, kind, rec_len: self.rec_len as u32, seq: self.rec_seq });
                continue;
            }
            let mut hdr = [0u8; record::HEADER];
            if !self.rx.peek(&mut hdr) {
                return Ok(());
            }
            let len = record_len(&hdr).expect("a whole header") - record::HEADER;
            if hdr[0] != record::APPLICATION_DATA || len < RECORD_OVERHEAD || len > record::MAX_CIPHERTEXT {
                println!("FAIL: tls: record type {} length {}", hdr[0], len);
                return Err(Step::Failed(Error::Tls));
            }
            let to_reader = !self.feed.is_null() && self.body_left().is_some_and(|left| left > 0);
            let r = self.records.as_mut().expect("framing records");
            // The reader takes the body's records whole; counting assumes no padding, as servers send none.
            if to_reader {
                self.rx.skip(record::HEADER);
                self.rec_left = len;
                self.rec_len = len;
                self.rec_seq = r.rx_seq;
                self.rec_first = true;
                r.rx_seq += 1;
                self.parser.count_body((len - RECORD_OVERHEAD) as u64);
                continue;
            }
            if self.rx.len() < record::HEADER + len {
                return Ok(());
            }
            self.rx.skip(record::HEADER);
            self.incoming.resize(len, 0);
            self.rx.take_copy(&mut self.incoming[..len]);
            let seq = r.rx_seq;
            r.rx_seq += 1;
            let Some((content, ty)) = record::open(&r.rx_key, &r.rx.iv, seq, &mut self.incoming[..len]) else {
                println!("FAIL: tls: record {} does not open", seq);
                return Err(Step::Failed(Error::Tls));
            };
            match ty {
                record::APPLICATION_DATA => {
                    if let Err(e) = self.parser.feed(content, self.sink.as_mut()) {
                        println!("FAIL: http: {:?}", e);
                        return Err(Step::Failed(Error::BadResponse));
                    }
                }
                record::HANDSHAKE => Self::handshake_message(r, content)?,
                record::ALERT => {
                    // close_notify, or a fatal alert: either way nothing follows.
                    self.peer_closed = true;
                    self.incoming.clear();
                    return Ok(());
                }
                _ => {
                    println!("FAIL: tls: inner content type {}", ty);
                    return Err(Step::Failed(Error::Tls));
                }
            }
            self.incoming.clear();
        }
    }

    /// Post-handshake messages: tickets are not used, a key update rekeys what follows.
    fn handshake_message(r: &mut Records, mut msgs: &[u8]) -> Result<(), Step> {
        while msgs.len() >= 4 {
            let n = 4 + (usize::from(msgs[1]) << 16 | usize::from(msgs[2]) << 8 | usize::from(msgs[3]));
            if n > msgs.len() {
                break;
            }
            // KeyUpdate. One that asks for ours too goes unanswered: requests are small and few.
            if msgs[0] == 24 {
                match r.kernel.update_rx_secret().ok().and_then(|(seq, s)| Some((seq, Secret::from_rustls(&s)?))) {
                    Some((seq, s)) => {
                        r.rx_key = s.key();
                        r.rx = s;
                        r.rx_seq = seq;
                    }
                    None => {
                        println!("FAIL: tls: key update");
                        return Err(Step::Failed(Error::Tls));
                    }
                }
            }
            msgs = &msgs[n..];
        }
        Ok(())
    }

    /// The handshake's bytes in a run of the socket's ring, to rustls. Returns how many leading bytes are done
    /// with. `cut` says the run ends at the ring's wrap, so a record it cannot complete is staged in `incoming`
    /// and finished from the next run, rather than waited for.
    fn take(&mut self, buf: &mut [u8], cut: bool) -> Result<usize, Step> {
        if !self.incoming.is_empty() {
            let need = record_len(&self.incoming).unwrap_or(5).saturating_sub(self.incoming.len()).min(buf.len());
            self.incoming.extend_from_slice(&buf[..need]);
            if record_len(&self.incoming).map_or(true, |n| self.incoming.len() < n) {
                return Ok(need);
            }
            let mut staged = core::mem::take(&mut self.incoming);
            let r = self.pump(&mut staged);
            // What rustls left of it once the handshake was over is the record layer's.
            if let Ok(used) = r {
                if self.convert && used < staged.len() {
                    self.rx.push_copy(&staged[used..]);
                }
            }
            staged.clear();
            self.incoming = staged;
            return r.map(|_| need);
        }
        let used = self.pump(buf)?;
        if used == 0 && cut && !self.convert {
            self.incoming.extend_from_slice(buf);
            return Ok(buf.len());
        }
        Ok(used)
    }

    /// Advance the handshake over `buf` until it stops making progress, or is over;
    /// returns how many of `buf`'s leading bytes it consumed.
    fn pump(&mut self, buf: &mut [u8]) -> Result<usize, Step> {
        let mut used = 0;
        let mut progress = self.tls.is_some();
        while progress && !self.convert {
            progress = false;
            let UnbufferedStatus { discard, state } = match self.tls.as_mut() {
                Some(tls) => tls.process_tls_records(&mut buf[used..]),
                None => break,
            };
            let st = match state {
                Ok(st) => st,
                Err(e) => {
                    println!("FAIL: tls: {:?}", e);
                    return Err(Step::Failed(Error::Tls));
                }
            };
            match st {
                ConnectionState::ReadTraffic(mut rt) => {
                    // Before the handshake is over there is none; drained so rustls moves on.
                    while let Some(rec) = rt.next_record() {
                        if rec.is_err() {
                            println!("FAIL: tls record");
                            return Err(Step::Failed(Error::Tls));
                        }
                    }
                    progress = true;
                }
                ConnectionState::EncodeTlsData(mut et) => {
                    // Room for one record past what is queued; a plain
                    // resize to the cap would cut a longer queue short and
                    // then index past its end.
                    let head = self.outgoing.len();
                    self.outgoing.resize(core::cmp::max(TLS_BUF_CAP, head + RECORD_MAX), 0);
                    match et.encode(&mut self.outgoing[head..]) {
                        Ok(n) => {
                            self.outgoing.truncate(head + n);
                            progress = true;
                        }
                        Err(e) => {
                            println!("FAIL: tls encode: {:?}", e);
                            return Err(Step::Failed(Error::Tls));
                        }
                    }
                }
                ConnectionState::TransmitTlsData(tt) => {
                    tt.done();
                    progress = true;
                }
                ConnectionState::WriteTraffic(_) => {
                    self.handshake_done = true;
                    self.convert = true;
                }
                _ => {}
            }
            used += discard;
        }
        Ok(used)
    }
}
