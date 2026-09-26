//! One connection: a TCP socket, optionally a TLS session, one request, and
//! wherever its response body goes. Driven by [`Conn::step`], which never blocks.

use alloc::boxed::Box;
use alloc::sync::Arc;
use alloc::vec::Vec;

use rustls::client::{ClientConfig, UnbufferedClientConnection};
use rustls::pki_types::ServerName;
use rustls::unbuffered::{ConnectionState, UnbufferedStatus};
use smoltcp::iface::{SocketHandle, SocketSet};
use smoltcp::socket::tcp;

use crate::endpoint::Endpoint;
use crate::error::Error;
use crate::http::{BodySink, ResponseHead, ResponseParser};
use crate::stats;
use crate::tls::TLS_BUF_CAP;

const SYN_TIMEOUT_NS: u64 = 5_000_000_000;

/// The two record buffers, owned by the slot rather than allocated per dial.
pub(crate) struct ConnBufs {
    incoming: Vec<u8>,
    outgoing: Vec<u8>,
    head: Vec<u8>,
}

impl ConnBufs {
    pub(crate) fn new() -> Self {
        Self {
            incoming: Vec::with_capacity(TLS_BUF_CAP),
            outgoing: Vec::with_capacity(TLS_BUF_CAP),
            head: Vec::with_capacity(512),
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

pub struct Conn {
    handle: SocketHandle,
    /// `None` on a plain-HTTP connection: no session to drive.
    tls: Option<UnbufferedClientConnection>,
    incoming: Vec<u8>,
    outgoing: Vec<u8>,
    head: Vec<u8>,
    request_queued: bool,
    handshake_done: bool,
    parser: ResponseParser,
    sink: Box<dyn BodySink>,

    outcome: Option<Step>,
    connect_start_ns: u64,
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

    pub(crate) fn new(
        handle: SocketHandle,
        queue_id: u16,
        src_port: u16,
        tls: Option<UnbufferedClientConnection>,
        head: &[u8],
        sink: Box<dyn BodySink>,
        bufs: ConnBufs,
        now_ns: u64,
    ) -> Conn {
        Conn {
            handle,
            tls,
            incoming: bufs.incoming,
            outgoing: bufs.outgoing,
            head: {
                let mut h = bufs.head;
                h.clear();
                h.extend_from_slice(head);
                h
            },
            request_queued: false,
            handshake_done: false,
            parser: ResponseParser::new(),
            sink,
            outcome: None,
            connect_start_ns: now_ns,
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
        ConnBufs {
            incoming: self.incoming,
            outgoing: self.outgoing,
            head: self.head,
        }
    }

    /// Finished successfully and still Established, so reusable.
    pub(crate) fn idle_reusable(&self, sockets: &SocketSet<'_>) -> bool {
        self.outcome == Some(Step::Complete) && sockets.get::<tcp::Socket>(self.handle).state() == tcp::State::Established
    }

    /// Reuse the socket and TLS session for a new request. `incoming` is kept:
    /// it may hold a partial record of the same TLS stream.
    pub(crate) fn reset_for(&mut self, head: &[u8], sink: Box<dyn BodySink>, now_ns: u64) {
        debug_assert!(self.outgoing.is_empty(), "reuse with unsent request bytes");
        self.head.clear();
        self.head.extend_from_slice(head);
        self.request_queued = false;
        self.parser = ResponseParser::new();
        self.sink = sink;
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

    /// One non-blocking step: drain RX, push TX, advance TLS.
    pub(crate) fn step(&mut self, sockets: &mut SocketSet<'_>, now_ns: u64) -> Step {
        if let Some(done) = self.outcome {
            return done;
        }
        let s = sockets.get_mut::<tcp::Socket>(self.handle);
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
                "FAIL: q{} SYN timeout on port {} after {} ms — no SYN-ACK",
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

        if self.tls.is_none() && !self.request_queued && s.may_send() {
            self.outgoing.extend_from_slice(&self.head);
            self.request_queued = true;
            self.handshake_done = true;
        }
        if !self.outgoing.is_empty() && s.can_send() {
            if let Ok(n) = s.send_slice(&self.outgoing) {
                if n > 0 {
                    self.outgoing.drain(..n);
                }
            }
        }
        // recv() returns one contiguous slice of the ring, so loop.
        while s.can_recv() {
            match s.recv(|buf| {
                self.incoming.extend_from_slice(buf);
                (buf.len(), buf.len())
            }) {
                Ok(n) if n > 0 => continue,
                _ => break,
            }
        }

        if self.tls.is_none() && !self.incoming.is_empty() {
            let parsed = self.parser.feed(&self.incoming, self.sink.as_mut());
            self.incoming.clear();
            if let Err(e) = parsed {
                println!("FAIL: q{} port {} http: {:?}", self.queue_id, self.src_port, e);
                return self.finish(Step::Failed(Error::BadResponse));
            }
        }

        if let Some(step) = self.pump_tls() {
            return self.finish(step);
        }
        if self.head_ns.is_none() && self.parser.headers_done() {
            self.head_ns = Some(now_ns);
        }

        // Keep-alive: completion comes from the framing, not a FIN.
        if self.parser.headers_done() && self.outgoing.is_empty() {
            let done = if has_body(self.parser.status(), self.is_head) {
                self.parser
                    .head()
                    .content_length
                    .map_or(false, |want| self.parser.body_bytes() >= want)
            } else {
                true
            };
            if done {
                return self.finish(Step::Complete);
            }
        }

        // FIN fallback for responses the length check could not decide.
        let s = sockets.get_mut::<tcp::Socket>(self.handle);
        let ended = matches!(
            s.state(),
            tcp::State::Closed | tcp::State::CloseWait | tcp::State::TimeWait
        );
        if self.handshake_done && self.request_queued && ended && self.outgoing.is_empty() && !s.can_recv() {
            // Closed before any head arrived is a failure, not an empty body.
            if !self.parser.headers_done() {
                #[cfg(not(feature = "selftest"))]
                println!(
                    "FAIL: q{} port {} closed before answering",
                    self.queue_id, self.src_port
                );
                return self.finish(Step::Failed(Error::BadResponse));
            }
            return self.finish(Step::Complete);
        }
        Step::Pending
    }

    /// Advance the record layer until it stops making progress; `Some` is failure.
    fn pump_tls(&mut self) -> Option<Step> {
        let mut progress = self.tls.is_some();
        while progress {
            progress = false;
            let UnbufferedStatus { discard, state } = match self.tls.as_mut() {
                Some(tls) => tls.process_tls_records(&mut self.incoming),
                None => break,
            };
            let st = match state {
                Ok(st) => st,
                Err(e) => {
                    println!("FAIL: tls: {:?}", e);
                    return Some(Step::Failed(Error::Tls));
                }
            };
            match st {
                ConnectionState::ReadTraffic(mut rt) => {
                    while let Some(rec) = rt.next_record() {
                        match rec {
                            Ok(rec) => {
                                if let Err(e) = self.parser.feed(rec.payload, self.sink.as_mut()) {
                                    println!("FAIL: http: {:?}", e);
                                    return Some(Step::Failed(Error::BadResponse));
                                }
                            }
                            Err(e) => {
                                println!("FAIL: tls record: {:?}", e);
                                return Some(Step::Failed(Error::Tls));
                            }
                        }
                    }
                    progress = true;
                }
                ConnectionState::EncodeTlsData(mut et) => {
                    let head = self.outgoing.len();
                    self.outgoing.resize(TLS_BUF_CAP, 0);
                    match et.encode(&mut self.outgoing[head..]) {
                        Ok(n) => {
                            self.outgoing.truncate(head + n);
                            progress = true;
                        }
                        Err(e) => {
                            println!("FAIL: tls encode: {:?}", e);
                            return Some(Step::Failed(Error::Tls));
                        }
                    }
                }
                ConnectionState::TransmitTlsData(tt) => {
                    tt.done();
                    progress = true;
                }
                ConnectionState::WriteTraffic(mut wt) => {
                    self.handshake_done = true;
                    if !self.request_queued {
                        let head = self.outgoing.len();
                        self.outgoing.resize(head + self.head.len() + 128, 0);
                        match wt.encrypt(&self.head, &mut self.outgoing[head..]) {
                            Ok(n) => {
                                self.outgoing.truncate(head + n);
                                self.request_queued = true;
                                progress = true;
                            }
                            Err(e) => {
                                println!("FAIL: tls encrypt: {:?}", e);
                                return Some(Step::Failed(Error::Tls));
                            }
                        }
                    }
                }
                _ => {}
            }
            self.incoming.drain(..discard);
        }
        None
    }
}
