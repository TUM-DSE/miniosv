//! Why the stack, or a connection, failed; coarse on purpose. capi.rs turns
//! these into the return codes mininet.hh names.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    NoDevice,
    NoMemory,
    /// Key or table unreadable, or table not a power of two.
    RssUnavailable,
    DhcpTimeout,
    ArpTimeout,
    NoPorts,
    ConnectRejected,
    SynTimeout,
    Tls,
    /// Includes chunked transfer-encoding, which is not implemented.
    BadResponse,
    BufferTooSmall,
}
