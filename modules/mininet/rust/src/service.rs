//! Requests submitted from other threads, answered by the workers: one
//! polling thread per worker, and a blocking [`Service::get`] for everyone
//! else. The ceiling is `workers * conns_per_worker` requests in flight.

use alloc::boxed::Box;
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicPtr, AtomicU32, AtomicU64, AtomicUsize, Ordering};

use crate::endpoint::Endpoint;
use crate::error::Error;
use crate::ffi::{shim_thread_current, shim_thread_park, shim_thread_unpark, shim_time_ns};
use crate::http::{BodySink, BufferSink, ContentRange};
use crate::stats;
use crate::thread;
use crate::worker::{Worker, WorkerConfig, WorkerHandle};
use crate::Stack;

/// What a completed request delivered.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GetResult {
    pub status: u16,
    /// Bytes written into the caller's buffer.
    pub written: u64,
    /// What the head claimed the body was, when it said.
    pub content_length: Option<u64>,
    pub content_range: Option<ContentRange>,
    pub etag: Option<String>,
    pub last_modified: Option<String>,
}

/// Slot lifecycle. The submitter parks until this leaves `PENDING`, then waits
/// for `RELEASED` before returning -- see [`Slot`].
const PENDING: u32 = 0;
const PUBLISHED: u32 = 1;
const RELEASED: u32 = 2;

/// One request in flight, on the submitting thread's stack. The worker marks
/// `PUBLISHED`, wakes the submitter, then marks `RELEASED`; the submitter
/// returns only on `RELEASED`, so the worker never unparks a dead thread.
struct Slot {
    head: *const u8,
    head_len: usize,
    buf: *mut u8,
    buf_cap: usize,
    submitted_ns: u64,
    published_ns: AtomicU64,
    waiter: *mut c_void,
    state: AtomicU32,
    next: AtomicPtr<Slot>,
    outcome: UnsafeCell<Option<Result<GetResult, Error>>>,
}

impl Slot {
    fn stub() -> Slot {
        Slot {
            head: ptr::null(),
            head_len: 0,
            buf: ptr::null_mut(),
            buf_cap: 0,
            submitted_ns: 0,
            published_ns: AtomicU64::new(0),
            waiter: ptr::null_mut(),
            state: AtomicU32::new(RELEASED),
            next: AtomicPtr::new(ptr::null_mut()),
            outcome: UnsafeCell::new(None),
        }
    }

    /// # Safety
    /// `p` is a slot this worker popped and has not released.
    unsafe fn complete(p: *mut Slot, res: Result<GetResult, Error>) {
        let s = unsafe { &*p };
        let waiter = s.waiter;
        unsafe { *s.outcome.get() = Some(res) };
        s.published_ns.store(unsafe { shim_time_ns() }, Ordering::Relaxed);
        s.state.store(PUBLISHED, Ordering::Release);
        if !waiter.is_null() {
            unsafe { shim_thread_unpark(waiter) };
        }
        s.state.store(RELEASED, Ordering::Release);
    }
}

/// Multi-producer, single-consumer handoff to one worker: Vyukov's intrusive
/// queue, so a producer preempted mid-push cannot stall the worker on a lock.
struct Queue {
    head: AtomicPtr<Slot>,
    tail: UnsafeCell<*mut Slot>,
    /// Boxed so its address survives the queue being moved into its `Arc`.
    stub: Box<Slot>,
}

unsafe impl Send for Queue {}
unsafe impl Sync for Queue {}

impl Queue {
    fn new() -> Self {
        let stub = Box::new(Slot::stub());
        let p = &*stub as *const Slot as *mut Slot;
        Self {
            head: AtomicPtr::new(p),
            tail: UnsafeCell::new(p),
            stub,
        }
    }

    fn push(&self, slot: *mut Slot) {
        unsafe { (*slot).next.store(ptr::null_mut(), Ordering::Relaxed) };
        let prev = self.head.swap(slot, Ordering::AcqRel);
        unsafe { (*prev).next.store(slot, Ordering::Release) };
    }

    /// `None` also while a producer is between its two stores; try again next poll.
    fn pop(&self) -> Option<*mut Slot> {
        let stub = &*self.stub as *const Slot as *mut Slot;
        unsafe {
            let mut tail = *self.tail.get();
            let mut next = (*tail).next.load(Ordering::Acquire);
            if tail == stub {
                if next.is_null() {
                    return None;
                }
                *self.tail.get() = next;
                tail = next;
                next = (*next).next.load(Ordering::Acquire);
            }
            if !next.is_null() {
                *self.tail.get() = next;
                return Some(tail);
            }
            if self.head.load(Ordering::Acquire) != tail {
                return None;
            }
            self.push(stub);
            next = (*tail).next.load(Ordering::Acquire);
            if !next.is_null() {
                *self.tail.get() = next;
                return Some(tail);
            }
            None
        }
    }
}

pub struct ServiceConfig {
    pub peer: Endpoint,
    pub conns_per_worker: usize,
    pub rx_buffer: usize,
    pub tx_buffer: usize,
}

