//! Stack-wide counters. Cheap, and always in the build.

use core::sync::atomic::{AtomicU64, Ordering};

static MISROUTED_DROPS: AtomicU64 = AtomicU64::new(0);
static TX_ALLOC_FAIL: AtomicU64 = AtomicU64::new(0);
static TX_BURST_FAIL: AtomicU64 = AtomicU64::new(0);
static CONNS_ESTABLISHED: AtomicU64 = AtomicU64::new(0);
static CONNS_FAILED: AtomicU64 = AtomicU64::new(0);
static REQUESTS_SERVED: AtomicU64 = AtomicU64::new(0);
static REQUESTS_REUSED: AtomicU64 = AtomicU64::new(0);
static REQUESTS_RETRIED: AtomicU64 = AtomicU64::new(0);
static REQUESTS_DONE: AtomicU64 = AtomicU64::new(0);
static QUEUE_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static WIRE_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static BODY_BYTES: AtomicU64 = AtomicU64::new(0);
static TTFB_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static TTFB_N: AtomicU64 = AtomicU64::new(0);
static XFER_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static XFER_N: AtomicU64 = AtomicU64::new(0);
static DIAL_NS_MAX: AtomicU64 = AtomicU64::new(0);
static WAKE_N: AtomicU64 = AtomicU64::new(0);
static WAKE_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static WAKE_NS_MAX: AtomicU64 = AtomicU64::new(0);
static GET_CALLS: AtomicU64 = AtomicU64::new(0);
static GET_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static IN_FLIGHT: AtomicU64 = AtomicU64::new(0);
static ACTIVE_SINCE_NS: AtomicU64 = AtomicU64::new(0);
static ACTIVE_NS_TOTAL: AtomicU64 = AtomicU64::new(0);

static SETUP_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static SETUP_NS_MAX: AtomicU64 = AtomicU64::new(0);
static SETUP_HIST: [AtomicU64; BUCKETS] = [const { AtomicU64::new(0) }; BUCKETS];
static DIAL_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static DIAL_HIST: [AtomicU64; BUCKETS] = [const { AtomicU64::new(0) }; BUCKETS];

/// Worker-loop counters: sums up to `WorkNs`, maxima after.
#[derive(Clone, Copy)]
#[repr(usize)]
pub(crate) enum Poll {
    Iters,
    GapNsTotal,
    GapsOver1ms,
    BusyNs,
    ActiveIters,
    WorkNs,
    GapNsMax,
    BusyNsMax,
    LoopNsMax,
    IfaceNsMax,
    StepsNsMax,
    RxPktsMax,
    TxPktsMax,
    N,
}
static POLL: [AtomicU64; Poll::N as usize] = [const { AtomicU64::new(0) }; Poll::N as usize];

/// Log2 histogram over microseconds: bucket `k > 0` holds `[2^(k-1), 2^k)`.
const BUCKETS: usize = 32;

fn bucket(us: u64) -> usize {
    if us == 0 {
        return 0;
    }
    (64 - us.leading_zeros() as usize).min(BUCKETS - 1)
}

fn record(hist: &[AtomicU64; BUCKETS], total: &AtomicU64, max: &AtomicU64, ns: u64) {
    total.fetch_add(ns, Ordering::Relaxed);
    max.fetch_max(ns, Ordering::Relaxed);
    hist[bucket(ns / 1_000)].fetch_add(1, Ordering::Relaxed);
}

/// Interpolated inside its bucket; [`dist`] clamps it to the exact max.
fn percentile(hist: &[u64; BUCKETS], p: u64) -> u64 {
    let count: u64 = hist.iter().sum();
    if count == 0 {
        return 0;
    }
    let target = (count * p).div_ceil(100).max(1);
    let mut seen = 0u64;
    for (k, &n) in hist.iter().enumerate() {
        if n == 0 {
            continue;
        }
        if seen + n >= target {
            if k == 0 {
                return 0;
            }
            let lo = 1u64 << (k - 1);
            return lo + lo * (target - seen - 1) / n;
        }
        seen += n;
    }
    0
}

