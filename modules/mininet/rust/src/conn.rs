//! One connection: a TCP socket, optionally a TLS session, one request, and
//! wherever its response body goes.
//!
//! Driven by [`Conn::step`], which the owning [`crate::Worker`] calls once per
//! poll for each of its connections. Nothing here blocks or waits -- a step
//! moves what it can and returns.

use alloc::boxed::Box;
use alloc::sync::Arc;
use alloc::vec::Vec;

use rustls::client::{ClientConfig, UnbufferedClientConnection};
use rustls::pki_types::ServerName;
use rustls::unbuffered::{ConnectionState, UnbufferedStatus};
use smoltcp::iface::{SocketHandle, SocketSet};
use smoltcp::socket::tcp;

use crate::clock::MonoClock;
use crate::endpoint::{Endpoint, Request};
use crate::error::Error;
use crate::http::{BodySink, NullSink, ResponseHead, ResponseParser};
use crate::stats;
use crate::tls::TLS_BUF_CAP;

/// A SYN now only goes unanswered on genuine loss -- the source port is chosen
/// so the SYN-ACK provably returns on this queue -- so the timeout is a
/// backstop rather than a polling interval.
const SYN_TIMEOUT_MS: i64 = 5_000;

/// What one [`Conn::step`] concluded.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Step {
    Pending,
    /// The peer closed after the whole response had been requested and the
    /// receive buffer had been drained.
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
    /// With TLS, set when the session first reaches `WriteTraffic`. Without
    /// it there is no handshake to wait on, so it means "the request may go
    /// out" -- which is what the clean-close check below reads it as.
    handshake_done: bool,
    discard_ciphertext: bool,
    parser: ResponseParser,
    sink: Box<dyn BodySink>,

    outcome: Option<Step>,
    connect_start_ms: i64,
    queue_id: u16,
    src_port: u16,
    /// Total SYNs sent, and whether this connection has left SynSent yet --
    /// either established or given up.
    attempts: u16,
    settled: bool,
    start_ms: i64,
    /// HEAD (and, defensively, 1xx/204/304) responses have no body regardless
    /// of what Content-Length says -- needed now that completion can't just
    /// wait for the peer to close.
    is_head: bool,
}

fn is_head_request(head: &[u8]) -> bool {
    head.starts_with(b"HEAD ")
}

/// Whether a response with this status ever carries a body, per RFC 7230
/// 3.3.3. Under keep-alive there is no FIN to fall back on, so this has to be
/// right rather than assumed.
fn has_body(status: u16, is_head: bool) -> bool {
    !is_head && !matches!(status, 100..=199 | 204 | 304)
}

impl Conn {
    pub(crate) fn new(
        handle: SocketHandle,
        queue_id: u16,
        src_port: u16,
        peer: &Endpoint,
        req: &Request<'_>,
        tls_config: &Arc<ClientConfig>,
        now_ms: i64,
    ) -> Result<Conn, Error> {
        let tls = if peer.tls {
            let name = ServerName::try_from(peer.host.as_str())
                .map_err(|_| Error::Tls)?
                .to_owned();
            match UnbufferedClientConnection::new(tls_config.clone(), name) {
                Ok(c) => Some(c),
                Err(e) => {
                    println!("FAIL: rustls new: {:?}", e);
                    return Err(Error::Tls);
                }
            }
        } else {
            None
        };

        Ok(Conn {
            handle,
            tls,
            incoming: Vec::with_capacity(TLS_BUF_CAP),
            outgoing: Vec::with_capacity(TLS_BUF_CAP),
            head: req.head.to_vec(),
            request_queued: false,
            handshake_done: false,
            // Meaningless without a record layer to skip.
            discard_ciphertext: req.discard_ciphertext && peer.tls,
            parser: ResponseParser::new(),
            sink: Box::new(NullSink),
            outcome: None,
            connect_start_ms: now_ms,
            queue_id,
            src_port,
            attempts: 1,
            settled: false,
            start_ms: now_ms,
            is_head: is_head_request(req.head),
        })
    }

    /// Whether this connection finished successfully and the peer hasn't
    /// closed it -- the socket is still Established, so the next request can
    /// reuse it instead of paying for a fresh handshake (and, over TLS, a
    /// fresh key exchange).
    pub(crate) fn idle_reusable(&self, sockets: &SocketSet<'_>) -> bool {
        self.outcome == Some(Step::Complete) && sockets.get::<tcp::Socket>(self.handle).state() == tcp::State::Established
    }

    /// Reuse this connection's socket (and, over TLS, its session) for a new
    /// request. Only valid when [`Conn::idle_reusable`] was just true; the TCP
    /// and TLS handshakes are not repeated.
    pub(crate) fn reset_for(&mut self, req: &Request<'_>, now_ms: i64) {
        // `incoming` is deliberately *not* cleared. A reused connection carries
        // on the same TLS session, so anything still buffered is a partial
        // record of that stream -- a server-sent ticket, or a record that
        // straddled the moment the body completed. Dropping it would leave
        // rustls decrypting from the middle of a record, which fails much
        // later and nowhere near here. Completion already requires `outgoing`
        // to be empty, so there is nothing pending to send either; clearing it
        // would only be able to discard a partly-written record.
        debug_assert!(self.outgoing.is_empty(), "reuse with unsent request bytes");
        self.head = req.head.to_vec();
        self.request_queued = false;
        self.discard_ciphertext = req.discard_ciphertext && self.tls.is_some();
        self.parser = ResponseParser::new();
        self.sink = Box::new(NullSink);
        self.outcome = None;
        self.connect_start_ms = now_ms;
        self.is_head = is_head_request(&self.head);
    }