impl ServiceConfig {
    pub fn new(peer: Endpoint) -> Self {
        Self {
            peer,
            conns_per_worker: 8,
            rx_buffer: 4 * 1024 * 1024,
            tx_buffer: 32 * 1024,
        }
    }
}

/// One thread per RSS queue, each serving requests forever.
pub struct Service {
    queues: Vec<Arc<Queue>>,
    next: AtomicUsize,
}

/// Workers take cpus from the top; cpu 0 is where the application starts.
fn worker_cpu(q: u16) -> usize {
    let cpus = unsafe { crate::ffi::shim_cpu_count() } as usize;
    cpus.saturating_sub(1 + q as usize)
}

impl Service {
    /// Spawn a worker per queue, each pinned to a cpu of its own, never joined.
    pub fn start(stack: &Stack, cfg: &ServiceConfig) -> Result<Service, Error> {
        let mut queues = Vec::with_capacity(stack.queues() as usize);
        for q in 0..stack.queues() {
            let handle = stack.handle(q).ok_or(Error::NoDevice)?;
            let queue = Arc::new(Queue::new());
            let mine = queue.clone();
            let peer = cfg.peer.clone();
            let (conns, rx, tx) = (cfg.conns_per_worker, cfg.rx_buffer, cfg.tx_buffer);
            thread::spawn(
                move || serve(handle, peer, conns, rx, tx, mine),
                Some(worker_cpu(q)),
            );
            queues.push(queue);
        }
        Ok(Service {
            queues,
            next: AtomicUsize::new(0),
        })
    }

    /// Send `head` and write the response body into `buf`. Blocks, parked.
    pub fn get(&self, head: &[u8], buf: &mut [u8]) -> Result<GetResult, Error> {
        if self.queues.is_empty() {
            return Err(Error::NoDevice);
        }
        let t0 = unsafe { shim_time_ns() };
        stats::get_started(t0);
        let slot = Slot {
            head: head.as_ptr(),
            head_len: head.len(),
            buf: buf.as_mut_ptr(),
            buf_cap: buf.len(),
            submitted_ns: t0,
            published_ns: AtomicU64::new(0),
            waiter: unsafe { shim_thread_current() },
            state: AtomicU32::new(PENDING),
            next: AtomicPtr::new(ptr::null_mut()),
            outcome: UnsafeCell::new(None),
        };

        let i = self.next.fetch_add(1, Ordering::Relaxed) % self.queues.len();
        self.queues[i].push(&slot as *const Slot as *mut Slot);

        unsafe { shim_thread_park(slot.state.as_ptr() as *const u32) };
        while slot.state.load(Ordering::Acquire) != RELEASED {
            core::hint::spin_loop();
        }
        let t1 = unsafe { shim_time_ns() };
        stats::get_finished(
            t1.saturating_sub(t0),
            t1.saturating_sub(slot.published_ns.load(Ordering::Relaxed)),
            t1,
        );

        unsafe { (*slot.outcome.get()).take() }.unwrap_or(Err(Error::BadResponse))
    }
}

/// Producers on their own threads, one consumer here: every slot pushed comes
/// out exactly once. Returns (received, duplicates, expected).
#[cfg(feature = "selftest")]
pub(crate) fn queue_selftest() -> (u64, u64, u64) {
    const PRODUCERS: usize = 4;
    const EACH: usize = 4000;
    let q = Arc::new(Queue::new());
    let mut threads = Vec::new();
    for p in 0..PRODUCERS {
        let q = q.clone();
        threads.push(thread::spawn(
            move || {
                for i in 0..EACH {
                    let s = Box::into_raw(Box::new(Slot::stub()));
                    unsafe { (*s).head_len = p * EACH + i + 1 };
                    q.push(s);
                }
            },
            None,
        ));
    }
    let total = (PRODUCERS * EACH) as u64;
    let mut seen = alloc::vec![false; PRODUCERS * EACH + 1];
    let (mut got, mut dups, mut idle) = (0u64, 0u64, 0u64);
    while got < total && idle < 200_000_000 {
        match q.pop() {
            Some(p) => {
                let id = unsafe { (*p).head_len };
                if id == 0 || id > PRODUCERS * EACH || seen[id] {
                    dups += 1;
                } else {
                    seen[id] = true;
                }
                drop(unsafe { Box::from_raw(p) });
                got += 1;
                idle = 0;
            }
            None => {
                idle += 1;
                core::hint::spin_loop();
            }
        }
    }
    for t in threads {
        t.join();
    }
    (got, dups, total)
}

/// One producer: pops come out in push order.
#[cfg(feature = "selftest")]
pub(crate) fn queue_fifo_selftest() -> bool {
    let q = Queue::new();
    let mut ordered = q.pop().is_none();
    let slots: Vec<*mut Slot> = (1..=64usize)
        .map(|i| {
            let s = Box::into_raw(Box::new(Slot::stub()));
            unsafe { (*s).head_len = i };
            s
        })
        .collect();
    for &s in &slots {
        q.push(s);
    }
    for i in 1..=64usize {
        ordered &= q.pop().map_or(false, |p| unsafe { (*p).head_len } == i);
    }
    ordered &= q.pop().is_none();
    for s in slots {
        drop(unsafe { Box::from_raw(s) });
    }
    ordered
}

