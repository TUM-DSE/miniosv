/*
 * memset and memcmp for x86-64.
 *
 * The generic libc is compiled for the architecture's baseline, with no -march
 * naming the machine, so its dispatch picks the SSE2 path and stays there. That
 * is the right choice for memcpy, which reaches memory bandwidth either way,
 * and the wrong one for these two:
 *
 *   memset  16-byte stores top out around 34 GiB/s however wide the machine
 *           is. `rep stosb` on a cpu with ERMS runs at cache-line width and
 *           needs no vector state, so it is available to a kernel that does
 *           not enable AVX.
 *   memcmp  the baseline build has no vector code at all -- a scalar 8-byte
 *           loop with a bswap per word. SSE2 compares 16 bytes per iteration
 *           and finds the differing byte with a mask, which is 3x here.
 *
 * These definitions win over the libc's because the libc is linked as a
 * trailing archive: an archive member is only pulled in for a symbol still
 * undefined, and these are defined by the time it is searched.
 *
 * The file is compiled -fno-builtin, without which the compiler rewrites the
 * loops below into calls to the very functions they implement.
 */

#include <cstddef>
#include <cstdint>

#include "cpuid.hh"

namespace {

// Unaligned, and allowed to alias whatever the caller's buffer really holds.
typedef uint64_t u64u __attribute__((may_alias, aligned(1)));
typedef uint32_t u32u __attribute__((may_alias, aligned(1)));
typedef uint16_t u16u __attribute__((may_alias, aligned(1)));

typedef unsigned char v16 __attribute__((vector_size(16), may_alias, aligned(1)));
typedef char v16i __attribute__((vector_size(16)));

/*
 * Where `rep stosb` starts winning.
 *
 * It has a fixed setup cost of a few tens of cycles, which a short store
 * sequence beats outright; past a few hundred bytes the setup disappears into
 * the copy and its full-line writes pull ahead. Measured against the generic
 * libc on Zen 4, the two cross between 256 and 1024 bytes, so 512 splits them.
 */
constexpr size_t erms_min = 512;

bool have_erms()
{
    // features() reads a cached cpuid result, but not before the cpu is set
    // up; a memset before that takes the store path, which is always correct.
    return processor::features().repmovsb;
}

inline int diff_in_chunk(const unsigned char *a, const unsigned char *b)
{
    const v16 x = *reinterpret_cast<const v16 *>(a);
    const v16 y = *reinterpret_cast<const v16 *>(b);
    const unsigned mask =
        static_cast<unsigned>(__builtin_ia32_pmovmskb128(v16i(x == y)));

    if (mask == 0xffffu) {
        return 0;
    }
    const unsigned i = static_cast<unsigned>(__builtin_ctz(~mask & 0xffffu));
    return int(a[i]) - int(b[i]);
}

inline int cmp_be64(const unsigned char *a, const unsigned char *b)
{
    const uint64_t x = __builtin_bswap64(*reinterpret_cast<const u64u *>(a));
    const uint64_t y = __builtin_bswap64(*reinterpret_cast<const u64u *>(b));
    return x == y ? 0 : (x < y ? -1 : 1);
}

inline int cmp_be32(const unsigned char *a, const unsigned char *b)
{
    const uint32_t x = __builtin_bswap32(*reinterpret_cast<const u32u *>(a));
    const uint32_t y = __builtin_bswap32(*reinterpret_cast<const u32u *>(b));
    return x == y ? 0 : (x < y ? -1 : 1);
}

inline int cmp_be16(const unsigned char *a, const unsigned char *b)
{
    const uint16_t x = __builtin_bswap16(*reinterpret_cast<const u16u *>(a));
    const uint16_t y = __builtin_bswap16(*reinterpret_cast<const u16u *>(b));
    return x == y ? 0 : (x < y ? -1 : 1);
}

} // namespace

