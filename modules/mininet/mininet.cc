/*
 * mininet.hh over the Rust crate's C ABI (rust/src/capi.rs).
 *
 * Nothing but naming: the crate exports flat `mininet_*` symbols because that
 * is what `#[no_mangle]` can promise, and callers want a namespace and
 * references. The structs are laid out identically on both sides -- capi.rs
 * declares them #[repr(C)] against this file -- so they are passed through,
 * not converted.
 */

#include "mininet.hh"

#include <cstddef>

extern "C" {

struct mininet_config_abi {
    const char *host;
    const char *address;
    int tls;
    uint32_t workers;
    uint32_t conns_per_worker;
    uint64_t rx_buffer;
};

struct mininet_response_abi {
    uint32_t status;
    uint64_t content_length;
    uint64_t bytes;
    uint32_t has_range;
    uint64_t range_first;
    uint64_t range_last;
    uint64_t range_total;
    char etag[128];
    char last_modified[64];
};

struct mininet_conn_stats_abi {
    uint64_t requests_served;
    uint64_t requests_reused;
};

int mininet_up(const mininet_config_abi *cfg);
int mininet_is_up(void);
const char *mininet_host(void);
int mininet_get(const char *head, uint64_t head_len, void *buf, uint64_t cap,
                mininet_response_abi *out);
const char *mininet_strerror(int rc);
mininet_conn_stats_abi mininet_conn_stats(void);
}

static_assert(sizeof(mininet::config) == sizeof(mininet_config_abi),
              "mininet::config must match capi.rs");
static_assert(sizeof(mininet::response) == sizeof(mininet_response_abi),
              "mininet::response must match capi.rs");
static_assert(offsetof(mininet::response, etag) == offsetof(mininet_response_abi, etag),
              "mininet::response field order must match capi.rs");
static_assert(offsetof(mininet::response, last_modified) ==
                  offsetof(mininet_response_abi, last_modified),
              "mininet::response field order must match capi.rs");

namespace mininet {

int up(const config &c)
{
	return mininet_up(reinterpret_cast<const mininet_config_abi *>(&c));
}

bool is_up()
{
	return mininet_is_up() != 0;
}

const char *host()
{
	return mininet_host();
}

int get(const char *head, size_t head_len, void *buf, size_t cap, response *out)
{
	return mininet_get(head, static_cast<uint64_t>(head_len), buf,
	                   static_cast<uint64_t>(cap),
	                   reinterpret_cast<mininet_response_abi *>(out));
}

const char *strerror(int rc)
{
	return mininet_strerror(rc);
}

conn_stats stats()
{
	auto s = mininet_conn_stats();
	return conn_stats{s.requests_served, s.requests_reused};
}

} // namespace mininet
