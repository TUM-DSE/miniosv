// mininet: a userspace network stack (smoltcp + rustls over minidpdk); one endpoint per image.

#ifndef MININET_HH
#define MININET_HH

#include <cstddef>
#include <cstdint>

namespace mininet {

// Return codes; strerror() names them.
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
    const char *host;
    const char *address;
    //! 0 dials plain HTTP on port 80.
    int tls;
    //! At least one, at most what the NIC grants.
    uint32_t workers;
    //! `workers * conns_per_worker` is the in-flight ceiling.
    uint32_t conns_per_worker;
};

enum : size_t {
    ETAG_MAX = 128,
    DATE_MAX = 64,
};

struct response {
    uint32_t status;
    uint64_t content_length;
    uint64_t bytes;
    uint32_t has_range;
    uint64_t range_first;
    uint64_t range_last;
    //! 0 when the server sent `*` for the total.
    uint64_t range_total;
    char etag[ETAG_MAX];
    char last_modified[DATE_MAX];
};

//! Call once; a second call is a no-op.
int up(const config &c);

const char *host();

//! Blocks, parked. A body larger than `cap` is E_BUFFER_TOO_SMALL.
int get(const char *head, size_t head_len, void *buf, size_t cap, response *out);

//! Never null.
const char *strerror(int rc);

//! One measured duration, in microseconds.
struct dist {
    uint64_t n, us_avg, us_p50, us_p90, us_max;
};

//! Cumulative since up(). Mirrors stats::Stats in rust/src/stats.rs field for
//! field; that struct is the ABI.
struct conn_stats {
    uint64_t misrouted_drops;
    uint64_t tx_alloc_fail;
    uint64_t tx_burst_fail;
    uint64_t conns_established;
    uint64_t conns_failed;
    dist setup;
    dist dial;
    uint64_t requests_served;
    uint64_t requests_reused;
    uint64_t requests_retried;
    uint64_t requests_done;
    uint64_t queue_ns_total;
    uint64_t wire_ns_total;
    uint64_t body_bytes;
    uint64_t ttfb_ns_total;
    uint64_t ttfb_n;
    uint64_t xfer_ns_total;
    uint64_t xfer_n;
    uint64_t poll_iters;
    uint64_t poll_gap_ns_total;
    uint64_t poll_gap_ns_max;
    uint64_t poll_gaps_over_1ms;
    uint64_t poll_busy_ns;
    uint64_t poll_active_iters;
    uint64_t poll_work_ns;
    uint64_t poll_busy_ns_max;
    uint64_t poll_loop_ns_max;
    uint64_t iface_ns_max;
    uint64_t steps_ns_max;
    uint64_t rx_pkts_max;
    uint64_t tx_pkts_max;
    uint64_t wake_n;
    uint64_t wake_ns_total;
    uint64_t wake_ns_max;
    uint64_t get_calls;
    uint64_t get_ns_total;
    uint64_t active_ns_total;
    uint64_t nic_ipackets;
    uint64_t nic_ibytes;
    uint64_t nic_imissed;
    uint64_t nic_ierrors;
    uint64_t nic_rx_nombuf;
};

conn_stats stats();

} // namespace mininet

#endif /* MININET_HH */
