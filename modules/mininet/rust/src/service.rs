//! Requests submitted from other threads, answered by the workers.
//!
//! [`Worker`] on its own is driven by whoever owns it: open connections, poll
//! until they finish, read the results off. That suits a benchmark, which
//! knows every request before it starts. It does not suit DuckDB, which
//! discovers the range it wants inside `FileSystem::Read` on an arbitrary
//! thread and cannot proceed until the bytes are there.
//!
//! So a [`Service`] puts each worker on its own thread, polling forever, and
//! gives everyone else a blocking [`Service::get`]. `FileSystem` has no async
//! read anywhere in its interface, so concurrency comes from DuckDB's own
//! threads: N threads in a blocking read is the shape the engine already has,
//! and the ceiling is `workers * conns_per_worker` requests in flight.

use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};

use crate::endpoint::{Endpoint, Request};
use crate::error::Error;
use crate::ffi::{shim_thread_current, shim_thread_park, shim_thread_unpark};
use crate::http::{BufferSink, ContentRange};
use crate::thread;
use crate::worker::{Worker, WorkerConfig, WorkerHandle};
use crate::Stack;

/// What a completed request delivered.
///
/// Not `Copy`: the header values are owned strings. httpfs compares ETags to
/// notice an object changing under an open handle, so they have to survive the
/// connection they arrived on.
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

/// One request in flight: what the worker needs, and where the answer goes.
///
/// It lives on the submitting thread's stack. [`Service::get`] blocks until
/// the worker is finished with it, so it cannot go away underneath, and
/// nothing here needs reference counting or an allocation per request.
///
/// The two-step handshake exists because of one narrow race. The worker has to
/// wake the submitter, which means dereferencing a thread handle that the
/// submitter published. If the worker's store of the result were the only
/// signal, the submitter could wake, return, and let its thread exit before
/// the worker's `unpark` ran -- waking a thread that no longer exists. So the
/// worker marks `PUBLISHED`, wakes, and only then marks `RELEASED`, and the
/// submitter does not return until it sees `RELEASED`. The wait for that is a
/// handful of instructions on an already-woken thread.
struct Slot {
    head: *const u8,
    head_len: usize,
    buf: *mut u8,
    buf_cap: usize,
    discard_ciphertext: bool,
    /// The submitter's thread, for the worker to wake.
    waiter: *mut c_void,
    state: AtomicU32,
    /// Written by the worker before `state` leaves `PENDING`, read by the
    /// submitter after. The `state` ordering is what makes that safe.
    outcome: UnsafeCell<Option<Result<GetResult, Error>>>,
}

impl Slot {
    /// Publish `res` and hand the slot back to the submitter.
    ///
    /// # Safety
    ///
    /// `p` must be a slot this worker took off the queue and has not yet
    /// released.
    unsafe fn complete(p: *mut Slot, res: Result<GetResult, Error>) {
        let s = unsafe { &*p };
        // Copy the handle out first: after the PUBLISHED store the submitter
        // may already be running, and everything in the slot is racing.
        let waiter = s.waiter;
        unsafe { *s.outcome.get() = Some(res) };
        s.state.store(PUBLISHED, Ordering::Release);
        if !waiter.is_null() {
            unsafe { shim_thread_unpark(waiter) };
        }
        s.state.store(RELEASED, Ordering::Release);
    }
}

/// Multi-producer, single-consumer handoff to one worker.
///
/// A spinlock rather than something cleverer: a push happens once per request,
/// not once per packet, so this is nowhere near the data path. The worker's
/// `pop` is the only consumer.
struct Queue {
    lock: AtomicBool,
    items: UnsafeCell<VecDeque<*mut Slot>>,
}

// SAFETY: every access to `items` is under `lock`.
unsafe impl Send for Queue {}
unsafe impl Sync for Queue {}

impl Queue {
    fn new() -> Self {
        Self {
            lock: AtomicBool::new(false),
            items: UnsafeCell::new(VecDeque::new()),
        }
    }

    fn acquire(&self) {
        while self
            .lock
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
    }

    fn release(&self) {
        self.lock.store(false, Ordering::Release);
    }

    fn push(&self, slot: *mut Slot) {
        self.acquire();
        unsafe { (*self.items.get()).push_back(slot) };
        self.release();
    }

    fn pop(&self) -> Option<*mut Slot> {
        self.acquire();
        let out = unsafe { (*self.items.get()).pop_front() };
        self.release();
        out
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

impl Service {
    /// Spawn a worker per queue, each pinned to the matching CPU.
    ///
    /// The threads are never joined: they poll for the life of the program,
    /// the way `modules/miniext` never unmounts in practice.
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
                Some(q as usize),
            );
            queues.push(queue);
        }
        Ok(Service {
            queues,
            next: AtomicUsize::new(0),
        })
    }

    /// Send `head` and write the response body into `buf`. Blocks.
    ///
    /// `head` is the complete request head; see [`Request`]. The calling
    /// thread parks rather than spins, so it does not compete with the worker
    /// for the CPU while it waits.
    pub fn get(&self, head: &[u8], buf: &mut [u8]) -> Result<GetResult, Error> {
        self.get_with(head, buf, false)
    }

    /// As [`Service::get`], with the record-layer diagnostic from [`Request`].
    pub fn get_with(
        &self,
        head: &[u8],
        buf: &mut [u8],
        discard_ciphertext: bool,
    ) -> Result<GetResult, Error> {
        if self.queues.is_empty() {
            return Err(Error::NoDevice);
        }
        let slot = Slot {
            head: head.as_ptr(),
            head_len: head.len(),
            buf: buf.as_mut_ptr(),
            buf_cap: buf.len(),
            discard_ciphertext,
            waiter: unsafe { shim_thread_current() },
            state: AtomicU32::new(PENDING),
            outcome: UnsafeCell::new(None),
        };

        // Round-robin: a worker owns its queue outright, so any of them can
        // carry any request, and spreading them keeps one worker from being
        // the whole service's depth.
        let i = self.next.fetch_add(1, Ordering::Relaxed) % self.queues.len();
        self.queues[i].push(&slot as *const Slot as *mut Slot);

        unsafe { shim_thread_park(slot.state.as_ptr() as *const u32) };
        while slot.state.load(Ordering::Acquire) != RELEASED {
            core::hint::spin_loop();
        }

        // SAFETY: the worker wrote this before the release store above, and is
        // finished with the slot. `take` rather than a read: GetResult owns
        // strings now, so the value has to be moved out, not copied.
        unsafe { (*slot.outcome.get()).take() }.unwrap_or(Err(Error::BadResponse))
    }
}

