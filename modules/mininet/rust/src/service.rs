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
use core::sync::atomic::{fence, AtomicBool, AtomicPtr, AtomicU32, AtomicU64, AtomicU8, AtomicUsize, Ordering};

use crate::endpoint::Endpoint;
use crate::error::Error;
use crate::feed::{Feed, Reader, DONE, FAILED};
use crate::ffi::{
    shim_rx_intr_arm, shim_rx_intr_disarm, shim_thread_current, shim_thread_park, shim_thread_unpark, shim_time_ns,
    shim_waiter_new, shim_waiter_signal, shim_waiter_wait, SHIM_THREAD_BOOST, SHIM_THREAD_RESERVE,
};
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
    /// From a worker taking the request to the head being in: the network's part, not the queue's.
    pub ttfb_ns: Option<u64>,
}

/// Slot lifecycle. The submitter parks until this leaves `PENDING`, then waits
/// for `RELEASED` before returning -- see [`Slot`].
const PENDING: u32 = 0;
const PUBLISHED: u32 = 1;
const RELEASED: u32 = 2;

/// One request in flight, on the submitting thread's stack. The worker marks
/// `PUBLISHED`, wakes the submitter, then marks `RELEASED`; the submitter
/// returns only on `RELEASED`, so the worker never unparks a dead thread.
/// Until then the submitter may be reading the body out of `feed`, woken
/// through `signal` while `waiting`.
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
    feed: AtomicPtr<Feed>,
    signal: AtomicU32,
    waiting: AtomicBool,
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
            feed: AtomicPtr::new(ptr::null_mut()),
            signal: AtomicU32::new(0),
            waiting: AtomicBool::new(false),
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
        s.signal.store(1, Ordering::Release);
        if !waiter.is_null() {
            unsafe { shim_thread_unpark(waiter) };
        }
        s.state.store(RELEASED, Ordering::Release);
    }

    /// The outcome, for a submitter reading a feed: it returns as soon as the feed says the worker let go.
    ///
    /// # Safety
    /// `p` is a slot this worker popped and has not let go of.
    unsafe fn hand_over(p: *mut Slot, res: Result<GetResult, Error>) {
        let s = unsafe { &*p };
        unsafe { *s.outcome.get() = Some(res) };
        s.published_ns.store(unsafe { shim_time_ns() }, Ordering::Relaxed);
    }

    /// Wake the submitter if it sleeps waiting for its feed.
    ///
    /// # Safety
    /// `p` is a slot this worker popped and has not released.
    unsafe fn notify(p: *mut Slot) {
        let s = unsafe { &*p };
        // Against the submitter's store of `waiting` and its look at the feed.
        fence(Ordering::SeqCst);
        if s.waiting.load(Ordering::Relaxed) {
            s.signal.store(1, Ordering::Release);
            unsafe { shim_thread_unpark(s.waiter) };
        }
    }
}

/// Multi-producer, single-consumer handoff to one worker: Vyukov's intrusive
/// queue, so a producer preempted mid-push cannot stall the worker on a lock.
struct Queue {
    head: AtomicPtr<Slot>,
    tail: UnsafeCell<*mut Slot>,
    /// Boxed so its address survives the queue being moved into its `Arc`.
    stub: Box<Slot>,
    /// Worker::new's verdict, for [`Service::start`] to wait on: `BOOTING`,
    /// `UP`, or `DOWN` with the error in `boot_err`.
    ready: AtomicU8,
    boot_err: UnsafeCell<Option<Error>>,
    /// The worker sleeps on `waiter` (idle_us): a push sets it going again.
    sleeping: AtomicBool,
    waiter: AtomicPtr<c_void>,
}

