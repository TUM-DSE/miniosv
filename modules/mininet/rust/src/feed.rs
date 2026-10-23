//! One response body on its way to the thread that asked for it. The worker publishes pieces of frames as they
//! arrive; the reader copies them into its buffer, or decrypts them there, and says how far it got; the worker then
//! frees those frames and opens the window by as much. The reader is the only one to touch the body's bytes.

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicU8, Ordering};

use ring::aead::LessSafeKey;

use crate::record::{self, Secret};
use crate::rx::{Owner, Run};

/// Plain body bytes.
pub(crate) const BODY: u8 = 0;
/// The first piece of a record's payload; `rec_len` and `seq` are its.
pub(crate) const REC_START: u8 = 1;
pub(crate) const REC_MORE: u8 = 2;

pub(crate) const RUNNING: u8 = 0;
pub(crate) const DONE: u8 = 1;
pub(crate) const FAILED: u8 = 2;

/// Pieces in flight; a 4 MiB window is at most ~1500 of them.
const CAP: u32 = 4096;
/// The reader is woken once this much more is waiting, and at the end.
const WAKE_READER_BYTES: u64 = 256 << 10;
/// The worker, if asleep, once the reader is done with this much more.
pub(crate) const WAKE_WORKER_BYTES: u64 = 1 << 20;

#[derive(Clone, Copy)]
pub(crate) struct Piece {
    pub(crate) run: Run,
    pub(crate) kind: u8,
    pub(crate) rec_len: u32,
    pub(crate) seq: u64,
}

const NO_PIECE: Piece = Piece {
    run: Run { owner: Owner::Frame(core::ptr::null_mut()), ptr: core::ptr::null(), len: 0, held: false },
    kind: BODY,
    rec_len: 0,
    seq: 0,
};

/// A line of its own, so the worker's and the reader's counters do not share one.
#[repr(align(64))]
struct Line<T>(T);

impl<T> core::ops::Deref for Line<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.0
    }
}

pub(crate) struct Feed {
    pieces: Box<[UnsafeCell<Piece>]>,
    // The worker's.
    produced: Line<AtomicU32>,
    produced_bytes: Line<AtomicU64>,
    // The reader's.
    consumed: Line<AtomicU32>,
    consumed_bytes: Line<AtomicU64>,
    /// Body bytes the worker wrote itself, ahead of every piece.
    base: AtomicU64,
    secret: UnsafeCell<Option<Secret>>,
    state: AtomicU8,
    /// The worker has let go of the reader's slot: past this the reader may return.
    slot_done: AtomicBool,
    reader_done: AtomicBool,
    reader_written: AtomicU64,
    reader_failed: AtomicBool,
    reader_overflowed: AtomicBool,
    // The worker's alone.
    reclaimed: UnsafeCell<u32>,
    signalled_bytes: UnsafeCell<u64>,
}

unsafe impl Send for Feed {}
unsafe impl Sync for Feed {}

impl Feed {
    pub(crate) fn new() -> Box<Feed> {
        Box::new(Feed {
            pieces: (0..CAP).map(|_| UnsafeCell::new(NO_PIECE)).collect(),
            produced: Line(AtomicU32::new(0)),
            produced_bytes: Line(AtomicU64::new(0)),
            consumed: Line(AtomicU32::new(0)),
            consumed_bytes: Line(AtomicU64::new(0)),
            base: AtomicU64::new(0),
            secret: UnsafeCell::new(None),
            state: AtomicU8::new(RUNNING),
            slot_done: AtomicBool::new(false),
            reader_done: AtomicBool::new(false),
            reader_written: AtomicU64::new(0),
            reader_failed: AtomicBool::new(false),
            reader_overflowed: AtomicBool::new(false),
            reclaimed: UnsafeCell::new(0),
            signalled_bytes: UnsafeCell::new(0),
        })
    }

    // Worker side.

    /// For the next request; no reader may still hold it.
    pub(crate) fn reset(&self) {
        debug_assert!(!self.unreclaimed());
        self.produced.store(0, Ordering::Relaxed);
        self.produced_bytes.store(0, Ordering::Relaxed);
        self.consumed.store(0, Ordering::Relaxed);
        self.consumed_bytes.store(0, Ordering::Relaxed);
        self.base.store(0, Ordering::Relaxed);
        unsafe { *self.secret.get() = None };
        self.state.store(RUNNING, Ordering::Relaxed);
        self.slot_done.store(false, Ordering::Relaxed);
        self.reader_done.store(false, Ordering::Relaxed);
        self.reader_written.store(0, Ordering::Relaxed);
        self.reader_failed.store(false, Ordering::Relaxed);
        self.reader_overflowed.store(false, Ordering::Relaxed);
        unsafe {
            *self.reclaimed.get() = 0;
            *self.signalled_bytes.get() = 0;
        }
        // Published by whatever hands the slot to the reader.
    }