/// One measured duration, in microseconds.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct Dist {
    pub n: u64,
    pub us_avg: u64,
    pub us_p50: u64,
    pub us_p90: u64,
    pub us_max: u64,
}

fn dist(hist: &[AtomicU64; BUCKETS], total: &AtomicU64, max: &AtomicU64) -> Dist {
    let mut h = [0u64; BUCKETS];
    for (o, a) in h.iter_mut().zip(hist.iter()) {
        *o = a.load(Ordering::Relaxed);
    }
    let n: u64 = h.iter().sum();
    let us_max = max.load(Ordering::Relaxed) / 1_000;
    Dist {
        n,
        us_avg: if n == 0 { 0 } else { total.load(Ordering::Relaxed) / n / 1_000 },
        us_p50: percentile(&h, 50).min(us_max),
        us_p90: percentile(&h, 90).min(us_max),
        us_max,
    }
}

/// The distribution of the given samples, through the same histogram.
#[cfg(feature = "selftest")]
pub(crate) fn dist_of(samples_ns: &[u64]) -> Dist {
    let hist: [AtomicU64; BUCKETS] = [const { AtomicU64::new(0) }; BUCKETS];
    let (total, max) = (AtomicU64::new(0), AtomicU64::new(0));
    for &ns in samples_ns {
        record(&hist, &total, &max, ns);
    }
    dist(&hist, &total, &max)
}

/// The C ABI too: `mininet::conn_stats` in mininet.hh mirrors this layout.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct Stats {
    /// Nonzero means the RSS model no longer matches the hardware.
    pub misrouted_drops: u64,
    /// Frames smoltcp believes it sent and the NIC never got (no mbuf).
    pub tx_alloc_fail: u64,
    /// Frames the TX ring would not take at once; held and sent next poll.
    pub tx_burst_fail: u64,
    pub conns_established: u64,
    pub conns_failed: u64,
    /// SYN on the wire to Established: the measured RTT.
    pub setup: Dist,
    /// CPU spent building a connection before its SYN could go out.
    pub dial: Dist,
    pub requests_served: u64,
    pub requests_reused: u64,
    pub requests_retried: u64,
    /// Per request: submit -> pick-up (queue), pick-up -> complete (wire),
    /// and the wire split at the first head byte (ttfb / xfer).
    pub requests_done: u64,
    pub queue_ns_total: u64,
    pub wire_ns_total: u64,
    pub body_bytes: u64,
    pub ttfb_ns_total: u64,
    pub ttfb_n: u64,
    pub xfer_ns_total: u64,
    pub xfer_n: u64,
    pub poll_iters: u64,
    pub poll_gap_ns_total: u64,
    pub poll_gap_ns_max: u64,
    pub poll_gaps_over_1ms: u64,
    pub poll_busy_ns: u64,
    pub poll_active_iters: u64,
    pub poll_work_ns: u64,
    /// Longest single poll, and longest stretch outside poll between two polls.
    pub poll_busy_ns_max: u64,
    pub poll_loop_ns_max: u64,
    /// Longest device poll and longest pass over the connections.
    pub iface_ns_max: u64,
    pub steps_ns_max: u64,
    /// Most packets one poll received and transmitted.
    pub rx_pkts_max: u64,
    pub tx_pkts_max: u64,
    /// Publish -> submitter running again.
    pub wake_n: u64,
    pub wake_ns_total: u64,
    pub wake_ns_max: u64,
    pub get_calls: u64,
    pub get_ns_total: u64,
    /// Wall time during which at least one request was outstanding.
    pub active_ns_total: u64,
    /// Off the device; a frame it dropped never reached any queue.
    pub nic_ipackets: u64,
    pub nic_ibytes: u64,
    pub nic_imissed: u64,
    pub nic_ierrors: u64,
    pub nic_rx_nombuf: u64,
}

fn poll(k: Poll) -> u64 {
    POLL[k as usize].load(Ordering::Relaxed)
}

