//! Rust's global allocator over OSv's `malloc`, through the shim.

use core::alloc::{GlobalAlloc, Layout};

use crate::ffi::{shim_aligned_alloc, shim_free, shim_malloc, shim_realloc};

struct ShimAllocator;

unsafe impl GlobalAlloc for ShimAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() <= 16 {
            unsafe { shim_malloc(layout.size() as u64) }
        } else {
            unsafe { shim_aligned_alloc(layout.align() as u64, layout.size() as u64) }
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe { shim_free(ptr) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= 16 {
            return unsafe { shim_realloc(ptr, new_size as u64) };
        }
        let new = unsafe { self.alloc(Layout::from_size_align_unchecked(new_size, layout.align())) };
        if !new.is_null() {
            unsafe { core::ptr::copy_nonoverlapping(ptr, new, layout.size().min(new_size)) };
            unsafe { shim_free(ptr) };
        }
        new
    }
}

#[global_allocator]
static GLOBAL: ShimAllocator = ShimAllocator;
