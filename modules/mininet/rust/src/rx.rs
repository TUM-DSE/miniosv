//! Received bytes that stay where the NIC put them. smoltcp hands an in-order segment to [`take_direct`] instead of
//! copying it into the socket's ring (vendor/smoltcp); the connection then holds it, in order with whatever did go
//! through the ring, as a run of a frame, until whoever reads the response is done with it.

use alloc::collections::VecDeque;
use alloc::rc::Rc;
use core::cell::Cell;
use core::ffi::c_void;
use core::ptr;

use crate::ffi::{shim_mbuf_free, shim_mbuf_ref};

/// The frame smoltcp is processing, while it came from the NIC.
#[derive(Clone, Copy)]
pub(crate) struct RxFrame {
    pub(crate) handle: *mut c_void,
    pub(crate) base: *const u8,
    pub(crate) len: usize,
}

impl RxFrame {
    pub(crate) const NONE: RxFrame = RxFrame { handle: ptr::null_mut(), base: ptr::null(), len: 0 };
}

/// What keeps a run of received bytes alive: a frame, or a copy out of smoltcp's ring.
#[derive(Clone, Copy)]
pub(crate) enum Owner {
    Frame(*mut c_void),
    Copy(*const [u8]),
}

impl Owner {
    /// One more holder. Worker thread only, like [`Owner::release`].
    pub(crate) fn retain(self) {
        match self {
            Owner::Frame(h) => unsafe { shim_mbuf_ref(h) },
            Owner::Copy(p) => unsafe { Rc::increment_strong_count(p) },
        }
    }

    pub(crate) fn release(self) {
        match self {
            Owner::Frame(h) => unsafe { shim_mbuf_free(h) },
            Owner::Copy(p) => drop(unsafe { Rc::from_raw(p) }),
        }
    }
}

/// A run of received bytes. `held`: smoltcp took it directly, so its window counts it until released.
#[derive(Clone, Copy)]
pub(crate) struct Run {
    pub(crate) owner: Owner,
    pub(crate) ptr: *const u8,
    pub(crate) len: usize,
    pub(crate) held: bool,
}

pub(crate) struct RxStream {
    runs: VecDeque<Run>,
    /// Bytes of the front run already used.
    front_off: usize,
    bytes: usize,
    frame: *const Cell<RxFrame>,
    /// Held bytes used up here (heads, record headers): the window can open by as much.
    pub(crate) unheld: usize,
}

impl RxStream {
    /// `frame`: the device's current frame, or null where no NIC is (the selftest's loopback).
    pub(crate) fn new(frame: *const Cell<RxFrame>) -> Self {
        Self { runs: VecDeque::new(), front_off: 0, bytes: 0, frame, unheld: 0 }
    }

    pub(crate) fn len(&self) -> usize {
        self.bytes
    }

    /// What smoltcp's ring held, after what came before it.
    pub(crate) fn push_copy(&mut self, data: &[u8]) {
        if data.is_empty() {
            return;
        }
        let rc: Rc<[u8]> = Rc::from(data);
        let ptr = rc.as_ptr();
        self.runs.push_back(Run { owner: Owner::Copy(Rc::into_raw(rc)), ptr, len: data.len(), held: false });
        self.bytes += data.len();
    }

    /// The front run's unused bytes.
    pub(crate) fn front(&self) -> Option<&[u8]> {
        self.runs
            .front()
            .map(|r| unsafe { core::slice::from_raw_parts(r.ptr.add(self.front_off), r.len - self.front_off) })
    }

    /// The first `out.len()` bytes, left in place; false if fewer are there.
    pub(crate) fn peek(&self, out: &mut [u8]) -> bool {
        if self.bytes < out.len() {
            return false;
        }
        let mut done = 0;
        let mut off = self.front_off;
        for r in &self.runs {
            let n = (r.len - off).min(out.len() - done);
            unsafe { ptr::copy_nonoverlapping(r.ptr.add(off), out.as_mut_ptr().add(done), n) };
            done += n;
            off = 0;
            if done == out.len() {
                break;
            }
        }
        true
    }

    /// Use up `n` bytes here.
    pub(crate) fn skip(&mut self, mut n: usize) {
        debug_assert!(n <= self.bytes);
        while n > 0 {
            let Some(r) = self.runs.front() else { break };
            let k = (r.len - self.front_off).min(n);
            if r.held {
                self.unheld += k;
            }
            self.advance(k);
            n -= k;
        }
    }

    /// Copy the next `out.len()` bytes out and use them up; false, and nothing used, if fewer are there.
    pub(crate) fn take_copy(&mut self, out: &mut [u8]) -> bool {
        if !self.peek(out) {
            return false;
        }
        self.skip(out.len());
        true
    }

    /// The next run of at most `max` bytes, handed on with its own hold on its owner.
    pub(crate) fn cut(&mut self, max: usize) -> Option<Run> {
        let r = *self.runs.front()?;
        let k = (r.len - self.front_off).min(max);
        if k == 0 {
            return None;
        }
        let out = Run { owner: r.owner, ptr: unsafe { r.ptr.add(self.front_off) }, len: k, held: r.held };
        if self.front_off + k < r.len {
            r.owner.retain();
            self.front_off += k;
        } else {
            // The run's own hold goes with the cut.
            self.runs.pop_front();
            self.front_off = 0;
        }
        self.bytes -= k;
        Some(out)
    }

    fn advance(&mut self, k: usize) {
        let r = *self.runs.front().expect("advance past the end");
        self.bytes -= k;
        if self.front_off + k < r.len {
            self.front_off += k;
        } else {
            self.runs.pop_front();
            self.front_off = 0;
            r.owner.release();
        }
    }

    /// Let go of everything, for a connection that is done or gone.
    pub(crate) fn clear(&mut self) {
        let n = self.bytes;
        self.skip(n);
    }
}

impl Drop for RxStream {
    fn drop(&mut self) {
        self.clear();
    }
}

/// Smaller segments go through smoltcp's ring: a 9 KiB frame held for a 1500-byte one is memory the pool
/// does not have.
const DIRECT_MIN: usize = 4096;

/// smoltcp's [`smoltcp::socket::tcp::RxDirect`]: keep the payload in its frame. `ctx` is the connection's stream.
pub(crate) fn take_direct(ctx: usize, payload: &[u8]) -> bool {
    let rx = unsafe { &mut *(ctx as *mut RxStream) };
    if rx.frame.is_null() || payload.len() < DIRECT_MIN {
        return false;
    }
    let f = unsafe { (*rx.frame).get() };
    let p = payload.as_ptr();
    if f.handle.is_null() || p < f.base || p.wrapping_add(payload.len()) > f.base.wrapping_add(f.len) {
        return false;
    }
    unsafe { shim_mbuf_ref(f.handle) };
    rx.runs.push_back(Run { owner: Owner::Frame(f.handle), ptr: p, len: payload.len(), held: true });
    rx.bytes += payload.len();
    true
}
