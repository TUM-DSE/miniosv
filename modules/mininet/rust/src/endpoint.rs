//! What to dial, and what to send once dialled.

use alloc::string::String;

/// A server, by address -- there is no resolver in the guest. `host` is both
/// the `Host:` header and the TLS server name, so it must match the
/// certificate even though it is not what gets dialled.
#[derive(Clone)]
pub struct Endpoint {
    pub ip: [u8; 4],
    pub port: u16,
    pub host: String,
    /// False dials plain HTTP, which isolates the network stack from the
    /// record layer. The two are not comparable measurements.
    pub tls: bool,
}

impl Endpoint {
    pub fn new(ip: [u8; 4], host: impl Into<String>, tls: bool) -> Self {
        Self {
            ip,
            port: if tls { 443 } else { 80 },
            host: host.into(),
            tls,
        }
    }

    pub fn is_configured(&self) -> bool {
        self.ip != [0, 0, 0, 0]
    }
}

/// One request on one connection.
///
/// `head` is the complete, pre-rendered request head; the stack carries HTTP,
/// it does not build it, which keeps signing and header policy in the caller
/// (httpfs, for DuckDB).
///
/// No endpoint field: the worker carrying the request is already bound to
/// one, since its usable source ports depend on the peer's address.
pub struct Request<'a> {
    pub head: &'a [u8],
    /// Diagnostic: after the handshake, throw ciphertext away instead of
    /// decrypting it. Separates record-layer cost from network cost. The
    /// transfer is then unverifiable byte-for-byte and `body_bytes` counts
    /// ciphertext, including TLS and HTTP framing. Ignored without TLS, where
    /// there is no record layer to skip.
    pub discard_ciphertext: bool,
}
