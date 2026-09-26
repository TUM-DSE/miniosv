/*
 * mininet.hh over the Rust crate's C ABI (rust/src/capi.rs), which lays the
 * structs out identically and takes them as they are.
 */

#include "mininet.hh"

extern "C" {
int mininet_up(const mininet::config *cfg);
const char *mininet_host(void);
int mininet_get(const char *head, uint64_t head_len, void *buf, uint64_t cap, mininet::response *out);
const char *mininet_strerror(int rc);
mininet::conn_stats mininet_conn_stats(void);
}

namespace mininet {

int up(const config &c)
{
	return mininet_up(&c);
}

const char *host()
{
	return mininet_host();
}

int get(const char *head, size_t head_len, void *buf, size_t cap, response *out)
{
	return mininet_get(head, head_len, buf, cap, out);
}

const char *strerror(int rc)
{
	return mininet_strerror(rc);
}

conn_stats stats()
{
	return mininet_conn_stats();
}

} // namespace mininet