pub fn snapshot() -> Stats {
    let nic = crate::nic::eth_stats().unwrap_or_default();
    Stats {
        misrouted_drops: MISROUTED_DROPS.load(Ordering::Relaxed),
        tx_alloc_fail: TX_ALLOC_FAIL.load(Ordering::Relaxed),
        tx_burst_fail: TX_BURST_FAIL.load(Ordering::Relaxed),
        conns_established: CONNS_ESTABLISHED.load(Ordering::Relaxed),
        conns_failed: CONNS_FAILED.load(Ordering::Relaxed),
        setup: dist(&SETUP_HIST, &SETUP_NS_TOTAL, &SETUP_NS_MAX),
        dial: dist(&DIAL_HIST, &DIAL_NS_TOTAL, &DIAL_NS_MAX),
        requests_served: REQUESTS_SERVED.load(Ordering::Relaxed),
        requests_reused: REQUESTS_REUSED.load(Ordering::Relaxed),
        requests_retried: REQUESTS_RETRIED.load(Ordering::Relaxed),
        requests_done: REQUESTS_DONE.load(Ordering::Relaxed),
        queue_ns_total: QUEUE_NS_TOTAL.load(Ordering::Relaxed),
        wire_ns_total: WIRE_NS_TOTAL.load(Ordering::Relaxed),
        body_bytes: BODY_BYTES.load(Ordering::Relaxed),
        ttfb_ns_total: TTFB_NS_TOTAL.load(Ordering::Relaxed),
        ttfb_n: TTFB_N.load(Ordering::Relaxed),
        xfer_ns_total: XFER_NS_TOTAL.load(Ordering::Relaxed),
        xfer_n: XFER_N.load(Ordering::Relaxed),
        poll_iters: poll(Poll::Iters),
        poll_gap_ns_total: poll(Poll::GapNsTotal),
        poll_gap_ns_max: poll(Poll::GapNsMax),
        poll_gaps_over_1ms: poll(Poll::GapsOver1ms),
        poll_busy_ns: poll(Poll::BusyNs),
        poll_active_iters: poll(Poll::ActiveIters),
        poll_work_ns: poll(Poll::WorkNs),
        poll_busy_ns_max: poll(Poll::BusyNsMax),
        poll_loop_ns_max: poll(Poll::LoopNsMax),
        iface_ns_max: poll(Poll::IfaceNsMax),
        steps_ns_max: poll(Poll::StepsNsMax),
        rx_pkts_max: poll(Poll::RxPktsMax),
        tx_pkts_max: poll(Poll::TxPktsMax),
        wake_n: WAKE_N.load(Ordering::Relaxed),
        wake_ns_total: WAKE_NS_TOTAL.load(Ordering::Relaxed),
        wake_ns_max: WAKE_NS_MAX.load(Ordering::Relaxed),
        get_calls: GET_CALLS.load(Ordering::Relaxed),
        get_ns_total: GET_NS_TOTAL.load(Ordering::Relaxed),
        active_ns_total: ACTIVE_NS_TOTAL.load(Ordering::Relaxed),
        nic_ipackets: nic.ipackets,
        nic_ibytes: nic.ibytes,
        nic_imissed: nic.imissed,
        nic_ierrors: nic.ierrors,
        nic_rx_nombuf: nic.rx_nombuf,
    }
}

pub(crate) fn request_retried() {
    REQUESTS_RETRIED.fetch_add(1, Ordering::Relaxed);
}
pub(crate) fn request_finished(
    queue_ns: u64,
    wire_ns: u64,
    bytes: u64,
    ttfb_ns: Option<u64>,
    xfer_ns: Option<u64>,
) {
    REQUESTS_DONE.fetch_add(1, Ordering::Relaxed);
    QUEUE_NS_TOTAL.fetch_add(queue_ns, Ordering::Relaxed);
    WIRE_NS_TOTAL.fetch_add(wire_ns, Ordering::Relaxed);
    BODY_BYTES.fetch_add(bytes, Ordering::Relaxed);
    if let Some(t) = ttfb_ns {
        TTFB_NS_TOTAL.fetch_add(t, Ordering::Relaxed);
        TTFB_N.fetch_add(1, Ordering::Relaxed);
    }
    if let Some(x) = xfer_ns {
        XFER_NS_TOTAL.fetch_add(x, Ordering::Relaxed);
        XFER_N.fetch_add(1, Ordering::Relaxed);
    }
}
/// Per-worker, flushed in batches so workers do not share cache lines per iteration.
#[derive(Default)]
pub(crate) struct PollAcc([u64; Poll::N as usize]);

