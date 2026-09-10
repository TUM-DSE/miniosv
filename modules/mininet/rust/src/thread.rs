//! OSv threads, for callers that want one worker per CPU.
//!
//! A worker polls its queue without ever yielding, so it wants a CPU to
//! itself. Pinning is the whole point of this being here rather than left to
//! the caller: two workers time-slicing one core is worse than one worker.

use alloc::boxed::Box;
use core::ffi::{c_int, c_void};

use crate::ffi::{shim_thread_join, shim_thread_spawn};

/// A running thread. Must be joined; dropping one without joining leaks the
/// OSv thread object.
pub struct JoinHandle(*mut c_void);

// SAFETY: the handle is an opaque OSv thread pointer, only ever passed back to
// the shim.
unsafe impl Send for JoinHandle {}

/// The closure is boxed and handed across the FFI boundary; the trampoline
/// takes it back and runs it exactly once.
pub fn spawn<F: FnOnce() + Send + 'static>(f: F, cpu: Option<usize>) -> JoinHandle {
    extern "C" fn trampoline(arg: *mut c_void) {
        let f: Box<Box<dyn FnOnce()>> = unsafe { Box::from_raw(arg as *mut Box<dyn FnOnce()>) };
        f();
    }

    let boxed: Box<Box<dyn FnOnce()>> = Box::new(Box::new(f));
    let arg = Box::into_raw(boxed) as *mut c_void;
    let cpu_id = match cpu {
        Some(c) => c as c_int,
        None => -1,
    };
    JoinHandle(unsafe { shim_thread_spawn(trampoline, arg, cpu_id) })
}

impl JoinHandle {
    pub fn join(self) {
        unsafe { shim_thread_join(self.0) };
    }
}
