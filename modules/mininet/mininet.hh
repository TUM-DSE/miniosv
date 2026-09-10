/*
 * mininet: a userspace network stack for miniOSv.
 *
 * No sockets, no file descriptors, no libc networking -- mininet polls the
 * NIC queues itself, the way modules/miniext drives NVMe itself. Implemented
 * in Rust (modules/mininet/rust): smoltcp for TCP/IP, rustls for TLS, over
 * minidpdk. This header is the whole C++ surface.
 *
 * One endpoint per image. A worker owns an RSS queue outright, and the source
 * ports it may use are a function of the *peer's* address -- that is what
 * lets it poll one queue with nothing shared and nothing locked. Serving a
 * second host would mean a second set of workers; not implemented.
 *
 * There is no resolver: up() takes the address the caller already knows.
 */

#ifndef MININET_HH
#define MININET_HH

#include <cstddef>
#include <cstdint>

namespace mininet {

// Return codes. Negative on failure, so `rc < 0` reads like an errno test --
// though these are not errnos; none of them has a sensible POSIX spelling.
// strerror() turns one into a string.
enum : int {
    OK                = 0,
    E_NO_DEVICE       = -1,   // no usable NIC: absent, or configure/start refused
    E_NO_MEMORY       = -2,
    E_RSS             = -3,   // steering model unreadable; see the note above
    E_DHCP            = -4,
    E_ARP             = -5,
    E_NO_PORTS        = -6,
    E_CONNECT         = -7,
    E_SYN_TIMEOUT     = -8,
    E_TLS             = -9,
    E_BAD_RESPONSE    = -10,  // no status line, endless head, or chunked
    E_BUFFER_TOO_SMALL = -11, // body larger than the buffer; nothing overran
    E_NOT_UP          = -12,
    E_BAD_ARGUMENT    = -13,
};

struct config {
    //! `Host:` header and TLS server name. Must be the name the certificate is
    //! issued for, even though `address` is what gets dialled.
    const char *host;
    //! Dotted quad, e.g. "3.5.216.240".
    const char *address;
    //! 0 dials plain HTTP on port 80, which isolates the network stack from
    //! the record layer.
    int tls;
    //! RSS queues, and so worker threads, to ask for. Clamped to what the
    //! device advertises -- ENA caps queue count per instance size.
    uint32_t workers;
    //! Connection slots per worker. The concurrency ceiling is
    //! `workers * conns_per_worker` requests in flight; past that, callers
    //! queue.
    uint32_t conns_per_worker;
    //! Per-socket receive buffer, or 0 for the default. This dominates the
    //! stack's memory: workers * conns_per_worker * rx_buffer.
    uint64_t rx_buffer;
};

//! Header values are fixed arrays, not pointers, so a response crosses by
//! value and there is nothing to free. An ETag longer than this is truncated;
//! callers compare ETags for equality and a truncated one simply fails to
//! match, which is the safe direction to be wrong in.
enum : size_t {
    ETAG_MAX = 128,
    DATE_MAX = 64,
};

struct response {
    //! HTTP status, or 0 if no head was read.
    uint32_t status;
    //! What the head said the body was; 0 when it said nothing.
    uint64_t content_length;
    //! Bytes written into the caller's buffer.
    uint64_t bytes;
    //! Content-Range, when there was one. The three numbers mean nothing when
    //! this is 0.
    uint32_t has_range;
    uint64_t range_first;
    uint64_t range_last;
    //! 0 when the server sent `*` for the total.
    uint64_t range_total;
    //! NUL-terminated; empty when the header was absent.
    char etag[ETAG_MAX];
    char last_modified[DATE_MAX];
};

//! Configure and start the NIC, take a DHCP lease, resolve the gateway, and
//! spawn one worker thread per queue pinned to its own CPU. Call once, at
//! startup; calling again while up is a no-op.
//!
//! The workers poll without yielding, so they want CPUs to themselves: leave
//! `workers` below the core count and tell the application about the rest.
int up(const config &c);

bool is_up();

//! The host the stack was brought up for, or nullptr when it is not up.
//!
//! One endpoint per image, so a caller that wants a different host has to
//! refuse rather than silently fetch from this one. See the note at the top.
const char *host();

//! Send `head` and write the response body into `buf`.
//!
//! `head` is the complete request head -- request line, headers, blank line --
//! rendered by the caller; mininet carries HTTP, it does not build it.
//!
//! Blocks. The calling thread is parked, not spun, so it does not compete for
//! the CPU a worker is using. Any thread may call this, and many may at once.
//!
//! A body larger than `cap` is E_BUFFER_TOO_SMALL: the excess is dropped
//! rather than written past the end, so this is reported as the failure it
//! is rather than as a short read.
int get(const char *head, size_t head_len, void *buf, size_t cap, response *out);

//! Never null.
const char *strerror(int rc);

struct conn_stats {
    //! Requests served, and how many of those reused a connection the peer
    //! hadn't closed instead of paying for a fresh handshake.
    uint64_t requests_served;
    uint64_t requests_reused;
};

conn_stats stats();

} // namespace mininet

#endif /* MININET_HH */