/// The worker thread body: poll, harvest, refill, forever.
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
            // Nothing can be served from this queue. Fail every request that
            // arrives rather than leaving submitters parked forever.
            println!("FAIL: q{}: {}", queue_id, e);
            loop {
                if let Some(p) = queue.pop() {
                    unsafe { Slot::complete(p, Err(e)) };
                }
                core::hint::spin_loop();
            }
        }
    };

    // The slot, whether its attempt went out on a reused socket, and whether
    // it has already been retried once.
    let mut pending: Vec<Option<(*mut Slot, bool, bool)>> = Vec::new();
    pending.resize(w.slots(), None);

    loop {
        w.poll();

        for slot in 0..w.slots() {
            match pending[slot] {
                Some((p, was_reused, retried)) => {
                    let done = w.conn(slot).and_then(|c| c.outcome());
                    if let Some(step) = done {
                        let res = match step {
                            crate::Step::Complete => {
                                let c = w.conn(slot).expect("just observed");
                                if c.sink_overflowed() {
                                    Err(Error::BufferTooSmall)
                                } else {
                                    Ok(GetResult {
                                        status: c.status(),
                                        written: c.sink_written(),
                                        content_length: c.head().content_length,
                                        content_range: c.head().content_range,
                                        etag: c.head().etag.clone(),
                                        last_modified: c.head().last_modified.clone(),
                                    })
                                }
                            }
                            crate::Step::Failed(e) => Err(e),
                            crate::Step::Pending => unreachable!("outcome() is terminal"),
                        };
                        // A keep-alive connection the peer closed while it was
                        // idle stays Established until its FIN arrives, so a
                        // request can go out on a socket that is already gone
                        // and come back as a failure. That is a race, not an
                        // answer: dial again and re-send. Once only, and only
                        // for a socket we reused -- a fresh connection failing
                        // this way is a real fault -- and only because every
                        // request here is a GET or a HEAD, which are safe to
                        // repeat.
                        if was_reused && !retried && matches!(step, crate::Step::Failed(_)) {
                            let s = unsafe { &*p };
                            let head =
                                unsafe { core::slice::from_raw_parts(s.head, s.head_len) };
                            let req = Request {
                                head,
                                discard_ciphertext: s.discard_ciphertext,
                            };
                            w.release(slot);
                            if w.connect_next(slot, &req).is_ok() {
                                let sink = unsafe { BufferSink::new(s.buf, s.buf_cap) };
                                if let Some(c) = w.conn_mut(slot) {
                                    c.set_sink(alloc::boxed::Box::new(sink));
                                }
                                pending[slot] = Some((p, false, true));
                                continue;
                            }
                            // Could not re-dial: report the original failure
                            // rather than inventing one about the retry.
                        }
                        unsafe { Slot::complete(p, res) };
                        pending[slot] = None;
                        // A connection the peer hasn't closed stays open for
                        // the next request on this slot instead of paying for
                        // a fresh handshake; one that failed, or that the peer
                        // already closed, still frees its port.
                        if !w.idle_reusable(slot) {
                            w.release(slot);
                        }
                    }
                }
                None => {
                    let p = match queue.pop() {
                        Some(p) => p,
                        None => continue,
                    };
                    // SAFETY: the submitter is parked and the slot is its
                    // stack frame, which it cannot leave until we release it.
                    let s = unsafe { &*p };
                    let head = unsafe { core::slice::from_raw_parts(s.head, s.head_len) };
                    let req = Request {
                        head,
                        discard_ciphertext: s.discard_ciphertext,
                    };
                    let reusing = w.idle_reusable(slot);
                    let opened = if reusing {
                        w.reuse(slot, &req)
                    } else {
                        // The socket can still be open here. It was reusable
                        // when the last request finished, so it was kept
                        // rather than released -- and the peer closed it in
                        // the meantime, leaving it in CloseWait. connect()
                        // refuses anything that is not Closed, so hand the
                        // socket back before dialling on it again.
                        w.release(slot);
                        w.connect_next(slot, &req)
                    };
                    match opened {
                        Ok(()) => {
                            let sink = unsafe { BufferSink::new(s.buf, s.buf_cap) };
                            if let Some(c) = w.conn_mut(slot) {
                                c.set_sink(alloc::boxed::Box::new(sink));
                            }
                            pending[slot] = Some((p, reusing, false));
                        }
                        Err(e) => unsafe { Slot::complete(p, Err(e)) },
                    }
                }
            }
        }
    }
}
