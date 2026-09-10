//! Why bringing the stack up, or a connection, failed.
//!
//! Deliberately coarse. Each variant names something a human has to act on --
//! a wrong instance type, a stale address, a bucket that refuses the request --
//! rather than an errno the caller could only print.

use core::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    NoDevice,
    /// Sizing is fixed, so a failed mempool allocation is a memory shortage.
    NoMemory,
    /// The Toeplitz key or the indirection table could not be read, or the
    /// table is not a power of two. Without both, source ports cannot be
    /// chosen to steer, and every worker would see other workers' traffic.
    RssUnavailable,
    /// Usually means the queue-0 RX path is not delivering.
    DhcpTimeout,
    ArpTimeout,
    /// Only possible with a pathological indirection table.
    NoPorts,
    ConnectRejected,
    SynTimeout,
    Tls,
    /// Includes chunked transfer-encoding, which is not implemented.
    BadResponse,
    /// The excess was dropped rather than written past the end, so what
    /// arrived is not the response that was asked for.
    BufferTooSmall,
}

impl Error {
    pub fn as_str(&self) -> &'static str {
        match self {
            Error::NoDevice => "no usable NIC",
            Error::NoMemory => "mempool allocation failed",
            Error::RssUnavailable => "RSS key or indirection table unavailable",
            Error::DhcpTimeout => "DHCP timed out",
            Error::ArpTimeout => "gateway ARP timed out",
            Error::NoPorts => "no ephemeral port steers to this queue",
            Error::ConnectRejected => "connect() rejected by the socket",
            Error::SynTimeout => "SYN timeout: no SYN-ACK",
            Error::Tls => "TLS failure",
            Error::BadResponse => "malformed or unsupported HTTP response",
            Error::BufferTooSmall => "response body exceeded the caller's buffer",
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}