    pub(crate) fn has_room(&self) -> bool {
        self.produced.load(Ordering::Relaxed).wrapping_sub(unsafe { *self.reclaimed.get() }) < CAP
    }

    /// After [`Feed::has_room`].
    pub(crate) fn push(&self, p: Piece) {
        let i = self.produced.load(Ordering::Relaxed);
        unsafe { *self.pieces[(i % CAP) as usize].get() = p };
        self.produced_bytes.store(self.produced_bytes.load(Ordering::Relaxed) + p.run.len as u64, Ordering::Relaxed);
        self.produced.store(i.wrapping_add(1), Ordering::Release);
    }

    /// Before any piece: the reader starts writing here.
    pub(crate) fn set_base(&self, n: u64) {
        self.base.store(n, Ordering::Relaxed);
    }

    /// Before the first record's piece.
    pub(crate) fn set_secret(&self, s: Secret) {
        unsafe { *self.secret.get() = Some(s) };
    }

    pub(crate) fn finish(&self, state: u8) {
        self.state.store(state, Ordering::Release);
    }

    /// The worker's last touch of the reader's slot is behind it.
    pub(crate) fn release_slot(&self) {
        self.slot_done.store(true, Ordering::Release);
    }

    pub(crate) fn slot_done(&self) -> bool {
        self.slot_done.load(Ordering::Acquire)
    }

    /// The last piece is out; everything pushed before is visible.
    pub(crate) fn finished(&self) -> bool {
        self.state.load(Ordering::Acquire) != RUNNING
    }

    pub(crate) fn produced(&self) -> u32 {
        self.produced.load(Ordering::Relaxed)
    }

    /// Pieces the reader is through with that are still held, or a reader done that left some.
    pub(crate) fn has_reclaimable(&self) -> bool {
        let r = unsafe { *self.reclaimed.get() };
        self.consumed.load(Ordering::Acquire) != r || (self.reader_done.load(Ordering::Acquire) && self.unreclaimed())
    }

    /// Whether the reader has enough waiting to be worth waking; true once per `WAKE_READER_BYTES`, and at the end.
    pub(crate) fn worth_waking(&self) -> bool {
        let p = self.produced_bytes.load(Ordering::Relaxed);
        unsafe {
            if self.finished() || p - *self.signalled_bytes.get() >= WAKE_READER_BYTES {
                *self.signalled_bytes.get() = p;
                return true;
            }
        }
        false
    }

    /// Let go of the pieces the reader is done with, or of all of them once it has given up; the held bytes freed.
    pub(crate) fn reclaim(&self) -> usize {
        let upto = if self.reader_done.load(Ordering::Acquire) {
            self.produced.load(Ordering::Relaxed)
        } else {
            self.consumed.load(Ordering::Acquire)
        };
        let r = unsafe { &mut *self.reclaimed.get() };
        let mut held = 0;
        while *r != upto {
            let p = unsafe { *self.pieces[(*r % CAP) as usize].get() };
            if p.run.held {
                held += p.run.len;
            }
            p.run.owner.release();
            *r = r.wrapping_add(1);
        }
        held
    }

    pub(crate) fn unreclaimed(&self) -> bool {
        unsafe { *self.reclaimed.get() != self.produced.load(Ordering::Relaxed) }
    }

    /// The reader has stopped: what it wrote, and how it ended.
    pub(crate) fn reader_result(&self) -> Option<(u64, bool, bool)> {
        self.reader_done.load(Ordering::Acquire).then(|| {
            (
                self.reader_written.load(Ordering::Relaxed),
                self.reader_failed.load(Ordering::Relaxed),
                self.reader_overflowed.load(Ordering::Relaxed),
            )
        })
    }

    // Reader side.

    /// Worth reading: a wake's worth waiting, or the end. Reading less at a time would chase every frame.
    pub(crate) fn has_news(&self) -> bool {
        self.finished()
            || self.produced_bytes.load(Ordering::Relaxed) - self.consumed_bytes.load(Ordering::Relaxed)
                >= WAKE_READER_BYTES
    }
}

struct Rec {
    start: usize,
    len: usize,
    fill: usize,
    seq: u64,
    in_scratch: bool,
}

/// The reading side of a [`Feed`], on the thread whose buffer the body fills.
pub(crate) struct Reader<'a> {
    feed: &'a Feed,
    buf: *mut u8,
    cap: usize,
    off: usize,
    started: bool,
    failed: bool,
    overflowed: bool,
    key: Option<(LessSafeKey, [u8; 12])>,
    rec: Option<Rec>,
    scratch: Vec<u8>,
    woke_worker_at: u64,
}