    pub fn set_sink(&mut self, sink: Box<dyn BodySink>) {
        self.sink = sink;
    }

    /// Bytes the sink stored, which is not the same as [`Conn::body_bytes`]:
    /// the body may be larger than the sink had room for.
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

    /// False before the first bytes arrive, and for the whole transfer when
    /// the record layer was stubbed out -- there is no plaintext head to
    /// parse in that case.
    pub fn headers_parsed(&self) -> bool {
        self.parser.headers_done()
    }

    pub fn head(&self) -> &ResponseHead {
        self.parser.head()
    }

    /// Response body bytes. Ciphertext, including framing, when the record
    /// layer was stubbed out.
    pub fn body_bytes(&self) -> u64 {
        self.parser.body_bytes()
    }

    pub fn src_port(&self) -> u16 {
        self.src_port
    }

    pub fn outcome(&self) -> Option<Step> {
        self.outcome
    }

    pub fn is_complete(&self) -> bool {
        self.outcome == Some(Step::Complete)
    }

    fn finish(&mut self, step: Step) -> Step {
        self.outcome = Some(step);
        step
    }

    /// Move this connection forward once: drain RX, push TX, advance the TLS
    /// state machine.
    pub(crate) fn step(&mut self, sockets: &mut SocketSet<'_>, clk: &MonoClock) -> Step {
        if let Some(done) = self.outcome {
            return done;
        }
        let now_ms = clk.elapsed_ms();
        let s = sockets.get_mut::<tcp::Socket>(self.handle);
        let state = s.state();

        // Leaving SynSent for Established means the SYN-ACK came back on the
        // right queue; record how many SYNs it took and how long it burned.
        if !self.settled && state == tcp::State::Established {
            self.settled = true;
            stats::conn_established(self.attempts, now_ms - self.start_ms);
        }

        // A SYN that goes unanswered now is a real failure -- lost packet or
        // unreachable peer, not a wrong port hash -- so fail loudly rather
        // than abandon it silently.
        if state == tcp::State::SynSent && now_ms - self.connect_start_ms > SYN_TIMEOUT_MS {
            s.abort();
            println!(
                "FAIL: q{} SYN timeout on port {} after {} ms — no SYN-ACK",
                self.queue_id, self.src_port, SYN_TIMEOUT_MS
            );
            if !self.settled {
                self.settled = true;
                stats::conn_failed(now_ms - self.start_ms);
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
        // Drain until the socket is empty, not once: smoltcp hands back the
        // largest *contiguous* slice of its ring, so a single call leaves the
        // remainder behind whenever the data has wrapped.
        while s.can_recv() {
            match s.recv(|buf| {
                self.incoming.extend_from_slice(buf);
                (buf.len(), buf.len())
            }) {
                Ok(n) if n > 0 => continue,
                _ => break,
            }
        }

        if self.discard_ciphertext && self.handshake_done && self.request_queued {
            self.parser.count_opaque(self.incoming.len());
            self.incoming.clear();
        }

        // Without a record layer the socket already holds response bytes.
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

        // Keep-alive: the peer does not close after one response, so
        // completion has to come from the framing rather than a FIN.
        // `discard_ciphertext` never reaches this -- it bypasses the parser
        // via count_opaque above -- and falls through to the FIN check below,
        // same as always: there is no length to compare against ciphertext.
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

        // Re-read the state: `state` was sampled before this iteration drained
        // the socket. Requiring the receive buffer to be empty too stops a
        // connection being called complete while the peer's FIN arrived with
        // data still buffered. Fallback for anything the length check above
        // couldn't decide -- a response with no Content-Length, or the
        // discard_ciphertext path, which has no head to read one from.
        let s = sockets.get_mut::<tcp::Socket>(self.handle);
        let ended = matches!(
            s.state(),
            tcp::State::Closed | tcp::State::CloseWait | tcp::State::TimeWait
        );
        if self.handshake_done && self.request_queued && ended && self.outgoing.is_empty() && !s.can_recv() {
            // A closed connection only means "the response ended" if a
            // response actually started. A peer that closes before answering
            // -- or a connection reset mid-request -- otherwise arrives at the
            // caller as a *successful* empty response: rc OK, status 0, zero
            // bytes, and httpfs keeps whatever was already in the buffer it
            // handed us. Seen on c6in.large at sf=10 as a range read reporting
            // want=47 got=0 status=0, and the caller reporting
            // "HTTP Error: Request returned HTTP 0".
            //
            // discard_ciphertext is exempt: it bypasses the parser on purpose,
            // so it has no head to have finished.
            if !self.discard_ciphertext && !self.parser.headers_done() {
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

    /// Advance the record layer until it stops making progress. `Some` means
    /// the session failed and the connection is finished.
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