const BOOTING: u8 = 0;
const UP: u8 = 1;
const DOWN: u8 = 2;

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
            ready: AtomicU8::new(BOOTING),
            boot_err: UnsafeCell::new(None),
            sleeping: AtomicBool::new(false),
            waiter: AtomicPtr::new(ptr::null_mut()),
        }
    }

    fn push(&self, slot: *mut Slot) {
        unsafe { (*slot).next.store(ptr::null_mut(), Ordering::Relaxed) };
        let prev = self.head.swap(slot, Ordering::AcqRel);
        unsafe { (*prev).next.store(slot, Ordering::Release) };
        self.wake_worker();
    }

    /// Wake the worker if it sleeps: something to do was published before this.
    fn wake_worker(&self) {
        // Against the worker's store of `sleeping` and its last look at its work: one of the two sees the other.
        fence(Ordering::SeqCst);
        if self.sleeping.load(Ordering::Relaxed) {
            let w = self.waiter.load(Ordering::Acquire);
            if !w.is_null() {
                unsafe { shim_waiter_signal(w) };
            }
        }
    }

    /// Whether a push may be waiting; consumer side only. A producer between its two stores counts.
    fn maybe_pending(&self) -> bool {
        let stub = &*self.stub as *const Slot as *mut Slot;
        unsafe {
            let tail = *self.tail.get();
            tail != stub || !(*stub).next.load(Ordering::Acquire).is_null() || self.head.load(Ordering::SeqCst) != stub
        }
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
    /// Dial the boot-resolved addresses, not `peer.ip`.
    pub resolve: bool,
    /// 0: poll forever on a reserved cpu. Otherwise sleep on the queue's interrupt after this long with nothing to do.
    pub idle_ns: u64,
    /// The worker writes bodies into callers' buffers; otherwise each caller reads its own out of the frames.
    pub worker_copies: bool,
}

