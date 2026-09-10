//! Rust's global allocator, routed to OSv's C `malloc` through the shim.
//! rustls, rustcrypto, webpki and every `Arc<T>` here need a heap, and there is
//! no `std` to supply one.

use core::alloc::{GlobalAlloc, Layout};

use crate::ffi::{shim_free, shim_malloc, shim_realloc};

struct ShimAllocator;

/// `malloc` gives 16-byte alignment. A caller that wants more gets an
/// over-sized block with the raw pointer stashed in the word just before the
/// aligned slot, so `dealloc` can recover what to free.
unsafe impl GlobalAlloc for ShimAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() <= 16 {
            return unsafe { shim_malloc(layout.size() as u64) };
        }
        let extra = layout.align() + core::mem::size_of::<*mut u8>();
        let raw = unsafe { shim_malloc((layout.size() + extra) as u64) };
        if raw.is_null() {
            return raw;
        }
        let raw_addr = raw as usize + core::mem::size_of::<*mut u8>();
        let aligned = (raw_addr + layout.align() - 1) & !(layout.align() - 1);
        unsafe {
            *((aligned - core::mem::size_of::<*mut u8>()) as *mut *mut u8) = raw;
        }
        aligned as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.align() <= 16 {
            unsafe { shim_free(ptr) };
        } else {
            unsafe {
                let slot = (ptr as usize - core::mem::size_of::<*mut u8>()) as *mut *mut u8;
                shim_free(*slot);
            }
        }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= 16 {
            return unsafe { shim_realloc(ptr, new_size as u64) };
        }
        let new_layout = unsafe { Layout::from_size_align_unchecked(new_size, layout.align()) };
        let new_ptr = unsafe { self.alloc(new_layout) };
        if !new_ptr.is_null() {
            let copy = core::cmp::min(layout.size(), new_size);
            unsafe { core::ptr::copy_nonoverlapping(ptr, new_ptr, copy) };
            unsafe { self.dealloc(ptr, layout) };
        }
        new_ptr
    }
}

#[global_allocator]
static GLOBAL: ShimAllocator = ShimAllocator;