extern "C" int memcmp(const void *lhs, const void *rhs, size_t n)
{
    const auto *a = static_cast<const unsigned char *>(lhs);
    const auto *b = static_cast<const unsigned char *>(rhs);

    // Under 64 bytes, loading two xmm registers costs more than it saves
    if (n < 16) {
        if (n >= 8) {
            if (const int d = cmp_be64(a, b)) {
                return d;
            }
            return cmp_be64(a + n - 8, b + n - 8);
        }
        if (n >= 4) {
            if (const int d = cmp_be32(a, b)) {
                return d;
            }
            return cmp_be32(a + n - 4, b + n - 4);
        }
        if (n >= 2) {
            if (const int d = cmp_be16(a, b)) {
                return d;
            }
            return cmp_be16(a + n - 2, b + n - 2);
        }
        return n == 1 ? int(a[0]) - int(b[0]) : 0;
    }
    if (n <= 32) {
        if (const int d = diff_in_chunk(a, b)) {
            return d;
        }
        return diff_in_chunk(a + n - 16, b + n - 16);
    }
    if (n < 64) {
        // Check chunks at 0, 16, n-32 and n-16.
        if (const int d = diff_in_chunk(a, b)) {
            return d;
        }
        if (const int d = diff_in_chunk(a + 16, b + 16)) {
            return d;
        }
        if (const int d = diff_in_chunk(a + n - 32, b + n - 32)) {
            return d;
        }
        return diff_in_chunk(a + n - 16, b + n - 16);
    }

    while (n >= 64) {
        const v16 e0 = *reinterpret_cast<const v16 *>(a) ==
                       *reinterpret_cast<const v16 *>(b);
        const v16 e1 = *reinterpret_cast<const v16 *>(a + 16) ==
                       *reinterpret_cast<const v16 *>(b + 16);
        const v16 e2 = *reinterpret_cast<const v16 *>(a + 32) ==
                       *reinterpret_cast<const v16 *>(b + 32);
        const v16 e3 = *reinterpret_cast<const v16 *>(a + 48) ==
                       *reinterpret_cast<const v16 *>(b + 48);

        if (__builtin_ia32_pmovmskb128(v16i(e0 & e1 & e2 & e3)) !=
            static_cast<int>(0xffff)) {
            for (unsigned off = 0; off < 64; off += 16) {
                if (const int d = diff_in_chunk(a + off, b + off)) {
                    return d;
                }
            }
        }
        a += 64;
        b += 64;
        n -= 64;
    }

    while (n >= 16) {
        if (const int d = diff_in_chunk(a, b)) {
            return d;
        }
        a += 16;
        b += 16;
        n -= 16;
    }
    if (n == 0) {
        return 0;
    }
    return diff_in_chunk(a + n - 16, b + n - 16);
}

extern "C" void *memset(void *dst, int c, size_t n)
{
    auto *p = static_cast<unsigned char *>(dst);
    const auto b = static_cast<unsigned char>(c);

    if (n >= erms_min && have_erms()) {
        asm volatile("rep stosb"
                     : "+D"(p), "+c"(n)
                     : "a"(c)
                     : "memory");
        return dst;
    }

    // Short lengths, by halving: each case writes the two ends, which overlap
    // in the middle rather than needing a loop or a remainder.
    if (n < 8) {
        if (n == 0) {
            return dst;
        }
        const uint32_t w = 0x01010101u * b;
        if (n >= 4) {
            *reinterpret_cast<u32u *>(p) = w;
            *reinterpret_cast<u32u *>(p + n - 4) = w;
        } else if (n >= 2) {
            *reinterpret_cast<u16u *>(p) = static_cast<uint16_t>(w);
            *reinterpret_cast<u16u *>(p + n - 2) = static_cast<uint16_t>(w);
        } else {
            p[0] = b;
        }
        return dst;
    }

    const uint64_t q = 0x0101010101010101ull * b;
    if (n <= 16) {
        *reinterpret_cast<u64u *>(p) = q;
        *reinterpret_cast<u64u *>(p + n - 8) = q;
        return dst;
    }

    const v16 v = v16{} + b;
    if (n <= 32) {
        *reinterpret_cast<v16 *>(p) = v;
        *reinterpret_cast<v16 *>(p + n - 16) = v;
        return dst;
    }
    if (n <= 64) {
        *reinterpret_cast<v16 *>(p) = v;
        *reinterpret_cast<v16 *>(p + 16) = v;
        *reinterpret_cast<v16 *>(p + n - 32) = v;
        *reinterpret_cast<v16 *>(p + n - 16) = v;
        return dst;
    }
    // Up to 256 the halving continues, because a loop here costs more in
    // branches than the stores it saves.
    if (n <= 128) {
        for (unsigned i = 0; i < 64; i += 16) {
            *reinterpret_cast<v16 *>(p + i) = v;
            *reinterpret_cast<v16 *>(p + n - 64 + i) = v;
        }
        return dst;
    }
    if (n <= 256) {
        for (unsigned i = 0; i < 128; i += 16) {
            *reinterpret_cast<v16 *>(p + i) = v;
            *reinterpret_cast<v16 *>(p + n - 128 + i) = v;
        }
        return dst;
    }

    // Longer than 256 and below the rep threshold, or without ERMS: 64 bytes
    // an iteration, with the tail written by overlapping the last 64.
    unsigned char *end = p + n;
    do {
        *reinterpret_cast<v16 *>(p) = v;
        *reinterpret_cast<v16 *>(p + 16) = v;
        *reinterpret_cast<v16 *>(p + 32) = v;
        *reinterpret_cast<v16 *>(p + 48) = v;
        p += 64;
    } while (p + 64 <= end);
    *reinterpret_cast<v16 *>(end - 64) = v;
    *reinterpret_cast<v16 *>(end - 48) = v;
    *reinterpret_cast<v16 *>(end - 32) = v;
    *reinterpret_cast<v16 *>(end - 16) = v;
    return dst;
}