impl ServiceConfig {
    pub fn new(peer: Endpoint) -> Self {
        Self {
            peer,
            conns_per_worker: 8,
            rx_buffer: 4 * 1024 * 1024,
            tx_buffer: 32 * 1024,
            resolve: false,
            idle_ns: 0,
            worker_copies: false,
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
    /// Spawn a worker per queue, each pinned to a cpu of its own, never
    /// joined, and wait for every one to come up: a worker that cannot is
    /// the stack's failure, not a share of every request's.
    pub fn start(stack: &Stack, cfg: &ServiceConfig) -> Result<Service, Error> {
        let mut queues = Vec::with_capacity(stack.queues() as usize);
        for q in 0..stack.queues() {
            let handle = stack.handle(q).ok_or(Error::NoDevice)?;
            let queue = Arc::new(Queue::new());
            let mine = queue.clone();
            let peer = cfg.peer.clone();
            let (conns, rx, tx, resolve) = (cfg.conns_per_worker, cfg.rx_buffer, cfg.tx_buffer, cfg.resolve);
            let idle_ns = cfg.idle_ns;
            let worker_copies = cfg.worker_copies;
            let flags = if idle_ns == 0 { SHIM_THREAD_RESERVE } else { SHIM_THREAD_BOOST };
            thread::spawn(
                move || serve(handle, peer, conns, rx, tx, resolve, idle_ns, worker_copies, mine),
                Some(worker_cpu(q)),
                flags,
            );
            queues.push(queue);
        }
        for q in &queues {
            while q.ready.load(Ordering::Acquire) == BOOTING {
                core::hint::spin_loop();
            }
            if q.ready.load(Ordering::Acquire) == DOWN {
                // The workers that did come up keep polling; nothing is
                // handed to them, and the pools are never freed.
                return Err(unsafe { *q.boot_err.get() }.unwrap_or(Error::NoDevice));
            }
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
            feed: AtomicPtr::new(ptr::null_mut()),
            signal: AtomicU32::new(0),
            waiting: AtomicBool::new(false),
        };

        let i = self.next.fetch_add(1, Ordering::Relaxed) % self.queues.len();
        let queue = &self.queues[i];
        queue.push(&slot as *const Slot as *mut Slot);

        // Read the body out of the feed as it arrives, if the worker gives one, until the worker is done.
        let (buf_ptr, buf_cap) = (buf.as_mut_ptr(), buf.len());
        let mut reader: Option<Reader> = None;
        let mut read_all = false;
        loop {
            slot.signal.store(0, Ordering::Relaxed);
            slot.waiting.store(true, Ordering::Relaxed);
            fence(Ordering::SeqCst);
            if slot.state.load(Ordering::Acquire) != PENDING {
                slot.waiting.store(false, Ordering::Relaxed);
                break;
            }
            let feed = unsafe { slot.feed.load(Ordering::Acquire).as_ref() };
            // All read: the worker lets go of the slot right after it says it is finished, so wait here rather
            // than park, and stay on this cpu with the body in its cache.
            if read_all && feed.is_some_and(|f| f.slot_done()) {
                slot.waiting.store(false, Ordering::Relaxed);
                break;
            }
            if read_all {
                slot.waiting.store(false, Ordering::Relaxed);
                core::hint::spin_loop();
                continue;
            }
            let news = feed.is_some_and(|f| f.has_news());
            if !news {
                unsafe { shim_thread_park(slot.signal.as_ptr() as *const u32) };
            }
            slot.waiting.store(false, Ordering::Relaxed);
            if read_all {
                continue;
            }
            let Some(f) = (unsafe { slot.feed.load(Ordering::Acquire).as_ref() }) else { continue };
            let r = reader.get_or_insert_with(|| Reader::new(f, buf_ptr, buf_cap));
            let mut wake = r.drain();
            if f.finished() {
                r.drain();
                r.finish();
                read_all = true;
                wake = true;
            }
            if wake {
                queue.wake_worker();
            }
        }
        if read_all {
            let (written, failed, overflowed) = reader.as_ref().expect("read").result();
            let t1 = unsafe { shim_time_ns() };
            stats::get_finished(
                t1.saturating_sub(t0),
                t1.saturating_sub(slot.published_ns.load(Ordering::Relaxed)),
                t1,
            );
            return match unsafe { (*slot.outcome.get()).take() }.unwrap_or(Err(Error::BadResponse)) {
                Ok(_) if overflowed => Err(Error::BufferTooSmall),
                Ok(_) if failed => Err(Error::Tls),
                Ok(mut g) => {
                    g.written = written;
                    Ok(g)
                }
                Err(e) => Err(e),
            };
        }
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
            0,
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

struct Pending {
    slot: *mut Slot,
    reused: bool,
    retried: bool,
    picked_ns: u64,
    /// The slot's, kept here: with a feed the slot may be gone before the stats are taken.
    submitted_ns: u64,
    /// Framed: the worker is done, and has this for the slot (no feed), or waits for the reader (a feed).
    result: Option<Result<GetResult, Error>>,
    framed: bool,
    failed: bool,
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
        ttfb_ns: None,
    })
}

fn serve(
    handle: WorkerHandle,
    peer: Endpoint,
    conns: usize,
    rx_buffer: usize,
    tx_buffer: usize,
    resolve: bool,
    idle_ns: u64,
    worker_copies: bool,
    queue: Arc<Queue>,
) {
    let queue_id = handle.queue_id();
    let mut cfg = WorkerConfig::new(peer);
    cfg.conns = conns;
    cfg.rx_buffer = rx_buffer;
    cfg.tx_buffer = tx_buffer;
    cfg.resolve = resolve;

    let mut w = match Worker::new(handle, &cfg) {
        Ok(w) => w,
        Err(e) => {
            println!("FAIL: q{}: {:?}", queue_id, e);
            // Written before the flag; Service::start reads it after.
            unsafe { *queue.boot_err.get() = Some(e) };
            queue.ready.store(DOWN, Ordering::Release);
            return;
        }
    };
    let waiter = if idle_ns != 0 { unsafe { shim_waiter_new() } } else { ptr::null_mut() };
    queue.waiter.store(waiter, Ordering::Release);
    queue.ready.store(UP, Ordering::Release);
    let mut idle_since_ns = unsafe { shim_time_ns() };

    let mut pending: Vec<Option<Pending>> = (0..w.slots()).map(|_| None).collect();
    // A feed per connection: its requester reads the body out of it.
    let feeds: Vec<Box<Feed>> = if worker_copies { Vec::new() } else { (0..w.slots()).map(|_| Feed::new()).collect() };
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
        let mut progress = false;

        for slot in 0..w.slots() {
            let feed = feeds.get(slot).map(|f| &**f);
            let feed_ptr = feed.map_or(ptr::null(), |f| f as *const Feed);
            let Some(pd) = pending[slot].as_mut() else {
                let Some(p) = queue.pop() else { continue };
                progress = true;
                let s = unsafe { &*p };
                if let Some(f) = feed {
                    f.reset();
                }
                let reused = w.idle_reusable(slot);
                let opened = if reused {
                    w.reuse(slot, head_of(s), sink_of(s), feed_ptr)
                } else {
                    // A kept socket the peer since closed sits in CloseWait;
                    // connect() needs Closed.
                    w.release(slot);
                    w.connect_next(slot, head_of(s), sink_of(s), feed_ptr)
                };
                match opened {
                    Ok(()) => {
                        s.feed.store(feed_ptr as *mut Feed, Ordering::Release);
                        pending[slot] =
                            Some(Pending {
                                slot: p,
                                reused,
                                retried: false,
                                picked_ns: now_ns,
                                submitted_ns: s.submitted_ns,
                                result: None,
                                framed: false,
                                failed: false,
                            });
                    }
                    Err(e) => unsafe { Slot::complete(p, Err(e)) },
                }
                continue;
            };

            if !pd.framed {
                let Some(step) = w.conn(slot).and_then(|c| c.outcome()) else {
                    if feed.is_some_and(|f| f.worth_waking()) {
                        unsafe { Slot::notify(pd.slot) };
                    }
                    continue;
                };
                progress = true;
                let res = match step {
                    crate::Step::Complete => result_of(w.conn_mut(slot).expect("just observed")),
                    crate::Step::Failed(e) => Err(e),
                    crate::Step::Pending => unreachable!("outcome() is terminal"),
                };
                if matches!(step, crate::Step::Failed(Error::SynTimeout)) {
                    w.dial_failed();
                }
                // A reused socket the peer had already closed: re-dial once. GET
                // and HEAD are safe to repeat, while nothing of the answer went out.
                let untouched = feed.map_or(true, |f| f.produced() == 0)
                    && w.conn(slot).map_or(true, |c| c.sink_written() == 0);
                if pd.reused && !pd.retried && matches!(step, crate::Step::Failed(_)) && untouched {
                    let s = unsafe { &*pd.slot };
                    w.release(slot);
                    if let Some(f) = feed {
                        f.reset();
                    }
                    if w.connect_next(slot, head_of(s), sink_of(s), feed_ptr).is_ok() {
                        stats::request_retried();
                        pd.reused = false;
                        pd.retried = true;
                        continue;
                    }
                }
                // Past the retry, so this one is a real failure: either a fresh
                // connection died, or the re-dial did not save it.
                if let crate::Step::Failed(e) = step {
                    println!(
                        "FAIL: q{} request failed{}: {:?}",
                        queue_id,
                        if pd.retried { " after re-dial" } else { "" },
                        e
                    );
                }
                let mut res = res;
                if let Ok(g) = res.as_mut() {
                    let head_ns = w.conn(slot).and_then(|c| c.head_ns());
                    g.ttfb_ns = head_ns.map(|h| (h + epoch_ns).saturating_sub(pd.picked_ns));
                }
                pd.framed = true;
                pd.failed = res.is_err();
                pd.result = Some(res);
                // The reader still has the body to read; the slot is its from here on.
                if let Some(f) = feed {
                    if f.produced() == 0 {
                        // The worker wrote whatever body there was.
                        f.set_base(w.conn(slot).map_or(0, |c| c.sink_written()));
                    }
                    let ok = !pd.failed;
                    unsafe { Slot::hand_over(pd.slot, pd.result.take().expect("just set")) };
                    f.finish(if ok { DONE } else { FAILED });
                    unsafe { Slot::notify(pd.slot) };
                    f.release_slot();
                    continue;
                }
            }

            // Framed: with a feed, done once the reader has read it all.
            let mut written = None;
            if let Some(f) = feed {
                let Some((n, failed, overflowed)) = f.reader_result() else { continue };
                progress = true;
                written = Some(n);
                if !pd.failed && failed {
                    println!("FAIL: q{} body did not decrypt", queue_id);
                }
                pd.failed |= failed || overflowed;
            }
            let pd = pending[slot].take().expect("this slot's");
            // Frames the reader is through with, and anything past the body, go back before the feed is reused.
            w.settle(slot);
            let head_ns = w.conn(slot).and_then(|c| c.head_ns()).map(|h| h + epoch_ns);
            let written = written.unwrap_or_else(|| pd.result.as_ref().map_or(0, |r| r.as_ref().map_or(0, |g| g.written)));
            stats::request_finished(
                pd.picked_ns.saturating_sub(pd.submitted_ns),
                now_ns.saturating_sub(pd.picked_ns),
                written,
                head_ns.map(|h| h.saturating_sub(pd.picked_ns)),
                head_ns.map(|h| now_ns.saturating_sub(h)),
            );
            if let Some(res) = pd.result {
                unsafe { Slot::complete(pd.slot, res) };
            }
            if pd.failed || !w.idle_reusable(slot) {
                w.release(slot);
            }
        }

        if idle_ns == 0 {
            continue;
        }
        let (rxp, txp) = w.last_dev();
        let reclaimable = feeds.iter().any(|f| f.has_reclaimable());
        if progress || rxp != 0 || txp != 0 || w.last_active() || w.rx_buffered() || reclaimable {
            idle_since_ns = now_ns;
        } else if now_ns.saturating_sub(idle_since_ns) >= idle_ns {
            sleep_until_work(&mut w, &queue, &feeds, waiter, queue_id);
            idle_since_ns = unsafe { shim_time_ns() };
        }
    }
}

/// The longest an idle worker sleeps without being woken: smoltcp's timers and the address refresh are seconds.
const MAX_SLEEP_NS: u64 = 10_000_000;

/// Arm the queue's interrupt and sleep until a packet, a request or smoltcp's next timer. The worker polls once
/// more after arming: what arrived before the interrupt was armed raised none.
fn sleep_until_work(w: &mut Worker, queue: &Queue, feeds: &[Box<Feed>], waiter: *mut c_void, queue_id: u16) {
    let port = w.port();
    queue.sleeping.store(true, Ordering::Relaxed);
    fence(Ordering::SeqCst);
    if unsafe { shim_rx_intr_arm(port, queue_id, waiter) } != 0 {
        // The device cannot interrupt: stay a poller.
        queue.sleeping.store(false, Ordering::Relaxed);
        return;
    }
    w.poll();
    let (rxp, _) = w.last_dev();
    if rxp == 0 && !w.rx_buffered() && !queue.maybe_pending() && !feeds.iter().any(|f| f.has_reclaimable()) {
        let timeout = w.next_timer_ns().unwrap_or(MAX_SLEEP_NS).min(MAX_SLEEP_NS);
        let t0 = unsafe { shim_time_ns() };
        let signalled = unsafe { shim_waiter_wait(waiter, timeout) } != 0;
        stats::worker_slept(unsafe { shim_time_ns() }.saturating_sub(t0), signalled);
    }
    unsafe { shim_rx_intr_disarm(port, queue_id) };
    queue.sleeping.store(false, Ordering::Relaxed);
}
