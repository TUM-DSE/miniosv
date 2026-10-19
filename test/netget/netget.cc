/*
 * netget: one object fetched through mininet by HTTP range GETs.
 *
 *   1. one small range, to see the stack come up and a response come back;
 *   2. NETGET_CHECK_MB consecutive 1 MiB ranges, hashed (FNV-1a) for the host to compare with the object's bytes;
 *   3. NETGET_SECONDS of NETGET_REQ_KB ranges from NETGET_THREADS threads, for throughput;
 *   4. the stack's counters.
 *
 * Boot arguments (run.py --args) override the NETGET_* defaults: workers= conns= threads= req_kb= seconds=
 * check_mb= path= host= tls=. tls=1 dials 443 and checks the server's certificate against host.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <osv/bootargs.hh>
#include <osv/power.hh>

#include "modules/mininet/mininet.hh"

#ifndef NETGET_SERVER
#define NETGET_SERVER "192.168.1.1"
#endif
#ifndef NETGET_PATH
#define NETGET_PATH "/tpch100/customer.parquet"
#endif
#ifndef NETGET_WORKERS
#define NETGET_WORKERS 4
#endif
#ifndef NETGET_CONNS
#define NETGET_CONNS 4
#endif
#ifndef NETGET_THREADS
#define NETGET_THREADS 16
#endif
#ifndef NETGET_REQ_KB
#define NETGET_REQ_KB 8192
#endif
#ifndef NETGET_CHECK_MB
#define NETGET_CHECK_MB 64
#endif
#ifndef NETGET_SECONDS
#define NETGET_SECONDS 10
#endif

static struct {
	std::string path = NETGET_PATH;
	std::string host = NETGET_SERVER;
	int tls = 0;
	unsigned workers = NETGET_WORKERS;
	unsigned conns = NETGET_CONNS;
	unsigned threads = NETGET_THREADS;
	uint64_t req_kb = NETGET_REQ_KB;
	unsigned check_mb = NETGET_CHECK_MB;
	unsigned seconds = NETGET_SECONDS;
} opt;

static void parse_args()
{
	for (auto &w : osv::bootargs_split(osv::bootargs())) {
		auto eq = w.find('=');
		if (eq == std::string::npos)
			continue;
		auto k = w.substr(0, eq), v = w.substr(eq + 1);
		if (k == "path")
			opt.path = v;
		else if (k == "host")
			opt.host = v;
		else if (k == "tls")
			opt.tls = std::stoi(v);
		else if (k == "workers")
			opt.workers = std::stoul(v);
		else if (k == "conns")
			opt.conns = std::stoul(v);
		else if (k == "threads")
			opt.threads = std::stoul(v);
		else if (k == "req_kb")
			opt.req_kb = std::stoull(v);
		else if (k == "check_mb")
			opt.check_mb = std::stoul(v);
		else if (k == "seconds")
			opt.seconds = std::stoul(v);
	}
	printf("netget: %s://%s (%s) ", opt.tls ? "https" : "http", opt.host.c_str(), NETGET_SERVER);
	printf("path %s workers %u conns %u threads %u req_kb %" PRIu64 " check_mb %u seconds %u\n",
	       opt.path.c_str(), opt.workers, opt.conns, opt.threads, opt.req_kb, opt.check_mb, opt.seconds);
}

static int range_get(uint64_t first, uint64_t len, void *buf, mininet::response *r)
{
	char head[512];
	int n = snprintf(head, sizeof(head),
			 "GET %s HTTP/1.1\r\nHost: %s\r\nRange: bytes=%" PRIu64 "-%" PRIu64 "\r\n\r\n",
			 opt.path.c_str(), opt.host.c_str(), first, first + len - 1);
	return mininet::get(head, n, buf, len, r);
}

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static int run()
{
	using clock = std::chrono::steady_clock;

	mininet::config c{};
	c.host = opt.host.c_str();
	c.address = NETGET_SERVER;
	c.tls = opt.tls;
	c.workers = opt.workers;
	c.conns_per_worker = opt.conns;
	auto t0 = clock::now();
	int rc = mininet::up(c);
	printf("netget: up %s in %.0f ms\n", mininet::strerror(rc),
	       std::chrono::duration<double, std::milli>(clock::now() - t0).count());
	if (rc != mininet::OK)
		return 1;

	std::vector<uint8_t> buf(std::max<uint64_t>(opt.req_kb, 1024) << 10);
	mininet::response r{};
	rc = range_get(0, 4096, buf.data(), &r);
	printf("netget: GET 4 KiB -> %s, status %u, %" PRIu64 " bytes, range %" PRIu64 "-%" PRIu64 "/%" PRIu64
	       ", etag %s\n",
	       mininet::strerror(rc), r.status, r.bytes, r.range_first, r.range_last, r.range_total, r.etag);
	if (rc != mininet::OK || r.status != 206 || r.bytes != 4096)
		return 1;
	const uint64_t object = r.range_total;

	uint64_t h = 0xcbf29ce484222325ULL;
	t0 = clock::now();
	for (uint64_t i = 0; i < opt.check_mb; i++) {
		rc = range_get(i << 20, 1 << 20, buf.data(), &r);
		if (rc != mininet::OK || r.bytes != (1 << 20)) {
			printf("netget: check range %" PRIu64 " -> %s, %" PRIu64 " bytes\n", i, mininet::strerror(rc),
			       r.bytes);
			return 1;
		}
		h = fnv1a(h, buf.data(), 1 << 20);
	}
	double s = std::chrono::duration<double>(clock::now() - t0).count();
	printf("netget: check %u MiB fnv1a %016" PRIx64 " in %.2f s\n", opt.check_mb, h, s);

	mininet::clear_stats();
	const uint64_t req = opt.req_kb << 10;
	const uint64_t slots = object / req;
	std::atomic<uint64_t> bytes{0}, reqs{0}, errors{0};
	std::atomic<bool> stop{false};
	std::vector<std::thread> threads;
	std::vector<std::vector<uint32_t>> lat_us(opt.threads);
	t0 = clock::now();
	for (unsigned t = 0; t < opt.threads; t++) {
		threads.emplace_back([&, t] {
			std::vector<uint8_t> b(req);
			mininet::response rr{};
			lat_us[t].reserve(1 << 20);
			for (uint64_t i = t; !stop.load(std::memory_order_relaxed); i += opt.threads) {
				auto a = clock::now();
				if (range_get((i % slots) * req, req, b.data(), &rr) != mininet::OK || rr.bytes != req) {
					errors++;
					continue;
				}
				lat_us[t].push_back(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - a).count());
				bytes += rr.bytes;
				reqs++;
			}
		});
	}
	std::this_thread::sleep_for(std::chrono::seconds(opt.seconds));
	stop = true;
	for (auto &t : threads)
		t.join();
	s = std::chrono::duration<double>(clock::now() - t0).count();
	printf("netget: %u threads x %" PRIu64 " KiB: %" PRIu64 " requests, %" PRIu64 " errors, %.2f GB in %.2f s = %.2f Gbit/s\n",
	       opt.threads, opt.req_kb, reqs.load(), errors.load(), bytes / 1e9, s, bytes * 8 / s / 1e9);

	std::vector<uint32_t> all;
	for (auto &v : lat_us)
		all.insert(all.end(), v.begin(), v.end());
	std::sort(all.begin(), all.end());
	if (!all.empty()) {
		auto q = [&](double p) { return all[std::min(all.size() - 1, (size_t)(all.size() * p))]; };
		printf("netget: request latency (us): p1 %u p10 %u p50 %u p90 %u p99 %u max %u\n", q(.01), q(.1), q(.5),
		       q(.9), q(.99), all.back());
		// A coarse histogram of the tail, in 1 ms bins from the median.
		uint32_t base = q(.5) / 1000 * 1000;
		unsigned bins[8] = {};
		for (auto v : all)
			if (v >= base)
				bins[std::min<uint32_t>(7, (v - base) / 1000)]++;
		printf("netget: from %u ms in 1 ms bins:", base / 1000);
		for (auto b : bins)
			printf(" %u", b);
		printf("\n");
	}

	auto st = mininet::stats();
	printf("netget: conns %" PRIu64 " (+%" PRIu64 " failed), misrouted %" PRIu64 ", tx alloc/burst fail %" PRIu64
	       "/%" PRIu64 ", jumbo rx %" PRIu64 " (max %" PRIu64 " B), nic in %" PRIu64 " pkts missed %" PRIu64
	       " errors %" PRIu64 " nombuf %" PRIu64 "\n",
	       st.conns_established, st.conns_failed, st.misrouted_drops, st.tx_alloc_fail, st.tx_burst_fail,
	       st.rx_jumbo_pkts, st.rx_len_max, st.nic_ipackets, st.nic_imissed, st.nic_ierrors, st.nic_rx_nombuf);
	printf("netget: setup n %" PRIu64 " avg %" PRIu64 " p50 %" PRIu64 " p90 %" PRIu64 " max %" PRIu64
	       " us; dial n %" PRIu64 " avg %" PRIu64 " p50 %" PRIu64 " p90 %" PRIu64 " max %" PRIu64
	       " us; retried %" PRIu64 "\n",
	       st.setup.n, st.setup.us_avg, st.setup.us_p50, st.setup.us_p90, st.setup.us_max, st.dial.n, st.dial.us_avg,
	       st.dial.us_p50, st.dial.us_p90, st.dial.us_max, st.requests_retried);
	auto avg_us = [](uint64_t ns, uint64_t n) { return n ? ns / 1e3 / n : 0.0; };
	printf("netget: per request (us): queue %.0f wire %.0f ttfb %.0f xfer %.0f get %.0f wake %.0f (max %.0f)\n",
	       avg_us(st.queue_ns_total, st.requests_done), avg_us(st.wire_ns_total, st.requests_done),
	       avg_us(st.ttfb_ns_total, st.ttfb_n), avg_us(st.xfer_ns_total, st.xfer_n),
	       avg_us(st.get_ns_total, st.get_calls), avg_us(st.wake_ns_total, st.wake_n), st.wake_ns_max / 1e3);
	printf("netget: poll iters %" PRIu64 " busy %.1f%% gap avg %.1f us max %.0f us >1ms %" PRIu64
	       " loop max %.0f us, rx pkts max %" PRIu64 " tx pkts max %" PRIu64 "\n",
	       st.poll_iters, s > 0 ? 100.0 * st.poll_busy_ns / 1e9 / s / opt.workers : 0.0,
	       avg_us(st.poll_gap_ns_total, st.poll_iters), st.poll_gap_ns_max / 1e3, st.poll_gaps_over_1ms,
	       st.poll_loop_ns_max / 1e3, st.rx_pkts_max, st.tx_pkts_max);
	return errors ? 1 : 0;
}

extern "C" void osv_app_main()
{
	parse_args();
	int rc = run();
	printf("netget: %s\n", rc ? "FAIL" : "PASS");
	osv::poweroff();
}