impl<'a> Reader<'a> {
    pub(crate) fn new(feed: &'a Feed, buf: *mut u8, cap: usize) -> Self {
        Self {
            feed,
            buf,
            cap,
            off: 0,
            started: false,
            failed: false,
            overflowed: false,
            key: None,
            rec: None,
            scratch: Vec::new(),
            woke_worker_at: 0,
        }
    }

    /// Read everything published so far. True if the worker should be woken to free what was read.
    pub(crate) fn drain(&mut self) -> bool {
        let f = self.feed;
        let produced = f.produced.load(Ordering::Acquire);
        let mut c = f.consumed.load(Ordering::Relaxed);
        if c == produced {
            return false;
        }
        if !self.started {
            self.started = true;
            self.off = f.base.load(Ordering::Relaxed) as usize;
        }
        let mut bytes = 0u64;
        while c != produced {
            let p = unsafe { *f.pieces[(c % CAP) as usize].get() };
            self.take(&p);
            bytes += p.run.len as u64;
            c = c.wrapping_add(1);
        }
        f.consumed.store(c, Ordering::Release);
        let total = f.consumed_bytes.fetch_add(bytes, Ordering::Relaxed) + bytes;
        if total - self.woke_worker_at >= WAKE_WORKER_BYTES {
            self.woke_worker_at = total;
            return true;
        }
        false
    }

    /// What was written, whether a record failed, and whether the body overflowed the buffer.
    pub(crate) fn result(&self) -> (u64, bool, bool) {
        (self.off as u64, self.failed || self.rec.is_some(), self.overflowed)
    }

    /// Done reading: the result goes to the worker, which may then reuse the feed.
    pub(crate) fn finish(&mut self) {
        let f = self.feed;
        if !self.started {
            self.off = f.base.load(Ordering::Relaxed) as usize;
        }
        // A record cut short of its length is a response cut short.
        let failed = self.failed || self.rec.is_some();
        f.reader_written.store(self.off as u64, Ordering::Relaxed);
        f.reader_failed.store(failed, Ordering::Relaxed);
        f.reader_overflowed.store(self.overflowed, Ordering::Relaxed);
        f.reader_done.store(true, Ordering::Release);
    }

    fn take(&mut self, p: &Piece) {
        if self.failed {
            return;
        }
        let src = unsafe { core::slice::from_raw_parts(p.run.ptr, p.run.len) };
        match p.kind {
            BODY => self.write(src),
            REC_START => {
                if self.key.is_none() {
                    match unsafe { (*self.feed.secret.get()).as_ref() } {
                        Some(s) => self.key = Some((s.key(), s.iv)),
                        None => {
                            self.failed = true;
                            return;
                        }
                    }
                }
                let len = p.rec_len as usize;
                let in_scratch = self.off + len > self.cap;
                if in_scratch && self.scratch.len() < len {
                    self.scratch.resize(record::MAX_CIPHERTEXT, 0);
                }
                self.rec = Some(Rec { start: self.off, len, fill: 0, seq: p.seq, in_scratch });
                self.append(src);
            }
            _ => self.append(src),
        }
    }

    fn write(&mut self, src: &[u8]) {
        let n = src.len().min(self.cap - self.off);
        unsafe { core::ptr::copy_nonoverlapping(src.as_ptr(), self.buf.add(self.off), n) };
        self.off += n;
        if n < src.len() {
            self.overflowed = true;
        }
    }

    fn append(&mut self, src: &[u8]) {
        let Some(r) = self.rec.as_mut() else {
            self.failed = true;
            return;
        };
        if r.fill + src.len() > r.len {
            self.failed = true;
            return;
        }
        let dst = if r.in_scratch { self.scratch.as_mut_ptr() } else { unsafe { self.buf.add(r.start) } };
        unsafe { core::ptr::copy_nonoverlapping(src.as_ptr(), dst.add(r.fill), src.len()) };
        r.fill += src.len();
        if r.fill < r.len {
            return;
        }
        let r = self.rec.take().expect("just used");
        let (key, iv) = self.key.as_ref().expect("set at the record's start");
        let region = unsafe { core::slice::from_raw_parts_mut(dst, r.len) };
        match record::open(key, iv, r.seq, region) {
            Some((content, record::APPLICATION_DATA)) => {
                if r.in_scratch {
                    let n = content.len();
                    let content = unsafe { core::slice::from_raw_parts(content.as_ptr(), n) };
                    self.write(content);
                } else {
                    self.off = r.start + content.len();
                }
            }
            // A session ticket the server sent late: not body.
            Some((_, record::HANDSHAKE)) => {}
            _ => self.failed = true,
        }
    }
}
