//! A real monotonic clock, so smoltcp's retransmit timers do not drift.

use crate::ffi::shim_time_ns;

pub struct MonoClock {
    epoch_ns: u64,
}

impl MonoClock {
    pub fn new() -> Self {
        Self {
            epoch_ns: unsafe { shim_time_ns() },
        }
    }

    pub fn epoch_ns(&self) -> u64 {
        self.epoch_ns
    }

    pub fn elapsed_ns(&self) -> u64 {
        unsafe { shim_time_ns() }.saturating_sub(self.epoch_ns)
    }

    pub fn elapsed_ms(&self) -> i64 {
        (self.elapsed_ns() / 1_000_000) as i64
    }
}

impl Default for MonoClock {
    fn default() -> Self {
        Self::new()
    }
}

/// Poll-iteration budget for the setup phase.
pub const ITER_BUDGET: u64 = 20_000_000_000;