impl PollAcc {
    pub(crate) fn add(&mut self, k: Poll, n: u64) {
        self.0[k as usize] += n;
    }

    pub(crate) fn max(&mut self, k: Poll, n: u64) {
        self.0[k as usize] = self.0[k as usize].max(n);
    }

    pub(crate) fn tick(&mut self, gap_ns: u64, busy_ns: u64, active: bool) {
        self.add(Poll::Iters, 1);
        self.add(Poll::GapNsTotal, gap_ns);
        self.max(Poll::GapNsMax, gap_ns);
        self.add(Poll::GapsOver1ms, (gap_ns > 1_000_000) as u64);
        self.add(Poll::BusyNs, busy_ns);
        self.add(Poll::ActiveIters, active as u64);
        self.add(Poll::WorkNs, if active { busy_ns } else { 0 });
        self.max(Poll::BusyNsMax, busy_ns);
        self.max(Poll::LoopNsMax, gap_ns.saturating_sub(busy_ns));
        if self.0[Poll::Iters as usize] >= 1024 {
            self.flush();
        }
    }

    pub(crate) fn flush(&mut self) {
        for (i, v) in self.0.iter().enumerate() {
            if i <= Poll::WorkNs as usize {
                POLL[i].fetch_add(*v, Ordering::Relaxed);
            } else {
                POLL[i].fetch_max(*v, Ordering::Relaxed);
            }
        }
        *self = Self::default();
    }
}

pub(crate) fn get_started(now_ns: u64) {
    if IN_FLIGHT.fetch_add(1, Ordering::AcqRel) == 0 {
        ACTIVE_SINCE_NS.store(now_ns, Ordering::Relaxed);
    }
}

pub(crate) fn get_finished(total_ns: u64, wake_ns: u64, now_ns: u64) {
    if IN_FLIGHT.fetch_sub(1, Ordering::AcqRel) == 1 {
        let since = ACTIVE_SINCE_NS.load(Ordering::Relaxed);
        ACTIVE_NS_TOTAL.fetch_add(now_ns.saturating_sub(since), Ordering::Relaxed);
    }
    GET_CALLS.fetch_add(1, Ordering::Relaxed);
    GET_NS_TOTAL.fetch_add(total_ns, Ordering::Relaxed);
    WAKE_N.fetch_add(1, Ordering::Relaxed);
    WAKE_NS_TOTAL.fetch_add(wake_ns, Ordering::Relaxed);
    WAKE_NS_MAX.fetch_max(wake_ns, Ordering::Relaxed);
}

pub(crate) fn misrouted_drop() {
    MISROUTED_DROPS.fetch_add(1, Ordering::Relaxed);
}
pub(crate) fn tx_alloc_fail() {
    TX_ALLOC_FAIL.fetch_add(1, Ordering::Relaxed);
}
/// Frames the TX ring would not take in the poll that made them; they are
/// retried next poll, so this counts delay, not loss.
pub(crate) fn tx_held(n: u64) {
    TX_BURST_FAIL.fetch_add(n, Ordering::Relaxed);
}
pub(crate) fn conn_established(setup_ns: u64) {
    CONNS_ESTABLISHED.fetch_add(1, Ordering::Relaxed);
    record(&SETUP_HIST, &SETUP_NS_TOTAL, &SETUP_NS_MAX, setup_ns);
}
pub(crate) fn conn_failed(setup_ns: u64) {
    CONNS_FAILED.fetch_add(1, Ordering::Relaxed);
    record(&SETUP_HIST, &SETUP_NS_TOTAL, &SETUP_NS_MAX, setup_ns);
}
pub(crate) fn conn_dialled(dial_ns: u64) {
    record(&DIAL_HIST, &DIAL_NS_TOTAL, &DIAL_NS_MAX, dial_ns);
}
pub(crate) fn request_started(reused: bool) {
    REQUESTS_SERVED.fetch_add(1, Ordering::Relaxed);
    if reused {
        REQUESTS_REUSED.fetch_add(1, Ordering::Relaxed);
    }
}
