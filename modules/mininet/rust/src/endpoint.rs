
use alloc::string::String;

/// A server, by address; `host` is the Host header and the TLS server name.
#[derive(Clone)]
pub struct Endpoint {
    pub ip: [u8; 4],
    pub port: u16,
    pub host: String,
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
}