#[derive(Clone, Copy)]
struct Pending {
    slot: *mut Slot,
    reused: bool,
    retried: bool,
    picked_ns: u64,
}

fn head_of(s: &Slot) -> &[u8] {
    unsafe { core::slice::from_raw_parts(s.head, s.head_len) }
}

fn sink_of(s: &Slot) -> Box<dyn BodySink> {
    Box::new(unsafe { BufferSink::new(s.buf, s.buf_cap) })
}

fn result_of(c: &mut crate::Conn) -> Result<GetResult, Error> {
    if c.sink_overflowed() {
        return Err(Error::BufferTooSmall);
    }
    Ok(GetResult {
        status: c.status(),
        written: c.sink_written(),
        content_length: c.head().content_length,
        content_range: c.head().content_range,
        etag: c.head_mut().etag.take(),
        last_modified: c.head_mut().last_modified.take(),
    })
}

fn serve(
    handle: WorkerHandle,
    peer: Endpoint,
    conns: usize,
    rx_buffer: usize,
    tx_buffer: usize,
    queue: Arc<Queue>,
) {
    let queue_id = handle.queue_id();
    let mut cfg = WorkerConfig::new(peer);
    cfg.conns = conns;
    cfg.rx_buffer = rx_buffer;
    cfg.tx_buffer = tx_buffer;

    let mut w = match Worker::new(handle, &cfg) {
        Ok(w) => w,
        Err(e) => {
            println!("FAIL: q{}: {:?}", queue_id, e);
            loop {
                if let Some(p) = queue.pop() {
                    unsafe { Slot::complete(p, Err(e)) };
                }
                core::hint::spin_loop();
            }
        }
    };

    let mut pending: Vec<Option<Pending>> = (0..w.slots()).map(|_| None).collect();
    let epoch_ns = w.clock().epoch_ns();
    let mut prev_poll_ns = 0u64;
    let mut prev_busy_ns = 0u64;
    let mut acc = stats::PollAcc::default();

    loop {
        w.poll();
        let now_ns = w.last_poll_ns();
        if prev_poll_ns != 0 {
            acc.tick(now_ns.saturating_sub(prev_poll_ns), prev_busy_ns, w.last_active());
            acc.max(stats::Poll::IfaceNsMax, w.last_iface_ns());
            acc.max(stats::Poll::StepsNsMax, w.last_busy_ns().saturating_sub(w.last_iface_ns()));
            let (rxp, txp) = w.last_dev();
            acc.max(stats::Poll::RxPktsMax, rxp);
            acc.max(stats::Poll::TxPktsMax, txp);
        }
        prev_poll_ns = now_ns;
        prev_busy_ns = w.last_busy_ns();

        for slot in 0..w.slots() {
            let Some(pd) = pending[slot] else {
                let Some(p) = queue.pop() else { continue };
                let s = unsafe { &*p };
                let reused = w.idle_reusable(slot);
                let opened = if reused {
                    w.reuse(slot, head_of(s), sink_of(s))
                } else {
                    // A kept socket the peer since closed sits in CloseWait;
                    // connect() needs Closed.
                    w.release(slot);
                    w.connect_next(slot, head_of(s), sink_of(s))
                };
                match opened {
                    Ok(()) => {
                        pending[slot] = Some(Pending { slot: p, reused, retried: false, picked_ns: now_ns });
                    }
                    Err(e) => unsafe { Slot::complete(p, Err(e)) },
                }
                continue;
            };
            let Some(step) = w.conn(slot).and_then(|c| c.outcome()) else { continue };
            let res = match step {
                crate::Step::Complete => result_of(w.conn_mut(slot).expect("just observed")),
                crate::Step::Failed(e) => Err(e),
                crate::Step::Pending => unreachable!("outcome() is terminal"),
            };
            // A reused socket the peer had already closed: re-dial once. GET
            // and HEAD are safe to repeat.
            if pd.reused && !pd.retried && matches!(step, crate::Step::Failed(_)) {
                let s = unsafe { &*pd.slot };
                w.release(slot);
                if w.connect_next(slot, head_of(s), sink_of(s)).is_ok() {
                    stats::request_retried();
                    pending[slot] = Some(Pending { reused: false, retried: true, ..pd });
                    continue;
                }
            }
            let head_ns = w.conn(slot).and_then(|c| c.head_ns()).map(|h| h + epoch_ns);
            stats::request_finished(
                pd.picked_ns.saturating_sub(unsafe { (*pd.slot).submitted_ns }),
                now_ns.saturating_sub(pd.picked_ns),
                res.as_ref().map_or(0, |g| g.written),
                head_ns.map(|h| h.saturating_sub(pd.picked_ns)),
                head_ns.map(|h| now_ns.saturating_sub(h)),
            );
            unsafe { Slot::complete(pd.slot, res) };
            pending[slot] = None;
            if !w.idle_reusable(slot) {
                w.release(slot);
            }
        }
    }
}
