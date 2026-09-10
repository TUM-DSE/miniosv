/*
 * mininet network-stack tests.
 *
 * Two halves, both runnable on an image with no NIC:
 *
 *   - the Rust stack's own unit tests (parser, body sink, completion rule),
 *     reached through mininet_selftest(). They live next to the code they
 *     check because that code is Rust and `pub(crate)`; this suite is the
 *     driver and the reporting.
 *   - the C++ surface: the ABI structs mininet.hh and capi.rs both describe,
 *     and the argument validation mininet_get() is supposed to do before it
 *     touches any of them.
 *
 * The point of running in the guest rather than on a host is the allocator.
 * The bug this suite was written for is a write through a raw pointer landing
 * outside its allocation (a page fault inside BufferSink::write at sf=10), and
 * a host-side mock heap is exactly what would hide it.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "modules/mininet/mininet.hh"

extern "C" {
// The Rust side's unit tests: returns how many checks failed.
uint32_t mininet_selftest(int verbose);
// Declared here rather than in mininet.hh because they are the raw ABI, which
// the header deliberately does not expose.
int mininet_get(const char *head, uint64_t head_len, void *buf, uint64_t cap, void *out);
int mininet_is_up(void);
const char *mininet_strerror(int rc);
}

namespace {

int failures;

void check(bool ok, const char *what)
{
	if (!ok) {
		failures++;
		printf("  FAIL %s\n", what);
	}
}

// mininet_get() must reject bad arguments before dereferencing anything, and
// must say "not up" ahead of everything else -- these run on an image with no
// NIC, so the stack is never up and that is the expected answer.
void test_get_argument_validation()
{
	printf("-- mininet_get argument validation\n");

	check(mininet_is_up() == 0, "stack is not up without a NIC");

	char buf[16];
	mininet::response r {};

	// Not up is checked first, so even a null head reports E_NOT_UP rather
	// than crashing on the pointer.
	check(mininet_get(nullptr, 0, buf, sizeof(buf), &r) == mininet::E_NOT_UP,
	      "null head on a down stack reports not-up, does not fault");
	check(mininet_get("GET / HTTP/1.1\r\n\r\n", 18, nullptr, 0, &r) == mininet::E_NOT_UP,
	      "HEAD-shaped call on a down stack reports not-up");
	check(mininet_get("GET / HTTP/1.1\r\n\r\n", 18, nullptr, 64, &r) == mininet::E_NOT_UP,
	      "null buffer with nonzero cap on a down stack reports not-up");
}

// Every error code has to map to a distinct, non-empty string: strerror() is
// how a failure reaches the benchmark CSV, and two codes sharing "unknown
// error" would make two different bugs look like one.
void test_strerror()
{
	printf("-- strerror\n");

	const int codes[] = {
		mininet::OK,          mininet::E_NO_DEVICE,   mininet::E_NO_MEMORY,
		mininet::E_RSS,       mininet::E_DHCP,        mininet::E_ARP,
		mininet::E_NO_PORTS,  mininet::E_CONNECT,     mininet::E_SYN_TIMEOUT,
		mininet::E_TLS,       mininet::E_BAD_RESPONSE, mininet::E_BUFFER_TOO_SMALL,
		mininet::E_NOT_UP,    mininet::E_BAD_ARGUMENT,
	};
	const size_t n = sizeof(codes) / sizeof(codes[0]);

	for (size_t i = 0; i < n; i++) {
		const char *s = mininet::strerror(codes[i]);
		check(s != nullptr && s[0] != '\0', "every code has a message");
		for (size_t j = i + 1; j < n; j++) {
			if (s && strcmp(s, mininet::strerror(codes[j])) == 0) {
				printf("  FAIL codes %d and %d share \"%s\"\n",
				       codes[i], codes[j], s);
				failures++;
			}
		}
	}
	check(strcmp(mininet::strerror(12345), "unknown error") == 0,
	      "an unknown code is reported as unknown");
}

// The response struct crosses the FFI by value, so its layout has to match
// capi.rs. The kernel already static_asserts sizeof and two offsets; this
// checks the thing those asserts cannot -- that the fixed char arrays are
// NUL-terminated and sized as the header promises.
void test_response_abi()
{
	printf("-- response ABI\n");

	check(mininet::ETAG_MAX == 128, "ETAG_MAX matches capi.rs");
	check(mininet::DATE_MAX == 64, "DATE_MAX matches capi.rs");
	check(sizeof(mininet::response {}.etag) == mininet::ETAG_MAX, "etag is ETAG_MAX bytes");
	check(sizeof(mininet::response {}.last_modified) == mininet::DATE_MAX,
	      "last_modified is DATE_MAX bytes");

	// A zero-initialised response must read as "no header", not as garbage:
	// ToResponse() in the httpfs client tests etag[0] for exactly this.
	mininet::response r {};
	check(r.etag[0] == '\0', "a zeroed response has an empty etag");
	check(r.last_modified[0] == '\0', "a zeroed response has an empty last_modified");
	check(r.status == 0 && r.bytes == 0 && r.has_range == 0,
	      "a zeroed response has no status, bytes or range");
}

// conn_stats is read once per run and printed straight into the benchmark's
// output, so a field that silently reads as garbage would become a number in
// a results table. Without a stack up, every counter must be a clean zero.
void test_stats_zeroed()
{
	printf("-- conn_stats\n");

	mininet::conn_stats c = mininet::stats();
	check(c.requests_served == 0, "no requests served before the stack is up");
	check(c.requests_reused == 0, "no requests reused before the stack is up");
	// reused can never exceed served, at any point in a run.
	check(c.requests_reused <= c.requests_served, "reused never exceeds served");
}

} // namespace

int os_mininet_main()
{
	printf("---- mininet network stack ----\n");
	failures = 0;

	// The Rust unit tests first: if the parser or the body sink is broken,
	// everything above them is untrustworthy anyway.
	uint32_t rust_failures = mininet_selftest(0);

	test_get_argument_validation();
	test_strerror();
	test_response_abi();
	test_stats_zeroed();

	int total = static_cast<int>(rust_failures) + failures;
	printf("---- mininet: %s (%u rust, %d c++ failures) ----\n",
	       total ? "FAILURE" : "ok", rust_failures, failures);
	return total ? 1 : 0;
}
