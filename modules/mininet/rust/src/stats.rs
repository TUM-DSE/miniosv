//! Stack-wide counters.
//!
//! These are cheap and stay in the build. Each one is an invariant that should
//! hold on every run, so a regression surfaces in the output rather than as a
//! mysteriously slower number. Anything that is a *policy* question -- whether
//! a 206 was expected, how many blocks a run planned -- belongs to the caller,
//! not here.

use core::sync::atomic::{AtomicU64, Ordering};

static MISROUTED_DROPS: AtomicU64 = AtomicU64::new(0);
static TX_ALLOC_FAIL: AtomicU64 = AtomicU64::new(0);
static TX_BURST_FAIL: AtomicU64 = AtomicU64::new(0);
static CONNS_ESTABLISHED: AtomicU64 = AtomicU64::new(0);
static CONNS_FAILED: AtomicU64 = AtomicU64::new(0);
static SYN_RETRIES: AtomicU64 = AtomicU64::new(0);
static SETUP_MS_TOTAL: AtomicU64 = AtomicU64::new(0);
static REQUESTS_SERVED: AtomicU64 = AtomicU64::new(0);
static REQUESTS_REUSED: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Copy, Default)]
pub struct Stats {
    /// Packets discarded because they arrived on a queue that does not own the
    /// destination port. Source ports are chosen so this cannot happen; a
    /// nonzero value means the RSS model no longer matches the hardware.
    pub misrouted_drops: u64,
    /// Frames dropped before reaching the NIC: no mbuf available, or the TX
    /// ring refused them. Both are invisible to smoltcp, which believes it
    /// sent them.
    pub tx_alloc_fail: u64,
    pub tx_burst_fail: u64,
    pub conns_established: u64,
    pub conns_failed: u64,
    /// Extra SYNs beyond the first. Should be 0: a source port is chosen so
    /// the SYN-ACK provably comes back on the right queue, which leaves real
    /// packet loss as the only cause.
    pub syn_retries: u64,
    pub setup_ms_total: u64,
    /// Requests served on a connection that was already open, versus one that
    /// had to be dialled fresh. The gap between the two is the M3 win.
    pub requests_served: u64,
    pub requests_reused: u64,
}

pub fn snapshot() -> Stats {
    Stats {
        misrouted_drops: MISROUTED_DROPS.load(Ordering::Relaxed),
        tx_alloc_fail: TX_ALLOC_FAIL.load(Ordering::Relaxed),
        tx_burst_fail: TX_BURST_FAIL.load(Ordering::Relaxed),
        conns_established: CONNS_ESTABLISHED.load(Ordering::Relaxed),
        conns_failed: CONNS_FAILED.load(Ordering::Relaxed),
        syn_retries: SYN_RETRIES.load(Ordering::Relaxed),
        setup_ms_total: SETUP_MS_TOTAL.load(Ordering::Relaxed),
        requests_served: REQUESTS_SERVED.load(Ordering::Relaxed),
        requests_reused: REQUESTS_REUSED.load(Ordering::Relaxed),
    }
}

pub(crate) fn misrouted_drop() {
    MISROUTED_DROPS.fetch_add(1, Ordering::Relaxed);
}
pub(crate) fn tx_alloc_fail() {
    TX_ALLOC_FAIL.fetch_add(1, Ordering::Relaxed);
}
pub(crate) fn tx_burst_fail() {
    TX_BURST_FAIL.fetch_add(1, Ordering::Relaxed);
}
pub(crate) fn conn_established(attempts: u16, setup_ms: i64) {
    CONNS_ESTABLISHED.fetch_add(1, Ordering::Relaxed);
    SYN_RETRIES.fetch_add(attempts.saturating_sub(1) as u64, Ordering::Relaxed);
    SETUP_MS_TOTAL.fetch_add(setup_ms.max(0) as u64, Ordering::Relaxed);
}
pub(crate) fn conn_failed(setup_ms: i64) {
    CONNS_FAILED.fetch_add(1, Ordering::Relaxed);
    SETUP_MS_TOTAL.fetch_add(setup_ms.max(0) as u64, Ordering::Relaxed);
}
pub(crate) fn request_started(reused: bool) {
    REQUESTS_SERVED.fetch_add(1, Ordering::Relaxed);
    if reused {
        REQUESTS_REUSED.fetch_add(1, Ordering::Relaxed);
    }
}
