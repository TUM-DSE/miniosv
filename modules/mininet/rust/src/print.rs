//! Console output: no `std`, no libc stdio, just OSv's `write(2)`.

use core::fmt::{self, Write};
use core::sync::atomic::{AtomicBool, Ordering};

use crate::ffi::write;

/// Formats into a fixed buffer so a line reaches the console in one write.
pub struct BufWriter<'a> {
    buf: &'a mut [u8],
    used: usize,
}

pub const LINE_MAX: usize = 256;

impl<'a> BufWriter<'a> {
    pub fn new(buf: &'a mut [u8]) -> Self {
        Self { buf, used: 0 }
    }

    pub fn used(&self) -> usize {
        self.used
    }
}

impl Write for BufWriter<'_> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let n = core::cmp::min(s.len(), self.buf.len() - self.used);
        self.buf[self.used..self.used + n].copy_from_slice(&s.as_bytes()[..n]);
        self.used += n;
        Ok(()) // silently truncate rather than fail a diagnostic print
    }
}

/// Serialises console writers; printing happens off the data path.
static PRINT_LOCK: AtomicBool = AtomicBool::new(false);

pub fn print_line(args: fmt::Arguments) {
    let mut buf = [0u8; LINE_MAX];
    let mut used = {
        let mut w = BufWriter::new(&mut buf);
        let _ = w.write_fmt(args);
        w.used
    };
    if used == LINE_MAX {
        used -= 1; // make room for the newline on a truncated line
    }
    buf[used] = b'\n';
    used += 1;

    while PRINT_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
    let mut off = 0;
    while off < used {
        let n = unsafe { write(1, buf[off..].as_ptr(), used - off) };
        if n <= 0 {
            break;
        }
        off += n as usize;
    }
    PRINT_LOCK.store(false, Ordering::Release);
}

/// `println!` without `std`, on the same lock as the stack's own output.
#[macro_export]
macro_rules! println {
    () => { $crate::print::print_line(format_args!("")) };
    ($($arg:tt)*) => { $crate::print::print_line(format_args!($($arg)*)) };
}
