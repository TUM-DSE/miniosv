//! OSv threads, pinned: a worker polls without yielding and wants a cpu to itself.

use alloc::boxed::Box;
use core::ffi::{c_int, c_void};

use crate::ffi::{shim_thread_join, shim_thread_spawn};

pub struct JoinHandle(*mut c_void);

unsafe impl Send for JoinHandle {}

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
