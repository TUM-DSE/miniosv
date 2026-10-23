//! TLS 1.3 records once the handshake is over: rustls hands over the traffic keys
//! (`dangerous_into_kernel_connection`, as for kTLS) and records are sealed and opened here, so a reader can decrypt
//! its response wherever its bytes land.

use alloc::vec::Vec;

use ring::aead::{Aad, LessSafeKey, Nonce, UnboundKey, AES_128_GCM, AES_256_GCM};
use rustls::ConnectionTrafficSecrets;

pub(crate) const HEADER: usize = 5;
pub(crate) const TAG: usize = 16;
pub(crate) const APPLICATION_DATA: u8 = 23;
pub(crate) const HANDSHAKE: u8 = 22;
pub(crate) const ALERT: u8 = 21;
/// A record's payload at most: 2^14 of plaintext, its content type, padding and tag (RFC 8446 5.2).
pub(crate) const MAX_CIPHERTEXT: usize = 16384 + 256;

/// One direction's key and IV, kept as bytes so each reader can build its own key.
#[derive(Clone)]
pub(crate) struct Secret {
    aes256: bool,
    key: [u8; 32],
    pub(crate) iv: [u8; 12],
}

impl Secret {
    pub(crate) fn from_rustls(s: &ConnectionTrafficSecrets) -> Option<Secret> {
        let (aes256, key, iv) = match s {
            ConnectionTrafficSecrets::Aes128Gcm { key, iv } => (false, key, iv),
            ConnectionTrafficSecrets::Aes256Gcm { key, iv } => (true, key, iv),
            _ => return None,
        };
        let mut out = Secret { aes256, key: [0; 32], iv: [0; 12] };
        let k = key.as_ref();
        out.key[..k.len()].copy_from_slice(k);
        out.iv.copy_from_slice(iv.as_ref());
        Some(out)
    }

    pub(crate) fn key(&self) -> LessSafeKey {
        let (alg, n) = if self.aes256 { (&AES_256_GCM, 32) } else { (&AES_128_GCM, 16) };
        LessSafeKey::new(UnboundKey::new(alg, &self.key[..n]).expect("AES-GCM key length"))
    }
}

/// The per-record nonce: the IV with the sequence number XORed into its tail.
pub(crate) fn nonce(iv: &[u8; 12], seq: u64) -> Nonce {
    let mut n = *iv;
    for (b, s) in n[4..].iter_mut().zip(seq.to_be_bytes()) {
        *b ^= s;
    }
    Nonce::assume_unique_for_key(n)
}

/// The record header, which is also the AEAD's additional data.
pub(crate) fn header(len: usize) -> [u8; HEADER] {
    [APPLICATION_DATA, 3, 3, (len >> 8) as u8, len as u8]
}

/// Decrypt one record's payload where it lies; the content and its inner type, padding removed.
pub(crate) fn open<'a>(key: &LessSafeKey, iv: &[u8; 12], seq: u64, payload: &'a mut [u8]) -> Option<(&'a [u8], u8)> {
    let hdr = header(payload.len());
    let plain = key.open_in_place(nonce(iv, seq), Aad::from(hdr), payload).ok()?;
    let end = plain.iter().rposition(|&b| b != 0)?;
    Some((&plain[..end], plain[end]))
}

/// Append `data` to `out` as one application-data record.
pub(crate) fn seal(key: &LessSafeKey, iv: &[u8; 12], seq: u64, data: &[u8], out: &mut Vec<u8>) -> bool {
    let len = data.len() + 1 + TAG;
    if len > MAX_CIPHERTEXT {
        return false;
    }
    let hdr = header(len);
    out.extend_from_slice(&hdr);
    let start = out.len();
    out.extend_from_slice(data);
    out.push(APPLICATION_DATA);
    match key.seal_in_place_separate_tag(nonce(iv, seq), Aad::from(hdr), &mut out[start..]) {
        Ok(tag) => {
            out.extend_from_slice(tag.as_ref());
            true
        }
        Err(_) => false,
    }
}
