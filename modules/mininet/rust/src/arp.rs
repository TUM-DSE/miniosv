//! ARP, done by hand rather than through smoltcp.
//!
//! The gateway is resolved once, on queue 0. Per-worker interfaces on other
//! queues cannot do it for themselves: an ARP reply is not hashed onto a queue
//! by the 4-tuple, so it would arrive somewhere arbitrary. Instead every worker
//! is handed the answer and seeds its neighbour cache with a fabricated reply.

use alloc::vec::Vec;

fn arp_frame(
    dst_mac: [u8; 6],
    src_mac: [u8; 6],
    op: u16,
    sender_ip: [u8; 4],
    target_mac: [u8; 6],
    target_ip: [u8; 4],
) -> [u8; 42] {
    let mut f = [0u8; 42];
    f[0..6].copy_from_slice(&dst_mac);
    f[6..12].copy_from_slice(&src_mac);
    f[12..14].copy_from_slice(&[0x08, 0x06]); // ARP ethertype
    f[14..16].copy_from_slice(&[0x00, 0x01]); // HTYPE
    f[16..18].copy_from_slice(&[0x08, 0x00]); // PTYPE
    f[18] = 6;
    f[19] = 4;
    f[20..22].copy_from_slice(&op.to_be_bytes());
    f[22..28].copy_from_slice(&src_mac);
    f[28..32].copy_from_slice(&sender_ip);
    f[32..38].copy_from_slice(&target_mac);
    f[38..42].copy_from_slice(&target_ip);
    f
}

pub(crate) fn request(our_mac: [u8; 6], our_ip: [u8; 4], target_ip: [u8; 4]) -> [u8; 42] {
    arp_frame([0xff; 6], our_mac, 1, our_ip, [0; 6], target_ip)
}

/// A reply the gateway never sent, synthesised so a worker's interface learns
/// the MAC on its first poll.
pub(crate) fn synthetic_reply(
    sender_mac: [u8; 6],
    sender_ip: [u8; 4],
    target_mac: [u8; 6],
    target_ip: [u8; 4],
) -> Vec<u8> {
    arp_frame(target_mac, sender_mac, 2, sender_ip, target_mac, target_ip).to_vec()
}

pub(crate) fn parse_reply_from(frame: &[u8], expected_ip: [u8; 4]) -> Option<[u8; 6]> {
    if frame.len() < 42 || frame[12..14] != [0x08, 0x06] || frame[20..22] != [0x00, 0x02] {
        return None;
    }
    if frame[28..32] != expected_ip[..] {
        return None;
    }
    let mut mac = [0u8; 6];
    mac.copy_from_slice(&frame[22..28]);
    Some(mac)
}
