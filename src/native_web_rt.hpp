#pragma once

#include <string>

namespace veloz::native {

namespace web_rt_parts {

inline constexpr const char* kCommonHeader = R"c(
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <sys/stat.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif
#endif

typedef struct tz_web_turi_str {
    uint32_t hdr;
    uint32_t word1;
    const char* ptr;
} tz_web_turi_str;

_Static_assert(sizeof(tz_web_turi_str) == 16, "Turi %text size mismatch");
_Static_assert(offsetof(tz_web_turi_str, word1) == 4, "Turi %text word1 offset mismatch");
_Static_assert(offsetof(tz_web_turi_str, ptr) == 8, "Turi %text ptr offset mismatch");

static inline const char* tz_web_str_view(const void* sptr, size_t* out_len) {
    if (!sptr) {
        if (out_len) *out_len = 0;
        return "";
    }
    const tz_web_turi_str* ts = (const tz_web_turi_str*)sptr;
    size_t n = (size_t)(ts->hdr & 0x7FFFFFFFu);
    if (out_len) *out_len = n;
    if (n == 0u) return "";
    if (n <= 12u) return (const char*)&ts->word1;
    return ts->ptr ? ts->ptr : "";
}

static _Thread_local char tz_web_tls_err[512];

static inline void tz_web_set_err(const void** out_ptr, int64_t* out_len, const char* msg) {
    if (!msg || !*msg) {
        if (out_ptr) *out_ptr = NULL;
        if (out_len) *out_len = 0;
        return;
    }
    size_t n = strlen(msg);
    if (n >= sizeof(tz_web_tls_err)) n = sizeof(tz_web_tls_err) - 1u;
    memcpy(tz_web_tls_err, msg, n);
    tz_web_tls_err[n] = '\0';
    if (out_ptr) *out_ptr = (const void*)tz_web_tls_err;
    if (out_len) *out_len = (int64_t)n;
}

static char* tz_web_dup_cstr(const char* p, size_t n) {
    char* s = (char*)malloc(n + 1u);
    if (!s) return NULL;
    if (n > 0u && p) memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

#ifdef _WIN32
typedef SRWLOCK tz_web_lock_t;
typedef SOCKET tz_web_sock_t;
#define TZ_WEB_BAD_SOCK INVALID_SOCKET
static inline void tz_web_lock_init(tz_web_lock_t* l) { InitializeSRWLock(l); }
static inline void tz_web_lock_destroy(tz_web_lock_t* l) { (void)l; }
static inline void tz_web_lock_acq(tz_web_lock_t* l) { AcquireSRWLockExclusive(l); }
static inline void tz_web_lock_rel(tz_web_lock_t* l) { ReleaseSRWLockExclusive(l); }
static inline void tz_web_sock_close(tz_web_sock_t s) {
    if (s != INVALID_SOCKET) closesocket(s);
}
#else
typedef pthread_mutex_t tz_web_lock_t;
typedef int tz_web_sock_t;
#define TZ_WEB_BAD_SOCK (-1)
static inline void tz_web_lock_init(tz_web_lock_t* l) { pthread_mutex_init(l, NULL); }
static inline void tz_web_lock_destroy(tz_web_lock_t* l) { pthread_mutex_destroy(l); }
static inline void tz_web_lock_acq(tz_web_lock_t* l) { pthread_mutex_lock(l); }
static inline void tz_web_lock_rel(tz_web_lock_t* l) { pthread_mutex_unlock(l); }
static inline void tz_web_sock_close(tz_web_sock_t s) {
    if (s >= 0) close(s);
}
#endif

static int32_t tz_web_net_state = 0;
static void tz_web_net_init(void) {
    if (__atomic_load_n(&tz_web_net_state, __ATOMIC_ACQUIRE) == 2) return;
    int32_t expected = 0;
    if (__atomic_compare_exchange_n(&tz_web_net_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#else
        signal(SIGPIPE, SIG_IGN);
#endif
        __atomic_store_n(&tz_web_net_state, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&tz_web_net_state, __ATOMIC_ACQUIRE) != 2) {
#ifdef _WIN32
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
}

static int tz_web_cpu_count(void) {
#ifdef _WIN32
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = si.dwNumberOfProcessors;
    }
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return (int)n;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return (int)n;
#endif
}
)c";

inline constexpr const char* kCommonSimd = R"c(
static inline unsigned tz_web_ctz32(uint32_t m) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctz(m);
#else
    unsigned idx = 0;
    while ((m & 1u) == 0u) { m >>= 1; idx++; }
    return idx;
#endif
}

static inline const char* tz_web_find_crlfcrlf(const char* buf, size_t len) {
    if (len < 4u) return NULL;
    size_t i = 1u;
#if defined(__AVX2__)
    const __m256i vnl = _mm256_set1_epi8('\n');
    while (i + 32u <= len) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(buf + i));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vnl));
        while (m != 0u) {
            unsigned b = tz_web_ctz32(m);
            size_t pos = i + (size_t)b;
            if (pos + 2u < len && buf[pos - 1u] == '\r' && buf[pos + 1u] == '\r' && buf[pos + 2u] == '\n') {
                return buf + pos - 1u;
            }
            m &= m - 1u;
        }
        i += 32u;
    }
#elif defined(__SSE2__)
    const __m128i vnl = _mm_set1_epi8('\n');
    while (i + 16u <= len) {
        __m128i v = _mm_loadu_si128((const __m128i*)(buf + i));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vnl));
        while (m != 0u) {
            unsigned b = tz_web_ctz32(m);
            size_t pos = i + (size_t)b;
            if (pos + 2u < len && buf[pos - 1u] == '\r' && buf[pos + 1u] == '\r' && buf[pos + 2u] == '\n') {
                return buf + pos - 1u;
            }
            m &= m - 1u;
        }
        i += 16u;
    }
#elif defined(__ARM_NEON)
    const uint8x16_t vnl = vdupq_n_u8('\n');
    while (i + 16u <= len) {
        uint8x16_t v = vld1q_u8((const uint8_t*)(buf + i));
        uint64x2_t eq64 = vreinterpretq_u64_u8(vceqq_u8(v, vnl));
        if ((vgetq_lane_u64(eq64, 0) | vgetq_lane_u64(eq64, 1)) != 0ULL) {
            for (size_t k = 0; k < 16u; ++k) {
                size_t pos = i + k;
                if (buf[pos] == '\n' && pos + 2u < len && buf[pos - 1u] == '\r' &&
                    buf[pos + 1u] == '\r' && buf[pos + 2u] == '\n') {
                    return buf + pos - 1u;
                }
            }
        }
        i += 16u;
    }
#endif
    for (; i + 2u < len; ++i) {
        if (buf[i] == '\n' && buf[i - 1u] == '\r' && buf[i + 1u] == '\r' && buf[i + 2u] == '\n') {
            return buf + i - 1u;
        }
    }
    return NULL;
}

static inline size_t tz_web_first_json_esc(const char* s, size_t n) {
    size_t i = 0;
#if defined(__AVX2__)
    const __m256i vq = _mm256_set1_epi8('"');
    const __m256i vb = _mm256_set1_epi8('\\');
    const __m256i v1f = _mm256_set1_epi8(0x1F);
    const __m256i vz = _mm256_setzero_si256();
    while (i + 32u <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(s + i));
        __m256i ctrl = _mm256_cmpeq_epi8(_mm256_subs_epu8(v, v1f), vz);
        __m256i bad = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, vq), _mm256_cmpeq_epi8(v, vb)), ctrl);
        uint32_t m = (uint32_t)_mm256_movemask_epi8(bad);
        if (m != 0u) return i + (size_t)tz_web_ctz32(m);
        i += 32u;
    }
#elif defined(__SSE2__)
    const __m128i vq = _mm_set1_epi8('"');
    const __m128i vb = _mm_set1_epi8('\\');
    const __m128i v1f = _mm_set1_epi8(0x1F);
    const __m128i vz = _mm_setzero_si128();
    while (i + 16u <= n) {
        __m128i v = _mm_loadu_si128((const __m128i*)(s + i));
        __m128i ctrl = _mm_cmpeq_epi8(_mm_subs_epu8(v, v1f), vz);
        __m128i bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vb)), ctrl);
        uint32_t m = (uint32_t)_mm_movemask_epi8(bad);
        if (m != 0u) return i + (size_t)tz_web_ctz32(m);
        i += 16u;
    }
#elif defined(__ARM_NEON)
    const uint8x16_t vq = vdupq_n_u8('"');
    const uint8x16_t vb = vdupq_n_u8('\\');
    const uint8x16_t vc = vdupq_n_u8(0x20u);
    while (i + 16u <= n) {
        uint8x16_t v = vld1q_u8((const uint8_t*)(s + i));
        uint8x16_t bad = vorrq_u8(vorrq_u8(vceqq_u8(v, vq), vceqq_u8(v, vb)), vcltq_u8(v, vc));
        uint64x2_t bad64 = vreinterpretq_u64_u8(bad);
        if ((vgetq_lane_u64(bad64, 0) | vgetq_lane_u64(bad64, 1)) != 0ULL) {
            for (size_t k = 0; k < 16u; ++k) {
                unsigned char c = (unsigned char)s[i + k];
                if (c == '"' || c == '\\' || c < 0x20u) return i + k;
            }
        }
        i += 16u;
    }
#endif
    for (; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c < 0x20u) return i;
    }
    return n;
}

static inline uint64_t tz_web_hash(const char* p, size_t n) {
    uint64_t h = (uint64_t)n * 0x9e3779b97f4a7c15ULL ^ 0xbf58476d1ce4e5b9ULL;
    size_t i = 0;
    while (i + 8u <= n) {
        uint64_t w;
        memcpy(&w, p + i, 8u);
        h ^= w;
        h *= 0x9e3779b97f4a7c15ULL;
        h ^= h >> 31;
        i += 8u;
    }
    if (i + 4u <= n) {
        uint32_t w4;
        memcpy(&w4, p + i, 4u);
        h ^= (uint64_t)w4;
        h *= 0x9e3779b97f4a7c15ULL;
        h ^= h >> 29;
        i += 4u;
    }
    while (i < n) {
        h ^= (uint64_t)(uint8_t)p[i++];
        h *= 0x9e3779b97f4a7c15ULL;
    }
    return h ^ (h >> 33);
}
)c";

inline constexpr const char* kCommonUtil = R"c(
static inline int tz_web_eq_nocase(const char* a, size_t alen, const char* b, size_t blen) {
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

static inline size_t tz_web_fmt_u64(char* dst, uint64_t v) {
    static const char digits[201] =
        "0001020304050607080910111213141516171819"
        "2021222324252627282930313233343536373839"
        "4041424344454647484950515253545556575859"
        "6061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    char tmp[24];
    size_t pos = sizeof(tmp);
    while (v >= 100u) {
        uint64_t q = v / 100u;
        size_t r = (size_t)(v - q * 100u) * 2u;
        v = q;
        pos -= 2u;
        tmp[pos] = digits[r];
        tmp[pos + 1u] = digits[r + 1u];
    }
    if (v >= 10u) {
        size_t r = (size_t)v * 2u;
        pos -= 2u;
        tmp[pos] = digits[r];
        tmp[pos + 1u] = digits[r + 1u];
    } else {
        pos -= 1u;
        tmp[pos] = (char)('0' + (int)v);
    }
    size_t len = sizeof(tmp) - pos;
    memcpy(dst, tmp + pos, len);
    return len;
}

static inline size_t tz_web_fmt_i64(char* dst, int64_t v) {
    if (v < 0) {
        dst[0] = '-';
        uint64_t uv = (uint64_t)(-(v + 1)) + 1u;
        return 1u + tz_web_fmt_u64(dst + 1, uv);
    }
    return tz_web_fmt_u64(dst, (uint64_t)v);
}

static inline int tz_web_parse_i64(const char* p, size_t n, int64_t* out) {
    if (!p || n == 0u) return 0;
    size_t i = 0;
    int neg = 0;
    if (p[0] == '-') {
        neg = 1;
        i = 1u;
        if (i >= n) return 0;
    } else if (p[0] == '+') {
        i = 1u;
        if (i >= n) return 0;
    }
    size_t digits = n - i;
    if (digits <= 18u) {
        uint64_t acc = 0;
        for (; i < n; ++i) {
            unsigned d = (unsigned)(unsigned char)p[i] - (unsigned)'0';
            if (d > 9u) return 0;
            acc = acc * 10ULL + (uint64_t)d;
        }
        *out = neg ? -(int64_t)acc : (int64_t)acc;
        return 1;
    }
    uint64_t acc = 0;
    uint64_t limit = neg ? ((uint64_t)INT64_MAX + 1ULL) : (uint64_t)INT64_MAX;
    for (; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (c < '0' || c > '9') return 0;
        uint64_t d = (uint64_t)(c - '0');
        if (acc > (limit - d) / 10ULL) return 0;
        acc = acc * 10ULL + d;
    }
    if (neg) {
        if (acc == (uint64_t)INT64_MAX + 1ULL) *out = INT64_MIN;
        else *out = -(int64_t)acc;
    } else {
        *out = (int64_t)acc;
    }
    return 1;
}

typedef struct tz_web_buf {
    char* data;
    size_t len;
    size_t cap;
    size_t base_cap;
} tz_web_buf;

static int tz_web_buf_init(tz_web_buf* b, size_t cap) {
    b->data = (char*)malloc(cap);
    if (!b->data) {
        b->len = 0;
        b->cap = 0;
        b->base_cap = 0;
        return 0;
    }
    b->len = 0;
    b->cap = cap;
    b->base_cap = cap;
    return 1;
}

static void tz_web_buf_free(tz_web_buf* b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int tz_web_buf_reserve(tz_web_buf* b, size_t extra) {
    if (b->len + extra <= b->cap) return 1;
    size_t next = b->cap ? b->cap * 2u : 4096u;
    while (next < b->len + extra) next *= 2u;
    char* p = (char*)realloc(b->data, next);
    if (!p) return 0;
    b->data = p;
    b->cap = next;
    return 1;
}

static inline int tz_web_buf_append(tz_web_buf* b, const void* src, size_t n) {
    if (n == 0u) return 1;
    if (!tz_web_buf_reserve(b, n)) return 0;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 1;
}

static void tz_web_buf_shrink(tz_web_buf* b, size_t max_keep) {
    b->len = 0;
    if (b->cap > max_keep && b->base_cap > 0u) {
        char* p = (char*)realloc(b->data, b->base_cap);
        if (p) {
            b->data = p;
            b->cap = b->base_cap;
        }
    }
}

static inline int tz_web_buf_append_json_str(tz_web_buf* b, const char* s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    size_t clean = (n > 0u && s) ? tz_web_first_json_esc(s, n) : n;
    if (clean == n) {
        if (!tz_web_buf_reserve(b, n + 2u)) return 0;
        char* dst = b->data + b->len;
        dst[0] = '"';
        if (n > 0u && s) memcpy(dst + 1, s, n);
        dst[n + 1u] = '"';
        b->len += n + 2u;
        return 1;
    }
    if (!tz_web_buf_reserve(b, clean + 8u)) return 0;
    b->data[b->len++] = '"';
    if (clean > 0u && s) {
        memcpy(b->data + b->len, s, clean);
        b->len += clean;
    }
    size_t start = clean;
    for (size_t i = clean; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c < 0x20u) {
            if (i > start) {
                if (!tz_web_buf_append(b, s + start, i - start)) return 0;
            }
            if (c == '"') {
                if (!tz_web_buf_append(b, "\\\"", 2u)) return 0;
            } else if (c == '\\') {
                if (!tz_web_buf_append(b, "\\\\", 2u)) return 0;
            } else if (c == '\n') {
                if (!tz_web_buf_append(b, "\\n", 2u)) return 0;
            } else if (c == '\r') {
                if (!tz_web_buf_append(b, "\\r", 2u)) return 0;
            } else if (c == '\t') {
                if (!tz_web_buf_append(b, "\\t", 2u)) return 0;
            } else {
                char esc[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 15u], hex[c & 15u]};
                if (!tz_web_buf_append(b, esc, 6u)) return 0;
            }
            start = i + 1u;
        }
    }
    if (n > start) {
        if (!tz_web_buf_append(b, s + start, n - start)) return 0;
    }
    return tz_web_buf_append(b, "\"", 1u);
}

#ifdef _WIN32
static inline int tz_web_win_sym(HMODULE mod, const char* name, void* dst, size_t sz) {
    FARPROC p = GetProcAddress(mod, name);
    if (!p) return 0;
    memcpy(dst, &p, sz);
    return 1;
}
#else
static inline int tz_web_posix_sym(void* mod, const char* name, void* dst, size_t sz) {
    void* p = dlsym(mod, name);
    if (!p) return 0;
    memcpy(dst, &p, sz);
    return 1;
}
#endif

#define TZ_JSON_200_TMPL "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: "
#define TZ_JSON_200_HDR_LEN 117u
#define TZ_JSON_200_DATE_OFF 70u
_Static_assert(sizeof(TZ_JSON_200_TMPL) - 1u == TZ_JSON_200_HDR_LEN, "json 200 header length mismatch");

static struct {
    int64_t sec;
    uint32_t seq;
    char text[2][32];
    char json_200_hdr[2][128];
} tz_web_date_cache = {
    -1,
    0,
    { "Thu, 01 Jan 1970 00:00:00 GMT", "Thu, 01 Jan 1970 00:00:00 GMT" },
    { TZ_JSON_200_TMPL, TZ_JSON_200_TMPL }
};
static int32_t tz_web_date_updating = 0;

static inline uint32_t tz_web_date_refresh_slot(void) {
    time_t now_t = time(NULL);
    int64_t now = (int64_t)now_t;
    int64_t cached = __atomic_load_n(&tz_web_date_cache.sec, __ATOMIC_RELAXED);
    if (now != cached) {
        int32_t exp = 0;
        if (__atomic_compare_exchange_n(&tz_web_date_updating, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            static const char* kDays[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
            static const char* kMonths[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
            struct tm gmt;
            memset(&gmt, 0, sizeof(gmt));
#ifdef _WIN32
            _gmtime64_s(&gmt, &now_t);
#else
            gmtime_r(&now_t, &gmt);
#endif
            uint32_t next_slot = (__atomic_load_n(&tz_web_date_cache.seq, __ATOMIC_RELAXED) + 1u) & 1u;
            int wday = (gmt.tm_wday >= 0 && gmt.tm_wday < 7) ? gmt.tm_wday : 0;
            int mon = (gmt.tm_mon >= 0 && gmt.tm_mon < 12) ? gmt.tm_mon : 0;
            snprintf(tz_web_date_cache.text[next_slot], 32, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                     kDays[wday], gmt.tm_mday, kMonths[mon], gmt.tm_year + 1900,
                     gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
            memcpy(tz_web_date_cache.json_200_hdr[next_slot] + TZ_JSON_200_DATE_OFF,
                   tz_web_date_cache.text[next_slot], 29u);
            __atomic_store_n(&tz_web_date_cache.seq, next_slot, __ATOMIC_RELEASE);
            __atomic_store_n(&tz_web_date_cache.sec, now, __ATOMIC_RELEASE);
            __atomic_store_n(&tz_web_date_updating, 0, __ATOMIC_RELEASE);
        }
    }
    return __atomic_load_n(&tz_web_date_cache.seq, __ATOMIC_ACQUIRE) & 1u;
}

static inline void tz_web_date_29(char out[29]) {
    uint32_t slot = tz_web_date_refresh_slot();
    memcpy(out, tz_web_date_cache.text[slot], 29u);
}
)c";

inline constexpr const char* kTlsApiLoader = R"c(
#if defined(TZ_USE_TLS) || defined(TZ_USE_CLIENT_TLS)
#define TZ_SSL_FILETYPE_PEM 1
#define TZ_TLS1_2_VERSION 0x0303L
#define TZ_TLS1_3_VERSION 0x0304L
#define TZ_SSL_CTRL_SET_MIN_PROTO_VERSION 123
#define TZ_SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define TZ_SSL_VERIFY_PEER 0x01
#define TZ_SSL_ERROR_WANT_READ 2
#define TZ_SSL_ERROR_WANT_WRITE 3
#define TZ_SSL_ERROR_ZERO_RETURN 6
#define TZ_BIO_CTRL_PENDING 10
#define TZ_X509_V_OK 0L

static struct {
    int loaded;
    int ok;
    void* (*tls_srv_method)(void);
    void* (*tls_cli_method)(void);
    void* (*ctx_new)(void*);
    void (*ctx_free)(void*);
    long (*ctx_ctrl)(void*, int, long, void*);
    int (*use_cert)(void*, const char*, int);
    int (*use_cert_chain)(void*, const char*);
    int (*use_key)(void*, const char*, int);
    int (*check_key)(void*);
    int (*ctx_default_verify)(void*);
    int (*ctx_load_verify)(void*, const char*, const char*);
    void (*ctx_set_verify)(void*, int, void*);
    void* (*ssl_new)(void*);
    void (*ssl_free)(void*);
    void (*ssl_set_bio)(void*, void*, void*);
    int (*ssl_set_fd)(void*, int);
    void (*ssl_set_accept)(void*);
    int (*ssl_accept)(void*);
    int (*ssl_connect)(void*);
    int (*ssl_read)(void*, void*, int);
    int (*ssl_write)(void*, const void*, int);
    int (*ssl_shutdown)(void*);
    int (*ssl_get_error)(const void*, int);
    long (*ssl_ctrl)(void*, int, long, void*);
    int (*ssl_set1_host)(void*, const char*);
    long (*ssl_get_verify_result)(const void*);
    void* (*ssl_get0_param)(void*);
    int (*param_set1_ip_asc)(void*, const char*);
    void* (*bio_s_mem)(void);
    void* (*bio_new)(void*);
    void (*bio_free)(void*);
    int (*bio_read)(void*, void*, int);
    int (*bio_write)(void*, const void*, int);
    size_t (*bio_ctrl_pending)(void*);
} tz_tls_api = {0};

static int tz_tls_load_from_handles(
#ifdef _WIN32
    HMODULE ssl_mod, HMODULE crypto_mod
#else
    void* ssl_mod, void* crypto_mod
#endif
) {
    if (!ssl_mod) return 0;
#ifdef _WIN32
    HMODULE cmod = crypto_mod ? crypto_mod : ssl_mod;
    #define TZ_S_SYM(f, n) tz_web_win_sym(ssl_mod, n, &tz_tls_api.f, sizeof(tz_tls_api.f))
    #define TZ_C_SYM(f, n) (tz_web_win_sym(cmod, n, &tz_tls_api.f, sizeof(tz_tls_api.f)) || tz_web_win_sym(ssl_mod, n, &tz_tls_api.f, sizeof(tz_tls_api.f)))
#else
    void* cmod = crypto_mod ? crypto_mod : ssl_mod;
    #define TZ_S_SYM(f, n) tz_web_posix_sym(ssl_mod, n, &tz_tls_api.f, sizeof(tz_tls_api.f))
    #define TZ_C_SYM(f, n) (tz_web_posix_sym(cmod, n, &tz_tls_api.f, sizeof(tz_tls_api.f)) || tz_web_posix_sym(ssl_mod, n, &tz_tls_api.f, sizeof(tz_tls_api.f)))
#endif
    int ok =
        TZ_S_SYM(tls_srv_method, "TLS_server_method") &&
        TZ_S_SYM(tls_cli_method, "TLS_client_method") &&
        TZ_S_SYM(ctx_new, "SSL_CTX_new") &&
        TZ_S_SYM(ctx_free, "SSL_CTX_free") &&
        TZ_S_SYM(ctx_ctrl, "SSL_CTX_ctrl") &&
        TZ_S_SYM(use_cert, "SSL_CTX_use_certificate_file") &&
        TZ_S_SYM(use_key, "SSL_CTX_use_PrivateKey_file") &&
        TZ_S_SYM(check_key, "SSL_CTX_check_private_key") &&
        TZ_S_SYM(ctx_default_verify, "SSL_CTX_set_default_verify_paths") &&
        TZ_S_SYM(ctx_load_verify, "SSL_CTX_load_verify_locations") &&
        TZ_S_SYM(ctx_set_verify, "SSL_CTX_set_verify") &&
        TZ_S_SYM(ssl_new, "SSL_new") &&
        TZ_S_SYM(ssl_free, "SSL_free") &&
        TZ_S_SYM(ssl_set_bio, "SSL_set_bio") &&
        TZ_S_SYM(ssl_set_fd, "SSL_set_fd") &&
        TZ_S_SYM(ssl_set_accept, "SSL_set_accept_state") &&
        TZ_S_SYM(ssl_accept, "SSL_accept") &&
        TZ_S_SYM(ssl_connect, "SSL_connect") &&
        TZ_S_SYM(ssl_read, "SSL_read") &&
        TZ_S_SYM(ssl_write, "SSL_write") &&
        TZ_S_SYM(ssl_shutdown, "SSL_shutdown") &&
        TZ_S_SYM(ssl_get_error, "SSL_get_error") &&
        TZ_S_SYM(ssl_ctrl, "SSL_ctrl") &&
        TZ_S_SYM(ssl_set1_host, "SSL_set1_host") &&
        TZ_S_SYM(ssl_get_verify_result, "SSL_get_verify_result") &&
        TZ_S_SYM(ssl_get0_param, "SSL_get0_param") &&
        TZ_C_SYM(bio_s_mem, "BIO_s_mem") &&
        TZ_C_SYM(bio_new, "BIO_new") &&
        TZ_C_SYM(bio_free, "BIO_free") &&
        TZ_C_SYM(bio_read, "BIO_read") &&
        TZ_C_SYM(bio_write, "BIO_write") &&
        TZ_C_SYM(bio_ctrl_pending, "BIO_ctrl_pending");
    (void)TZ_S_SYM(use_cert_chain, "SSL_CTX_use_certificate_chain_file");
    (void)TZ_C_SYM(param_set1_ip_asc, "X509_VERIFY_PARAM_set1_ip_asc");
    #undef TZ_S_SYM
    #undef TZ_C_SYM
    return ok;
}

static int tz_tls_init_api(void) {
    if (__atomic_load_n(&tz_tls_api.loaded, __ATOMIC_ACQUIRE) == 2) return tz_tls_api.ok;
    int exp = 0;
    if (__atomic_compare_exchange_n(&tz_tls_api.loaded, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        int ok = 0;
        const char* env_ssl = getenv("TURI_OPENSSL_DLL");
#ifdef _WIN32
        const char* cands[6] = {
            env_ssl,
            "libssl-3-x64.dll",
            "C:\\Program Files\\Git\\mingw64\\bin\\libssl-3-x64.dll",
            "C:\\Program Files\\OpenSSL-Win64\\bin\\libssl-3-x64.dll",
            "C:\\Program Files\\PostgreSQL\\18\\bin\\libssl-3-x64.dll",
            "libssl-1_1-x64.dll"
        };
        for (int i = 0; i < 6 && !ok; ++i) {
            if (!cands[i] || !cands[i][0]) continue;
            HMODULE sm = (strchr(cands[i], '\\') || strchr(cands[i], '/'))
                ? LoadLibraryExA(cands[i], NULL, LOAD_WITH_ALTERED_SEARCH_PATH)
                : LoadLibraryA(cands[i]);
            if (!sm) continue;
            HMODULE cm = NULL;
            char pathbuf[1024];
            DWORD plen = GetModuleFileNameA(sm, pathbuf, (DWORD)sizeof(pathbuf));
            if (plen > 0u && plen + 32u < sizeof(pathbuf)) {
                char* sep = strrchr(pathbuf, '\\');
                char* sep2 = strrchr(pathbuf, '/');
                if (sep2 && (!sep || sep2 > sep)) sep = sep2;
                if (sep) {
                    strcpy(sep + 1, "libcrypto-3-x64.dll");
                    cm = LoadLibraryExA(pathbuf, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
                    if (!cm) {
                        strcpy(sep + 1, "libcrypto-1_1-x64.dll");
                        cm = LoadLibraryExA(pathbuf, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
                    }
                }
            }
            if (!cm) cm = LoadLibraryA("libcrypto-3-x64.dll");
            if (!cm) cm = LoadLibraryA("libcrypto-1_1-x64.dll");
            ok = tz_tls_load_from_handles(sm, cm);
        }
#else
        const char* cands[4] = { env_ssl, "libssl.so.3", "libssl.so.1.1", "libssl.dylib" };
        for (int i = 0; i < 4 && !ok; ++i) {
            if (!cands[i] || !cands[i][0]) continue;
            void* sm = dlopen(cands[i], RTLD_LAZY | RTLD_LOCAL);
            if (!sm) continue;
            void* cm = dlopen("libcrypto.so.3", RTLD_LAZY | RTLD_LOCAL);
            if (!cm) cm = dlopen("libcrypto.so.1.1", RTLD_LAZY | RTLD_LOCAL);
            if (!cm) cm = dlopen("libcrypto.dylib", RTLD_LAZY | RTLD_LOCAL);
            ok = tz_tls_load_from_handles(sm, cm);
        }
#endif
        tz_tls_api.ok = ok;
        __atomic_store_n(&tz_tls_api.loaded, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&tz_tls_api.loaded, __ATOMIC_ACQUIRE) != 2) {
#ifdef _WIN32
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
    return tz_tls_api.ok;
}
#endif
)c";

inline constexpr const char* kServerCore = R"c(
#define TZ_WEB_MAX_HEADERS 32
#define TZ_WEB_MAX_PARAMS 8
#define TZ_WEB_MAX_SEGS 16
#define TZ_WEB_RECV_INIT 8192u
#define TZ_WEB_SEND_INIT 8192u
#define TZ_WEB_BODY_INIT 1024u
#define TZ_WEB_HDR_INIT 512u
#define TZ_WEB_MAX_HDR_BYTES 16384u
#define TZ_WEB_MAX_BODY_BYTES (16u * 1024u * 1024u)
#define TZ_WEB_MAX_CONNS 4096u
#define TZ_WEB_STATIC_CACHE_MAX 64u
#define TZ_WEB_STATIC_CACHE_FILE_MAX (256u * 1024u)

enum {
    TZ_M_GET = 1,
    TZ_M_POST = 2,
    TZ_M_PUT = 3,
    TZ_M_DELETE = 4,
    TZ_M_PATCH = 5,
    TZ_M_HEAD = 6,
    TZ_M_OPTIONS = 7
};

typedef void (*tz_web_handler_fn)(void* ctx);
typedef int32_t (*tz_web_mw_fn)(void* ctx);
typedef void (*tz_ws_open_fn)(void* ws);
typedef void (*tz_ws_msg_fn)(void* ws, int32_t opcode, const uint8_t* data, int64_t len);
typedef void (*tz_ws_close_fn)(void* ws, int32_t code);

typedef struct tz_web_slice {
    const char* ptr;
    size_t len;
} tz_web_slice;

typedef struct tz_web_route_entry {
    int32_t method;
    char* pattern;
    size_t pattern_len;
    uint64_t hash;
    tz_web_handler_fn fn;
    int is_static_resp;
    int is_param;
    const char* fast_prefix;
    size_t fast_prefix_len;
    tz_web_slice fast_param_name;
    char* static_prefix;
    size_t static_prefix_len;
    char* static_suffix;
    size_t static_suffix_len;
    tz_web_slice segs[TZ_WEB_MAX_SEGS];
    int seg_is_param[TZ_WEB_MAX_SEGS];
    int seg_is_wild[TZ_WEB_MAX_SEGS];
    int seg_count;
} tz_web_route_entry;

typedef struct tz_hash_slot {
    uint64_t hash;
    const char* pattern;
    size_t pattern_len;
    tz_web_handler_fn fn;
    const char* static_prefix;
    const char* static_suffix;
    uint32_t static_prefix_len;
    uint32_t static_suffix_len;
    int32_t method;
    uint8_t occupied;
    uint8_t is_static_resp;
} tz_hash_slot;

typedef struct tz_web_mount {
    char* prefix;
    size_t prefix_len;
    char* root_dir;
    size_t root_dir_len;
} tz_web_mount;

typedef struct tz_web_static_cache_entry {
    char* full_path;
    uint64_t hash;
    int64_t size;
    int64_t mtime;
    const char* mime;
    char etag[40];
    size_t etag_len;
    char* data;
} tz_web_static_cache_entry;

typedef struct tz_web_ws_route_entry {
    char* pattern;
    size_t pattern_len;
    tz_ws_open_fn on_open;
    tz_ws_msg_fn on_msg;
    tz_ws_close_fn on_close;
} tz_web_ws_route_entry;

typedef struct tz_web_ws_conn tz_web_ws_conn;

typedef struct tz_web_server tz_web_server;

enum {
    TZ_CONN_FREE = 0,
    TZ_CONN_HTTP = 1,
    TZ_CONN_WS = 2,
    TZ_CONN_CLOSING = 3
};

#ifdef _WIN32
enum {
    TZ_IO_ACCEPT = 1,
    TZ_IO_RECV = 2,
    TZ_IO_SEND = 3,
    TZ_IO_YIELD = 4
};

typedef struct tz_io_ov {
    OVERLAPPED ov;
    int kind;
    uint32_t slot_idx;
} tz_io_ov;

typedef struct tz_accept_slot {
    tz_io_ov tag;
    SOCKET accept_sock;
    char addr_buf[(sizeof(struct sockaddr_storage) + 16) * 2];
} tz_accept_slot;
#endif

typedef struct tz_web_conn {
    tz_web_server* srv;
    tz_web_sock_t sock;
    uint32_t slot_idx;
    int state;
    int close_after_send;
    int skip_on_sync;
    uint32_t cur_date_slot;
#ifdef __linux__
    uint32_t ep_events;
#endif
    tz_web_buf recv_buf;
    tz_web_buf send_buf;
    tz_web_buf extra_hdrs;
    tz_web_buf body_buf;
    size_t parse_pos;
    size_t send_off;
    tz_web_ws_conn* ws;
#ifdef TZ_USE_TLS
    void* ssl;
    void* rbio;
    void* wbio;
    int tls_established;
    tz_web_buf tls_wire_out;
    size_t tls_wire_off;
    tz_web_buf tls_raw_in;
    tz_web_lock_t tls_lock;
#endif
#ifdef _WIN32
    tz_io_ov io_tag;
#endif
} tz_web_conn;

typedef struct tz_web_ctx {
    tz_web_conn* conn;
    int32_t method;
    tz_web_slice path;
    tz_web_slice query;
    tz_web_slice body;
    tz_web_slice hdr_names[TZ_WEB_MAX_HEADERS];
    tz_web_slice hdr_vals[TZ_WEB_MAX_HEADERS];
    int hdr_count;
    tz_web_slice param_names[TZ_WEB_MAX_PARAMS];
    tz_web_slice param_vals[TZ_WEB_MAX_PARAMS];
    int param_count;
    int32_t resp_status;
    int resp_sent;
    int json_active;
    int json_fields;
} tz_web_ctx;

struct tz_web_ws_conn {
    tz_web_server* srv;
    tz_web_conn* conn;
    uint64_t id;
    tz_ws_open_fn on_open;
    tz_ws_msg_fn on_msg;
    tz_ws_close_fn on_close;
    tz_web_lock_t send_lock;
    int32_t closed;
    struct tz_web_ws_conn* next;
};

#ifdef __linux__
typedef struct tz_epoll_worker {
    tz_web_server* srv;
    int worker_id;
    int epfd;
    int evfd;
    tz_web_sock_t listen_sock;
    pthread_t thread;
} tz_epoll_worker;
#endif

struct tz_web_server {
    char* host;
    int req_port;
    int bound_port;
    int num_workers;
    int32_t running;
    int32_t started;
    uint64_t reqs_total;
    uint64_t active_conns;
    uint64_t next_ws_id;

    tz_web_route_entry* routes;
    size_t route_count;
    size_t route_cap;

    tz_hash_slot* hash_slots;
    size_t hash_mask;
    tz_web_route_entry** param_routes;
    size_t param_count;

    tz_web_mount* mounts;
    size_t mount_count;
    size_t mount_cap;

    tz_web_static_cache_entry static_cache[TZ_WEB_STATIC_CACHE_MAX];
    size_t static_cache_count;
    size_t static_cache_cursor;
    tz_web_lock_t static_cache_lock;

    tz_web_mw_fn mw_before;
    tz_web_mw_fn mw_after;
    void* user_data;

    tz_web_ws_route_entry* ws_routes;
    size_t ws_route_count;
    size_t ws_route_cap;
    tz_web_ws_conn* ws_head;
    tz_web_lock_t ws_lock;

    tz_web_conn* conns;
    uint32_t* free_stack;
    uint32_t free_top;
    uint32_t conn_cap;
    tz_web_lock_t conn_lock;
#ifdef TZ_USE_TLS
    void* tls_ctx;
#endif

#ifdef _WIN32
    HANDLE iocp;
    SOCKET listen_sock;
    LPFN_ACCEPTEX fn_acceptex;
    tz_accept_slot* accept_slots;
    int accept_count;
    int32_t pending_accepts;
    HANDLE* worker_threads;
    HANDLE bg_thread;
#elif defined(__linux__)
    tz_epoll_worker* ep_workers;
    pthread_mutex_t bind_lock;
    pthread_cond_t bind_cond;
    int bind_ready;
    pthread_t bg_thread;
    int has_bg_thread;
#else
    tz_web_sock_t listen_sock;
    pthread_t bg_thread;
    int has_bg_thread;
#endif
};

#if defined(TZ_USE_TLS) && defined(TZ_HAS_SERVER)
static int tz_tls_drain_wbio_to(tz_web_conn* c, tz_web_buf* dst) {
    if (!c->wbio) return 1;
    char tmp[8192];
    while (tz_tls_api.bio_ctrl_pending(c->wbio) > 0u) {
        int n = tz_tls_api.bio_read(c->wbio, tmp, (int)sizeof(tmp));
        if (n <= 0) break;
        if (!tz_web_buf_append(dst, tmp, (size_t)n)) return 0;
    }
    return 1;
}

static int tz_tls_encrypt_to_buf(tz_web_conn* c, const char* data, size_t len, tz_web_buf* dst) {
    size_t off = 0u;
    while (off < len) {
        size_t rem = len - off;
        int chunk = rem > 65536u ? 65536 : (int)rem;
        int w = tz_tls_api.ssl_write(c->ssl, data + off, chunk);
        if (!tz_tls_drain_wbio_to(c, dst)) return 0;
        if (w <= 0) {
            int err = tz_tls_api.ssl_get_error(c->ssl, w);
            if (err == TZ_SSL_ERROR_WANT_WRITE || err == TZ_SSL_ERROR_WANT_READ) continue;
            return 0;
        }
        off += (size_t)w;
    }
    return tz_tls_drain_wbio_to(c, dst);
}
#endif

static const char* tz_web_reason(int status) {
    switch (status) {
        case 101: return "101 Switching Protocols";
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 301: return "301 Moved Permanently";
        case 302: return "302 Found";
        case 304: return "304 Not Modified";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 429: return "429 Too Many Requests";
        case 431: return "431 Request Header Fields Too Large";
        case 500: return "500 Internal Server Error";
        case 502: return "502 Bad Gateway";
        case 503: return "503 Service Unavailable";
        default: return "200 OK";
    }
}

static const char* tz_web_mime_for_path(const char* p, size_t n) {
    size_t dot = n;
    for (size_t i = n; i > 0u; --i) {
        if (p[i - 1u] == '.') { dot = i; break; }
        if (p[i - 1u] == '/' || p[i - 1u] == '\\') break;
    }
    if (dot >= n) return "application/octet-stream";
    const char* ext = p + dot;
    size_t elen = n - dot;
    if (tz_web_eq_nocase(ext, elen, "html", 4) || tz_web_eq_nocase(ext, elen, "htm", 3)) return "text/html; charset=utf-8";
    if (tz_web_eq_nocase(ext, elen, "css", 3)) return "text/css; charset=utf-8";
    if (tz_web_eq_nocase(ext, elen, "js", 2) || tz_web_eq_nocase(ext, elen, "mjs", 3)) return "application/javascript; charset=utf-8";
    if (tz_web_eq_nocase(ext, elen, "json", 4)) return "application/json; charset=utf-8";
    if (tz_web_eq_nocase(ext, elen, "txt", 3)) return "text/plain; charset=utf-8";
    if (tz_web_eq_nocase(ext, elen, "svg", 3)) return "image/svg+xml";
    if (tz_web_eq_nocase(ext, elen, "png", 3)) return "image/png";
    if (tz_web_eq_nocase(ext, elen, "jpg", 3) || tz_web_eq_nocase(ext, elen, "jpeg", 4)) return "image/jpeg";
    if (tz_web_eq_nocase(ext, elen, "webp", 4)) return "image/webp";
    if (tz_web_eq_nocase(ext, elen, "ico", 3)) return "image/x-icon";
    if (tz_web_eq_nocase(ext, elen, "woff2", 5)) return "font/woff2";
    if (tz_web_eq_nocase(ext, elen, "wasm", 4)) return "application/wasm";
    if (tz_web_eq_nocase(ext, elen, "xml", 3)) return "application/xml; charset=utf-8";
    return "application/octet-stream";
}
)c";

inline constexpr const char* kServerHttp = R"c(
static int tz_web_write_http_resp(tz_web_conn* c, int status, const char* ct, size_t ct_len,
                                  const char* body, size_t body_len, const char* extra, size_t extra_len,
                                  int close_conn, int head_only) {
    const char* reason = tz_web_reason(status);
    size_t reason_len = strlen(reason);
    char clen_buf[32];
    size_t clen_len = tz_web_fmt_u64(clen_buf, (uint64_t)body_len);
    char date_buf[29];
    tz_web_date_29(date_buf);
    size_t need = 9u + reason_len + 2u
                + (ct_len ? (14u + ct_len + 2u) : 0u)
                + 16u + clen_len + 2u
                + 6u + 29u + 2u
                + extra_len
                + (close_conn ? 19u : 0u)
                + 2u + (head_only ? 0u : body_len);
    if (!tz_web_buf_reserve(&c->send_buf, need)) return 0;
    char* dst = c->send_buf.data + c->send_buf.len;
    size_t pos = 0;
    memcpy(dst + pos, "HTTP/1.1 ", 9u); pos += 9u;
    memcpy(dst + pos, reason, reason_len); pos += reason_len;
    dst[pos++] = '\r'; dst[pos++] = '\n';
    if (ct_len > 0u && ct) {
        memcpy(dst + pos, "Content-Type: ", 14u); pos += 14u;
        memcpy(dst + pos, ct, ct_len); pos += ct_len;
        dst[pos++] = '\r'; dst[pos++] = '\n';
    }
    memcpy(dst + pos, "Content-Length: ", 16u); pos += 16u;
    memcpy(dst + pos, clen_buf, clen_len); pos += clen_len;
    dst[pos++] = '\r'; dst[pos++] = '\n';
    memcpy(dst + pos, "Date: ", 6u); pos += 6u;
    memcpy(dst + pos, date_buf, 29u); pos += 29u;
    dst[pos++] = '\r'; dst[pos++] = '\n';
    if (extra_len > 0u && extra) {
        memcpy(dst + pos, extra, extra_len); pos += extra_len;
    }
    if (close_conn) {
        memcpy(dst + pos, "Connection: close\r\n", 19u); pos += 19u;
    }
    dst[pos++] = '\r'; dst[pos++] = '\n';
    if (!head_only && body_len > 0u && body) {
        memcpy(dst + pos, body, body_len); pos += body_len;
    }
    c->send_buf.len += pos;
    return 1;
}

static inline int tz_web_ctx_find_hdr(const tz_web_ctx* ctx, const char* name, size_t nlen, tz_web_slice* out) {
    for (int i = 0; i < ctx->hdr_count; ++i) {
        if (tz_web_eq_nocase(ctx->hdr_names[i].ptr, ctx->hdr_names[i].len, name, nlen)) {
            if (out) *out = ctx->hdr_vals[i];
            return 1;
        }
    }
    return 0;
}

static void tz_web_ctx_finish_default(tz_web_ctx* ctx) {
    if (!ctx->resp_sent) {
        int st = ctx->resp_status ? ctx->resp_status : 204;
        tz_web_write_http_resp(ctx->conn, st, NULL, 0u, NULL, 0u,
                               ctx->conn->extra_hdrs.data, ctx->conn->extra_hdrs.len,
                               ctx->conn->close_after_send, ctx->method == TZ_M_HEAD);
        ctx->resp_sent = 1;
    }
}

static int tz_web_parse_method(const char* p, size_t n) {
    if (n == 3u && memcmp(p, "GET", 3u) == 0) return TZ_M_GET;
    if (n == 4u && memcmp(p, "POST", 4u) == 0) return TZ_M_POST;
    if (n == 3u && memcmp(p, "PUT", 3u) == 0) return TZ_M_PUT;
    if (n == 6u && memcmp(p, "DELETE", 6u) == 0) return TZ_M_DELETE;
    if (n == 5u && memcmp(p, "PATCH", 5u) == 0) return TZ_M_PATCH;
    if (n == 4u && memcmp(p, "HEAD", 4u) == 0) return TZ_M_HEAD;
    if (n == 7u && memcmp(p, "OPTIONS", 7u) == 0) return TZ_M_OPTIONS;
    return 0;
}

static int tz_web_match_param_route(const tz_web_route_entry* r, const char* path, size_t plen, tz_web_ctx* ctx) {
    if (r->fast_prefix_len > 0u) {
        if (plen <= r->fast_prefix_len || memcmp(path, r->fast_prefix, r->fast_prefix_len) != 0) {
            return 0;
        }
        const char* val = path + r->fast_prefix_len;
        size_t vlen = plen - r->fast_prefix_len;
        if (memchr(val, '/', vlen) != NULL) return 0;
        ctx->param_names[0] = r->fast_param_name;
        ctx->param_vals[0].ptr = val;
        ctx->param_vals[0].len = vlen;
        ctx->param_count = 1;
        return 1;
    }
    ctx->param_count = 0;
    size_t pos = 0;
    if (pos < plen && path[pos] == '/') ++pos;
    int sidx = 0;
    while (sidx < r->seg_count) {
        if (r->seg_is_wild[sidx]) {
            if (pos > plen) return 0;
            if (ctx->param_count < TZ_WEB_MAX_PARAMS) {
                ctx->param_names[ctx->param_count] = r->segs[sidx];
                ctx->param_vals[ctx->param_count].ptr = path + pos;
                ctx->param_vals[ctx->param_count].len = plen - pos;
                ctx->param_count++;
            }
            return 1;
        }
        if (pos >= plen) {
            if (r->seg_count == 1 && r->segs[0].len == 0u && !r->seg_is_param[0]) return 1;
            return 0;
        }
        const char* seg_start = path + pos;
        const char* slash = (const char*)memchr(seg_start, '/', plen - pos);
        size_t slen = slash ? (size_t)(slash - seg_start) : (plen - pos);
        if (r->seg_is_param[sidx]) {
            if (slen == 0u) return 0;
            if (ctx->param_count < TZ_WEB_MAX_PARAMS) {
                ctx->param_names[ctx->param_count] = r->segs[sidx];
                ctx->param_vals[ctx->param_count].ptr = seg_start;
                ctx->param_vals[ctx->param_count].len = slen;
                ctx->param_count++;
            }
        } else {
            if (slen != r->segs[sidx].len || (slen > 0u && memcmp(seg_start, r->segs[sidx].ptr, slen) != 0))
                return 0;
        }
        pos += slen;
        if (pos < plen && path[pos] == '/') ++pos;
        sidx++;
    }
    return pos == plen;
}
)c";

inline constexpr const char* kServerRouter = R"c(
static int tz_web_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int tz_web_sanitize_rel_path(const char* raw, size_t rlen, char* out, size_t cap, size_t* out_len) {
    size_t w = 0;
    for (size_t i = 0; i < rlen; ++i) {
        char ch = raw[i];
        if (ch == '%') {
            if (i + 2u >= rlen) return 0;
            int h1 = tz_web_hexval(raw[i + 1u]);
            int h2 = tz_web_hexval(raw[i + 2u]);
            if (h1 < 0 || h2 < 0) return 0;
            ch = (char)((h1 << 4) | h2);
            i += 2u;
        }
        if ((unsigned char)ch < 0x20u || ch == '\\' || ch == ':') return 0;
        if (ch == '/' && (w == 0u || out[w - 1u] == '/')) continue;
        if (w + 1u >= cap) return 0;
        out[w++] = ch;
    }
    out[w] = '\0';
    /* Reject any segment equal to "." or ".." */
    size_t seg_start = 0;
    for (size_t i = 0; i <= w; ++i) {
        if (i == w || out[i] == '/') {
            size_t slen = i - seg_start;
            if ((slen == 1u && out[seg_start] == '.') ||
                (slen == 2u && out[seg_start] == '.' && out[seg_start + 1u] == '.')) {
                return 0;
            }
            seg_start = i + 1u;
        }
    }
    if (w == 0u) {
        if (cap < 11u) return 0;
        memcpy(out, "index.html", 10u);
        out[10] = '\0';
        w = 10u;
    }
    *out_len = w;
    return 1;
}

static int tz_web_try_static_mounts(tz_web_server* srv, tz_web_ctx* ctx) {
    if (srv->mount_count == 0u || (ctx->method != TZ_M_GET && ctx->method != TZ_M_HEAD)) return 0;
    for (size_t mi = 0; mi < srv->mount_count; ++mi) {
        const tz_web_mount* m = &srv->mounts[mi];
        if (ctx->path.len < m->prefix_len) continue;
        if (memcmp(ctx->path.ptr, m->prefix, m->prefix_len) != 0) continue;
        if (ctx->path.len > m->prefix_len && m->prefix_len > 1u && ctx->path.ptr[m->prefix_len] != '/') continue;
        const char* rel_raw = ctx->path.ptr + m->prefix_len;
        size_t rel_raw_len = ctx->path.len - m->prefix_len;
        if (rel_raw_len > 0u && rel_raw[0] == '/') { rel_raw++; rel_raw_len--; }
        char rel_clean[512];
        size_t rel_len = 0;
        if (!tz_web_sanitize_rel_path(rel_raw, rel_raw_len, rel_clean, sizeof(rel_clean), &rel_len)) {
            tz_web_write_http_resp(ctx->conn, 403, "text/plain; charset=utf-8", 25u,
                                   "Forbidden", 9u, NULL, 0u, ctx->conn->close_after_send, ctx->method == TZ_M_HEAD);
            ctx->resp_sent = 1;
            return 1;
        }
        char full[1024];
        if (m->root_dir_len + 1u + rel_len >= sizeof(full)) return 0;
        memcpy(full, m->root_dir, m->root_dir_len);
        size_t fp = m->root_dir_len;
        if (fp > 0u && full[fp - 1u] != '/' && full[fp - 1u] != '\\') full[fp++] = '/';
        memcpy(full + fp, rel_clean, rel_len);
        fp += rel_len;
        full[fp] = '\0';

        struct stat st;
        if (stat(full, &st) != 0 || !(st.st_mode & S_IFREG)) continue;

        char etag[40];
        int elen_i = snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
                              (unsigned long long)st.st_size, (unsigned long long)st.st_mtime);
        size_t etag_len = elen_i > 0 ? (size_t)elen_i : 0u;
        tz_web_slice inm;
        if (tz_web_ctx_find_hdr(ctx, "if-none-match", 13u, &inm)) {
            if (inm.len == etag_len && memcmp(inm.ptr, etag, etag_len) == 0) {
                char extra[64];
                int ex = snprintf(extra, sizeof(extra), "ETag: %s\r\n", etag);
                tz_web_write_http_resp(ctx->conn, 304, NULL, 0u, NULL, 0u, extra, ex > 0 ? (size_t)ex : 0u,
                                       ctx->conn->close_after_send, 1);
                ctx->resp_sent = 1;
                return 1;
            }
        }
        const char* mime = tz_web_mime_for_path(full, fp);
        uint64_t fhash = tz_web_hash(full, fp);
        if ((uint64_t)st.st_size <= TZ_WEB_STATIC_CACHE_FILE_MAX) {
            tz_web_lock_acq(&srv->static_cache_lock);
            int hit = -1;
            for (size_t ci = 0; ci < srv->static_cache_count; ++ci) {
                tz_web_static_cache_entry* e = &srv->static_cache[ci];
                if (e->hash == fhash && e->size == (int64_t)st.st_size &&
                    e->mtime == (int64_t)st.st_mtime && strcmp(e->full_path, full) == 0) {
                    hit = (int)ci;
                    break;
                }
            }
            if (hit >= 0) {
                tz_web_static_cache_entry* e = &srv->static_cache[hit];
                char extra[64];
                int ex = snprintf(extra, sizeof(extra), "ETag: %s\r\n", e->etag);
                tz_web_write_http_resp(ctx->conn, 200, e->mime, strlen(e->mime),
                                       e->data, (size_t)e->size, extra, ex > 0 ? (size_t)ex : 0u,
                                       ctx->conn->close_after_send, ctx->method == TZ_M_HEAD);
                tz_web_lock_rel(&srv->static_cache_lock);
                ctx->resp_sent = 1;
                return 1;
            }
            tz_web_lock_rel(&srv->static_cache_lock);
        }

        FILE* f = fopen(full, "rb");
        if (!f) continue;
        size_t fsz = (size_t)st.st_size;
        char* fbuf = (char*)malloc(fsz ? fsz : 1u);
        if (!fbuf) { fclose(f); return 0; }
        size_t got = fsz ? fread(fbuf, 1u, fsz, f) : 0u;
        fclose(f);
        if (got != fsz) { free(fbuf); continue; }

        char extra[64];
        int ex = snprintf(extra, sizeof(extra), "ETag: %s\r\n", etag);
        tz_web_write_http_resp(ctx->conn, 200, mime, strlen(mime), fbuf, fsz,
                               extra, ex > 0 ? (size_t)ex : 0u,
                               ctx->conn->close_after_send, ctx->method == TZ_M_HEAD);
        ctx->resp_sent = 1;

        if (fsz <= TZ_WEB_STATIC_CACHE_FILE_MAX) {
            tz_web_lock_acq(&srv->static_cache_lock);
            size_t slot;
            if (srv->static_cache_count < TZ_WEB_STATIC_CACHE_MAX) {
                slot = srv->static_cache_count++;
            } else {
                slot = srv->static_cache_cursor;
                srv->static_cache_cursor = (srv->static_cache_cursor + 1u) % TZ_WEB_STATIC_CACHE_MAX;
                free(srv->static_cache[slot].full_path);
                free(srv->static_cache[slot].data);
            }
            tz_web_static_cache_entry* e = &srv->static_cache[slot];
            e->full_path = tz_web_dup_cstr(full, fp);
            e->hash = fhash;
            e->size = (int64_t)fsz;
            e->mtime = (int64_t)st.st_mtime;
            e->mime = mime;
            memcpy(e->etag, etag, etag_len + 1u);
            e->etag_len = etag_len;
            e->data = fbuf;
            tz_web_lock_rel(&srv->static_cache_lock);
        } else {
            free(fbuf);
        }
        return 1;
    }
    return 0;
}

static void tz_web_build_route_tables(tz_web_server* srv) {
    free(srv->hash_slots);
    free(srv->param_routes);
    size_t stat_count = 0;
    size_t param_count = 0;
    for (size_t i = 0; i < srv->route_count; ++i) {
        if (srv->routes[i].is_param) param_count++;
        else stat_count++;
    }
    size_t cap = 64u;
    while (cap < stat_count * 2u) cap *= 2u;
    srv->hash_slots = (tz_hash_slot*)calloc(cap, sizeof(tz_hash_slot));
    srv->hash_mask = cap - 1u;

    if (param_count > 0u) {
        srv->param_routes = (tz_web_route_entry**)malloc(param_count * sizeof(tz_web_route_entry*));
    } else {
        srv->param_routes = NULL;
    }
    srv->param_count = 0;
    for (size_t i = 0; i < srv->route_count; ++i) {
        tz_web_route_entry* r = &srv->routes[i];
        if (r->is_param) {
            srv->param_routes[srv->param_count++] = r;
        } else {
            uint64_t h = r->hash ^ ((uint64_t)r->method * 0x9e3779b97f4a7c15ULL);
            size_t idx = (size_t)(h & srv->hash_mask);
            while (srv->hash_slots[idx].occupied) idx = (idx + 1u) & srv->hash_mask;
            tz_hash_slot* s = &srv->hash_slots[idx];
            s->hash = r->hash;
            s->pattern = r->pattern;
            s->pattern_len = r->pattern_len;
            s->fn = r->fn;
            s->static_prefix = r->static_prefix;
            s->static_suffix = r->static_suffix;
            s->static_prefix_len = (uint32_t)r->static_prefix_len;
            s->static_suffix_len = (uint32_t)r->static_suffix_len;
            s->method = r->method;
            s->occupied = 1u;
            s->is_static_resp = (uint8_t)(r->is_static_resp ? 1u : 0u);
        }
    }
}
)c";

inline constexpr const char* kServerWsStub = R"c(
static int tz_web_try_ws_upgrade(tz_web_server* srv, tz_web_ctx* ctx, size_t consumed) {
    (void)srv; (void)ctx; (void)consumed;
    return 0;
}
static int tz_web_ws_process_frames(tz_web_conn* c) {
    (void)c;
    return 0;
}
static void tz_web_ws_detach_conn(tz_web_conn* c, int code) {
    (void)c; (void)code;
}
)c";

inline constexpr const char* kServerWsFull = R"c(
static void tz_sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
    size_t rem = len % 64u;
    size_t pad_len = (rem < 56u) ? (56u - rem) : (120u - rem);
    size_t total = len + pad_len + 8u;
    uint8_t stack_buf[256];
    uint8_t* msg = total <= sizeof(stack_buf) ? stack_buf : (uint8_t*)malloc(total);
    memcpy(msg, data, len);
    msg[len] = 0x80u;
    if (pad_len > 1u) memset(msg + len + 1u, 0, pad_len - 1u);
    uint64_t bits = (uint64_t)len * 8ULL;
    for (int i = 0; i < 8; ++i) msg[len + pad_len + (size_t)i] = (uint8_t)((bits >> ((7 - i) * 8)) & 0xFFu);

    for (size_t off = 0; off < total; off += 64u) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = msg + off + (size_t)i * 4u;
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
        for (int i = 16; i < 80; ++i) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    if (msg != stack_buf) free(msg);
    uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = (uint8_t)(hs[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(hs[i]);
    }
}

static size_t tz_base64_encode(const uint8_t* in, size_t len, char* out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3u) {
        uint32_t v = ((uint32_t)in[i] << 16)
                   | ((i + 1u < len ? (uint32_t)in[i + 1u] : 0u) << 8)
                   | (i + 2u < len ? (uint32_t)in[i + 2u] : 0u);
        out[o++] = tbl[(v >> 18) & 63u];
        out[o++] = tbl[(v >> 12) & 63u];
        out[o++] = (i + 1u < len) ? tbl[(v >> 6) & 63u] : '=';
        out[o++] = (i + 2u < len) ? tbl[v & 63u] : '=';
    }
    out[o] = '\0';
    return o;
}

static int tz_web_ws_send_blocking(tz_web_sock_t sock, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int chunk = (len - sent) > 65536u ? 65536 : (int)(len - sent);
#ifdef _WIN32
        int n = send(sock, (const char*)(buf + sent), chunk, 0);
        if (n <= 0) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                fd_set wf; FD_ZERO(&wf); FD_SET(sock, &wf);
                struct timeval tv = { 2, 0 };
                if (select(0, NULL, &wf, NULL, &tv) > 0) continue;
            }
            return 0;
        }
#else
        ssize_t n = send(sock, buf + sent, (size_t)chunk, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { sock, POLLOUT, 0 };
                if (poll(&pfd, 1, 2000) > 0) continue;
            }
            return 0;
        }
        if (n == 0) return 0;
#endif
        sent += (size_t)n;
    }
    return 1;
}

static int tz_web_ws_write_frame(tz_web_ws_conn* ws, int32_t opcode, const uint8_t* data, size_t len) {
    if (!ws || __atomic_load_n(&ws->closed, __ATOMIC_ACQUIRE)) return 0;
    uint8_t hdr[10];
    size_t hlen = 0;
    hdr[hlen++] = (uint8_t)(0x80u | ((uint32_t)opcode & 0x0Fu));
    if (len < 126u) {
        hdr[hlen++] = (uint8_t)len;
    } else if (len <= 65535u) {
        hdr[hlen++] = 126u;
        hdr[hlen++] = (uint8_t)(len >> 8);
        hdr[hlen++] = (uint8_t)(len & 0xFFu);
    } else {
        hdr[hlen++] = 127u;
        uint64_t ulen = (uint64_t)len;
        for (int i = 0; i < 8; ++i) hdr[hlen++] = (uint8_t)((ulen >> ((7 - i) * 8)) & 0xFFu);
    }
    tz_web_lock_acq(&ws->send_lock);
    int ok = 0;
    if (!__atomic_load_n(&ws->closed, __ATOMIC_ACQUIRE) && ws->conn && ws->conn->sock != TZ_WEB_BAD_SOCK) {
#ifdef TZ_USE_TLS
        tz_web_conn* c = ws->conn;
        if (__builtin_expect(c->ssl != NULL, 0)) {
            tz_web_buf wire;
            tz_web_buf_init(&wire, 256u + len);
            tz_web_lock_acq(&c->tls_lock);
            int enc_ok = 1;
            if (c->tls_wire_out.len > c->tls_wire_off) {
                if (!tz_web_buf_append(&wire, c->tls_wire_out.data + c->tls_wire_off,
                                       c->tls_wire_out.len - c->tls_wire_off)) enc_ok = 0;
                c->tls_wire_out.len = 0;
                c->tls_wire_off = 0;
            }
            if (enc_ok) enc_ok = tz_tls_encrypt_to_buf(c, (const char*)hdr, hlen, &wire);
            if (enc_ok && len > 0u && data) enc_ok = tz_tls_encrypt_to_buf(c, (const char*)data, len, &wire);
            tz_web_lock_rel(&c->tls_lock);
            if (enc_ok && wire.len > 0u) {
                ok = tz_web_ws_send_blocking(c->sock, (const uint8_t*)wire.data, wire.len);
            } else {
                ok = enc_ok;
            }
            tz_web_buf_free(&wire);
        } else {
            ok = tz_web_ws_send_blocking(ws->conn->sock, hdr, hlen);
            if (ok && len > 0u && data) ok = tz_web_ws_send_blocking(ws->conn->sock, data, len);
        }
#else
        ok = tz_web_ws_send_blocking(ws->conn->sock, hdr, hlen);
        if (ok && len > 0u && data) ok = tz_web_ws_send_blocking(ws->conn->sock, data, len);
#endif
    }
    tz_web_lock_rel(&ws->send_lock);
    return ok;
}
)c";

inline constexpr const char* kServerWsApi = R"c(
static int tz_web_try_ws_upgrade(tz_web_server* srv, tz_web_ctx* ctx, size_t consumed) {
    if (srv->ws_route_count == 0u || ctx->method != TZ_M_GET) return 0;
    tz_web_slice upg, key;
    if (!tz_web_ctx_find_hdr(ctx, "upgrade", 7u, &upg) ||
        !tz_web_eq_nocase(upg.ptr, upg.len, "websocket", 9u)) return 0;
    if (!tz_web_ctx_find_hdr(ctx, "sec-websocket-key", 17u, &key) || key.len == 0u || key.len > 128u)
        return 0;
    const tz_web_ws_route_entry* matched = NULL;
    for (size_t i = 0; i < srv->ws_route_count; ++i) {
        if (srv->ws_routes[i].pattern_len == ctx->path.len &&
            memcmp(srv->ws_routes[i].pattern, ctx->path.ptr, ctx->path.len) == 0) {
            matched = &srv->ws_routes[i];
            break;
        }
    }
    if (!matched) return 0;

    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[200];
    memcpy(concat, key.ptr, key.len);
    memcpy(concat + key.len, kGuid, sizeof(kGuid) - 1u);
    uint8_t digest[20];
    tz_sha1((const uint8_t*)concat, key.len + sizeof(kGuid) - 1u, digest);
    char accept[32];
    size_t alen = tz_base64_encode(digest, 20u, accept);

    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %.*s\r\n\r\n", (int)alen, accept);
    tz_web_buf_append(&ctx->conn->send_buf, resp, (size_t)rlen);

    tz_web_ws_conn* ws = (tz_web_ws_conn*)calloc(1, sizeof(tz_web_ws_conn));
    ws->srv = srv;
    ws->conn = ctx->conn;
    ws->id = __atomic_fetch_add(&srv->next_ws_id, 1ULL, __ATOMIC_RELAXED) + 1ULL;
    ws->on_open = matched->on_open;
    ws->on_msg = matched->on_msg;
    ws->on_close = matched->on_close;
    tz_web_lock_init(&ws->send_lock);
    tz_web_lock_acq(&srv->ws_lock);
    ws->next = srv->ws_head;
    srv->ws_head = ws;
    tz_web_lock_rel(&srv->ws_lock);

    ctx->conn->ws = ws;
    ctx->conn->state = TZ_CONN_WS;
    ctx->conn->close_after_send = 0;
    ctx->resp_sent = 1;
    (void)consumed;
    /* Note: on_open runs after handshake response is written or immediately; safe here after send_buf queued */
    return 1;
}

static void tz_web_ws_detach_conn(tz_web_conn* c, int code) {
    tz_web_ws_conn* ws = c->ws;
    if (!ws) return;
    c->ws = NULL;
    tz_web_server* srv = ws->srv;
    int32_t was_closed = __atomic_exchange_n(&ws->closed, 1, __ATOMIC_ACQ_REL);
    if (!was_closed && ws->on_close) ws->on_close((void*)ws, (int32_t)code);
    tz_web_lock_acq(&srv->ws_lock);
    tz_web_ws_conn** pp = &srv->ws_head;
    while (*pp) {
        if (*pp == ws) { *pp = ws->next; break; }
        pp = &((*pp)->next);
    }
    tz_web_lock_rel(&srv->ws_lock);
    tz_web_lock_acq(&ws->send_lock);
    ws->conn = NULL;
    tz_web_lock_rel(&ws->send_lock);
    tz_web_lock_destroy(&ws->send_lock);
    free(ws);
}

static int tz_web_ws_process_frames(tz_web_conn* c) {
    tz_web_ws_conn* ws = c->ws;
    if (!ws) return 0;
    size_t pos = 0;
    const uint8_t* buf = (const uint8_t*)c->recv_buf.data;
    size_t len = c->recv_buf.len;
    int keep_open = 1;
    while (pos + 2u <= len) {
        uint8_t b0 = buf[pos];
        uint8_t b1 = buf[pos + 1u];
        int opcode = b0 & 0x0Fu;
        int masked = (b1 & 0x80u) != 0;
        uint64_t plen = (uint64_t)(b1 & 0x7Fu);
        size_t hsz = 2u;
        if (plen == 126u) {
            if (pos + 4u > len) break;
            plen = ((uint64_t)buf[pos + 2u] << 8) | (uint64_t)buf[pos + 3u];
            hsz = 4u;
        } else if (plen == 127u) {
            if (pos + 10u > len) break;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | (uint64_t)buf[pos + 2u + (size_t)i];
            hsz = 10u;
        }
        if (plen > TZ_WEB_MAX_BODY_BYTES) { keep_open = 0; break; }
        size_t msz = masked ? 4u : 0u;
        if (pos + hsz + msz + (size_t)plen > len) break;
        const uint8_t* mask = buf + pos + hsz;
        uint8_t* payload = (uint8_t*)(c->recv_buf.data + pos + hsz + msz);
        if (masked) {
            for (size_t i = 0; i < (size_t)plen; ++i) payload[i] ^= mask[i & 3u];
        }
        pos += hsz + msz + (size_t)plen;
        if (opcode == 0x8) {
            int code = (plen >= 2u) ? (((int)payload[0] << 8) | (int)payload[1]) : 1000;
            uint8_t close_frame[4] = { 0x88u, 2u, (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
            tz_web_buf_append(&c->send_buf, close_frame, 4u);
            c->close_after_send = 1;
            tz_web_ws_detach_conn(c, code);
            keep_open = 0;
            break;
        } else if (opcode == 0x9) {
            /* Auto Pong */
            uint8_t pong_hdr[10];
            size_t ph = 0;
            pong_hdr[ph++] = 0x8Au;
            if (plen < 126u) {
                pong_hdr[ph++] = (uint8_t)plen;
            } else {
                pong_hdr[ph++] = 126u;
                pong_hdr[ph++] = (uint8_t)(plen >> 8);
                pong_hdr[ph++] = (uint8_t)(plen & 0xFFu);
            }
            tz_web_buf_append(&c->send_buf, pong_hdr, ph);
            if (plen > 0u) tz_web_buf_append(&c->send_buf, payload, (size_t)plen);
        } else if (opcode == 0x1 || opcode == 0x2) {
            if (ws->on_msg) ws->on_msg((void*)ws, (int32_t)opcode, payload, (int64_t)plen);
        }
    }
    if (pos > 0u) {
        size_t rem = c->recv_buf.len - pos;
        if (rem > 0u) memmove(c->recv_buf.data, c->recv_buf.data + pos, rem);
        c->recv_buf.len = rem;
    }
    return keep_open;
}

void tz_web_ws_route(void* sp, const void* pat_tstr, void* on_open, void* on_msg, void* on_close) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) return;
    size_t plen = 0;
    const char* pat = tz_web_str_view(pat_tstr, &plen);
    if (srv->ws_route_count >= srv->ws_route_cap) {
        size_t nc = srv->ws_route_cap ? srv->ws_route_cap * 2u : 8u;
        srv->ws_routes = (tz_web_ws_route_entry*)realloc(srv->ws_routes, nc * sizeof(tz_web_ws_route_entry));
        srv->ws_route_cap = nc;
    }
    tz_web_ws_route_entry* e = &srv->ws_routes[srv->ws_route_count++];
    e->pattern = tz_web_dup_cstr(pat, plen);
    e->pattern_len = plen;
    e->on_open = (tz_ws_open_fn)on_open;
    e->on_msg = (tz_ws_msg_fn)on_msg;
    e->on_close = (tz_ws_close_fn)on_close;
}

int32_t tz_web_ws_send(void* wsp, int32_t opcode, const uint8_t* data, int64_t len) {
    if (len < 0) return 0;
    return tz_web_ws_write_frame((tz_web_ws_conn*)wsp, opcode, data, (size_t)len);
}

int32_t tz_web_ws_send_str(void* wsp, const void* text_tstr) {
    size_t n = 0;
    const char* p = tz_web_str_view(text_tstr, &n);
    return tz_web_ws_write_frame((tz_web_ws_conn*)wsp, 0x1, (const uint8_t*)p, n);
}

int64_t tz_web_ws_broadcast_str(void* sp, const void* text_tstr) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) return 0;
    size_t n = 0;
    const char* p = tz_web_str_view(text_tstr, &n);
    int64_t count = 0;
    tz_web_lock_acq(&srv->ws_lock);
    for (tz_web_ws_conn* w = srv->ws_head; w; w = w->next) {
        if (tz_web_ws_write_frame(w, 0x1, (const uint8_t*)p, n)) count++;
    }
    tz_web_lock_rel(&srv->ws_lock);
    return count;
}

void tz_web_ws_close(void* wsp) {
    tz_web_ws_conn* ws = (tz_web_ws_conn*)wsp;
    if (!ws) return;
    uint8_t close_frame[4] = { 0x88u, 2u, 0x03u, 0xE8u };
    tz_web_ws_write_frame(ws, 0x8, close_frame + 2, 2u);
    __atomic_store_n(&ws->closed, 1, __ATOMIC_RELEASE);
}

int64_t tz_web_ws_id(void* wsp) {
    tz_web_ws_conn* ws = (tz_web_ws_conn*)wsp;
    return ws ? (int64_t)ws->id : 0;
}
)c";

inline constexpr const char* kServerDispatchAndApi = R"c(
static int tz_web_parse_one_http(const char* buf, size_t avail, tz_web_ctx* ctx, size_t* consumed,
                                 int* out_close, int* out_http_err) {
    *consumed = 0;
    *out_http_err = 0;
    const char* hdr_end = tz_web_find_crlfcrlf(buf, avail);
    size_t sep_len = 4u;
    if (!hdr_end) {
        if (avail > TZ_WEB_MAX_HDR_BYTES) { *out_http_err = 431; return -1; }
        return 0;
    }
    size_t hdr_bytes = (size_t)(hdr_end - buf) + sep_len;
    if (hdr_bytes > TZ_WEB_MAX_HDR_BYTES) { *out_http_err = 431; return -1; }

    const char* line_end = (const char*)memchr(buf, '\n', hdr_bytes);
    if (!line_end) { *out_http_err = 400; return -1; }
    size_t rlen = (size_t)(line_end - buf);
    if (rlen > 0u && buf[rlen - 1u] == '\r') rlen--;

    int method = 0;
    const char* target = NULL;
    if (rlen >= 4u && memcmp(buf, "GET ", 4u) == 0) {
        method = TZ_M_GET;
        target = buf + 4u;
    } else if (rlen >= 5u && memcmp(buf, "POST ", 5u) == 0) {
        method = TZ_M_POST;
        target = buf + 5u;
    } else {
        const char* sp1 = (const char*)memchr(buf, ' ', rlen);
        if (!sp1 || sp1 == buf) { *out_http_err = 400; return -1; }
        method = tz_web_parse_method(buf, (size_t)(sp1 - buf));
        if (!method) { *out_http_err = 400; return -1; }
        target = sp1 + 1;
    }

    size_t rem_req = rlen - (size_t)(target - buf);
    const char* sp2 = (const char*)memchr(target, ' ', rem_req);
    if (!sp2 || sp2 == target) { *out_http_err = 400; return -1; }
    size_t target_len = (size_t)(sp2 - target);

    const char* ver = sp2 + 1;
    size_t ver_len = rlen - (size_t)(ver - buf);
    if (ver_len < 8u || memcmp(ver, "HTTP/1.", 7u) != 0) { *out_http_err = 400; return -1; }
    int is_http10 = (ver[7] == '0');

    const char* qm = (const char*)memchr(target, '?', target_len);
    ctx->method = method;
    ctx->path.ptr = target;
    ctx->path.len = qm ? (size_t)(qm - target) : target_len;
    ctx->query.ptr = qm ? (qm + 1) : "";
    ctx->query.len = qm ? (target_len - (size_t)(qm - target) - 1u) : 0u;
    ctx->hdr_count = 0;
    ctx->param_count = 0;

    int64_t content_len = -1;
    int chunked = 0;
    int conn_close = is_http10 ? 1 : 0;
    const char* p = line_end + 1;
    while (p < hdr_end) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(hdr_end - p) + 1u);
        size_t l = nl ? (size_t)(nl - p) : (size_t)(hdr_end - p);
        const char* next_p = nl ? (nl + 1) : hdr_end;
        if (l > 0u && p[l - 1u] == '\r') l--;
        if (l > 0u) {
            const char* col = (const char*)memchr(p, ':', l);
            if (!col || col == p) { *out_http_err = 400; return -1; }
            size_t nlen = (size_t)(col - p);
            const char* v = col + 1;
            size_t vlen = l - nlen - 1u;
            while (vlen > 0u && (*v == ' ' || *v == '\t')) { v++; vlen--; }
            while (vlen > 0u && (v[vlen - 1u] == ' ' || v[vlen - 1u] == '\t')) vlen--;
            if (ctx->hdr_count < TZ_WEB_MAX_HEADERS) {
                ctx->hdr_names[ctx->hdr_count].ptr = p;
                ctx->hdr_names[ctx->hdr_count].len = nlen;
                ctx->hdr_vals[ctx->hdr_count].ptr = v;
                ctx->hdr_vals[ctx->hdr_count].len = vlen;
                ctx->hdr_count++;
            }
            switch (nlen) {
                case 14u:
                    if ((((unsigned char)p[0]) | 32u) == 'c' &&
                        tz_web_eq_nocase(p, 14u, "content-length", 14u)) {
                        int64_t cl = -1;
                        if (content_len >= 0 || !tz_web_parse_i64(v, vlen, &cl) || cl < 0) {
                            *out_http_err = 400; return -1;
                        }
                        content_len = cl;
                    }
                    break;
                case 17u:
                    if ((((unsigned char)p[0]) | 32u) == 't' &&
                        tz_web_eq_nocase(p, 17u, "transfer-encoding", 17u)) {
                        if (tz_web_eq_nocase(v, vlen, "chunked", 7u)) chunked = 1;
                    }
                    break;
                case 10u:
                    if ((((unsigned char)p[0]) | 32u) == 'c' &&
                        tz_web_eq_nocase(p, 10u, "connection", 10u)) {
                        if (vlen == 5u && tz_web_eq_nocase(v, 5u, "close", 5u)) conn_close = 1;
                        else if (vlen == 10u && tz_web_eq_nocase(v, 10u, "keep-alive", 10u)) conn_close = 0;
                    }
                    break;
                default:
                    break;
            }
        }
        p = next_p;
    }
    if (chunked && content_len >= 0) { *out_http_err = 400; return -1; }
    *out_close = conn_close;

    if (chunked) {
        /* Scan chunked body after hdr_bytes */
        size_t pos = hdr_bytes;
        size_t total_body = 0;
        while (1) {
            const char* nl = (const char*)memchr(buf + pos, '\n', avail - pos);
            if (!nl) return 0;
            size_t slen = (size_t)(nl - (buf + pos));
            if (slen > 0u && buf[pos + slen - 1u] == '\r') slen--;
            size_t csz = 0;
            if (slen == 0u) { *out_http_err = 400; return -1; }
            for (size_t k = 0; k < slen; ++k) {
                char hc = buf[pos + k];
                if (hc == ';') break;
                int hv = tz_web_hexval(hc);
                if (hv < 0) { *out_http_err = 400; return -1; }
                csz = (csz << 4) | (size_t)hv;
                if (csz > TZ_WEB_MAX_BODY_BYTES) { *out_http_err = 413; return -1; }
            }
            pos = (size_t)(nl - buf) + 1u;
            if (csz == 0u) {
                if (pos + 2u > avail) return 0;
                if (buf[pos] != '\r' || buf[pos + 1u] != '\n') { *out_http_err = 400; return -1; }
                pos += 2u;
                break;
            }
            if (total_body + csz > TZ_WEB_MAX_BODY_BYTES) { *out_http_err = 413; return -1; }
            if (pos + csz + 2u > avail) return 0;
            if (buf[pos + csz] != '\r' || buf[pos + csz + 1u] != '\n') { *out_http_err = 400; return -1; }
            total_body += csz;
            pos += csz + 2u;
        }
        /* Unchunk in-place at buf + hdr_bytes */
        char* w = (char*)(buf + hdr_bytes);
        size_t rpos = hdr_bytes;
        size_t out_len = 0;
        while (1) {
            const char* nl = (const char*)memchr(buf + rpos, '\n', pos - rpos);
            size_t slen = (size_t)(nl - (buf + rpos));
            if (slen > 0u && buf[rpos + slen - 1u] == '\r') slen--;
            size_t csz = 0;
            for (size_t k = 0; k < slen; ++k) {
                if (buf[rpos + k] == ';') break;
                csz = (csz << 4) | (size_t)tz_web_hexval(buf[rpos + k]);
            }
            rpos = (size_t)(nl - buf) + 1u;
            if (csz == 0u) break;
            if (w + out_len != buf + rpos) memmove(w + out_len, buf + rpos, csz);
            out_len += csz;
            rpos += csz + 2u;
        }
        ctx->body.ptr = w;
        ctx->body.len = out_len;
        *consumed = pos;
        return 1;
    }

    size_t blen = content_len > 0 ? (size_t)content_len : 0u;
    if (blen > TZ_WEB_MAX_BODY_BYTES) { *out_http_err = 413; return -1; }
    if (avail < hdr_bytes + blen) return 0;
    ctx->body.ptr = blen ? (buf + hdr_bytes) : "";
    ctx->body.len = blen;
    *consumed = hdr_bytes + blen;
    return 1;
}
)c";

inline constexpr const char* kServerDispatchLoop = R"c(
static void tz_web_flush_plain_blocking(tz_web_sock_t sock, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(sock, data + sent, (int)(len - sent), 0);
        if (n <= 0) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set wf; FD_ZERO(&wf); FD_SET(sock, &wf);
                struct timeval tv = { 2, 0 };
                if (select(0, NULL, &wf, NULL, &tv) > 0) continue;
            }
            break;
        }
#else
        ssize_t n = send(sock, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { sock, POLLOUT, 0 };
                if (poll(&pfd, 1, 2000) > 0) continue;
            }
            break;
        }
        if (n == 0) break;
#endif
        sent += (size_t)n;
    }
}

static void tz_web_flush_101_blocking(tz_web_conn* c) {
    if (c->send_buf.len == 0u) return;
#ifdef TZ_USE_TLS
    if (__builtin_expect(c->ssl != NULL, 0)) {
        tz_web_buf wire;
        tz_web_buf_init(&wire, 512u);
        tz_web_lock_acq(&c->tls_lock);
        if (c->tls_wire_out.len > c->tls_wire_off) {
            tz_web_buf_append(&wire, c->tls_wire_out.data + c->tls_wire_off,
                              c->tls_wire_out.len - c->tls_wire_off);
            c->tls_wire_out.len = 0;
            c->tls_wire_off = 0;
        }
        tz_tls_encrypt_to_buf(c, c->send_buf.data, c->send_buf.len, &wire);
        tz_web_lock_rel(&c->tls_lock);
        if (wire.len > 0u) tz_web_flush_plain_blocking(c->sock, wire.data, wire.len);
        tz_web_buf_free(&wire);
        c->send_buf.len = 0;
        c->send_off = 0;
        return;
    }
#endif
    tz_web_flush_plain_blocking(c->sock, c->send_buf.data, c->send_buf.len);
    c->send_buf.len = 0;
    c->send_off = 0;
}

static void tz_web_dispatch_conn_http(tz_web_conn* c) {
    tz_web_server* srv = c->srv;
    c->cur_date_slot = tz_web_date_refresh_slot();
    uint64_t batch_reqs = 0;
    while (c->parse_pos < c->recv_buf.len) {
        tz_web_ctx ctx;
        ctx.conn = c;
        ctx.method = 0;
        ctx.path.ptr = "";
        ctx.path.len = 0;
        ctx.query.ptr = "";
        ctx.query.len = 0;
        ctx.body.ptr = "";
        ctx.body.len = 0;
        ctx.hdr_count = 0;
        ctx.param_count = 0;
        ctx.resp_status = 0;
        ctx.resp_sent = 0;
        ctx.json_active = 0;
        ctx.json_fields = 0;
        size_t consumed = 0;
        int close_req = 0;
        int http_err = 0;
        int pr = tz_web_parse_one_http(c->recv_buf.data + c->parse_pos,
                                       c->recv_buf.len - c->parse_pos,
                                       &ctx, &consumed, &close_req, &http_err);
        if (pr == 0) break;
        if (pr < 0) {
            c->close_after_send = 1;
            tz_web_write_http_resp(c, http_err ? http_err : 400,
                                   "text/plain; charset=utf-8", 25u,
                                   tz_web_reason(http_err ? http_err : 400),
                                   strlen(tz_web_reason(http_err ? http_err : 400)),
                                   NULL, 0u, 1, 0);
            c->parse_pos = c->recv_buf.len;
            break;
        }
        c->parse_pos += consumed;
        c->close_after_send = close_req;
        c->extra_hdrs.len = 0;
        c->body_buf.len = 0;
        batch_reqs++;

        if (srv->ws_route_count != 0u && tz_web_try_ws_upgrade(srv, &ctx, consumed)) {
            tz_web_ws_conn* ws = c->ws;
            /* Flush 101 handshake before on_open/frames so frames never precede 101 */
            if (c->send_buf.len > 0u) {
                tz_web_flush_101_blocking(c);
            }
            /* Shift remaining bytes if client sent frame immediately */
            size_t rem = c->recv_buf.len - c->parse_pos;
            if (rem > 0u) memmove(c->recv_buf.data, c->recv_buf.data + c->parse_pos, rem);
            c->recv_buf.len = rem;
            c->parse_pos = 0;
            if (ws && ws->on_open) ws->on_open((void*)ws);
            if (c->recv_buf.len > 0u) tz_web_ws_process_frames(c);
            break;
        }

        int mw_ok = 1;
        if (srv->mw_before) {
            mw_ok = srv->mw_before((void*)&ctx);
            if (!mw_ok) tz_web_ctx_finish_default(&ctx);
        }

        if (mw_ok) {
            /* 1. O(1) inline hash slot lookup */
            uint64_t ph = tz_web_hash(ctx.path.ptr, ctx.path.len);
            uint64_t kh = ph ^ ((uint64_t)ctx.method * 0x9e3779b97f4a7c15ULL);
            const tz_hash_slot* found = NULL;
            if (srv->hash_slots) {
                size_t idx = (size_t)(kh & srv->hash_mask);
                while (srv->hash_slots[idx].occupied) {
                    const tz_hash_slot* cand = &srv->hash_slots[idx];
                    if (cand->hash == ph && cand->method == ctx.method &&
                        cand->pattern_len == ctx.path.len &&
                        (ctx.path.len == 0u || memcmp(cand->pattern, ctx.path.ptr, ctx.path.len) == 0)) {
                        found = cand;
                        break;
                    }
                    idx = (idx + 1u) & srv->hash_mask;
                }
            }
            if (found) {
                if (found->is_static_resp) {
                    const char* date_p = tz_web_date_cache.text[c->cur_date_slot & 1u];
                    if (c->extra_hdrs.len == 0u && !close_req) {
                        size_t need = (size_t)found->static_prefix_len + 29u + (size_t)found->static_suffix_len;
                        if (tz_web_buf_reserve(&c->send_buf, need)) {
                            char* dst = c->send_buf.data + c->send_buf.len;
                            memcpy(dst, found->static_prefix, found->static_prefix_len);
                            memcpy(dst + found->static_prefix_len, date_p, 29u);
                            memcpy(dst + found->static_prefix_len + 29u, found->static_suffix, found->static_suffix_len);
                            c->send_buf.len += need;
                        }
                    } else {
                        size_t body_len_s = found->static_suffix_len >= 4u ? (found->static_suffix_len - 4u) : 0u;
                        tz_web_buf_append(&c->send_buf, found->static_prefix, found->static_prefix_len);
                        tz_web_buf_append(&c->send_buf, date_p, 29u);
                        tz_web_buf_append(&c->send_buf, "\r\n", 2u);
                        if (c->extra_hdrs.len > 0u) tz_web_buf_append(&c->send_buf, c->extra_hdrs.data, c->extra_hdrs.len);
                        if (close_req) tz_web_buf_append(&c->send_buf, "Connection: close\r\n", 19u);
                        tz_web_buf_append(&c->send_buf, "\r\n", 2u);
                        if (body_len_s > 0u && ctx.method != TZ_M_HEAD) {
                            tz_web_buf_append(&c->send_buf, found->static_suffix + 4u, body_len_s);
                        }
                    }
                    ctx.resp_sent = 1;
                } else {
                    if (found->fn) found->fn((void*)&ctx);
                    tz_web_ctx_finish_default(&ctx);
                }
            } else {
                /* 2. Parametric/wildcard routes */
                int matched = 0;
                int path_matched_other_method = 0;
                for (size_t pi = 0; pi < srv->param_count; ++pi) {
                    tz_web_route_entry* pr_ent = srv->param_routes[pi];
                    if (tz_web_match_param_route(pr_ent, ctx.path.ptr, ctx.path.len, &ctx)) {
                        if (pr_ent->method == ctx.method) {
                            matched = 1;
                            if (pr_ent->fn) pr_ent->fn((void*)&ctx);
                            tz_web_ctx_finish_default(&ctx);
                            break;
                        } else {
                            path_matched_other_method = 1;
                        }
                    }
                }
                if (!matched && !path_matched_other_method) {
                    for (size_t ri = 0; ri < srv->route_count; ++ri) {
                        tz_web_route_entry* r = &srv->routes[ri];
                        if (!r->is_param && r->method != ctx.method && r->hash == ph &&
                            r->pattern_len == ctx.path.len &&
                            memcmp(r->pattern, ctx.path.ptr, ctx.path.len) == 0) {
                            path_matched_other_method = 1;
                            break;
                        }
                    }
                }
                if (!matched) {
                    if (tz_web_try_static_mounts(srv, &ctx)) {
                        matched = 1;
                    } else if (path_matched_other_method) {
                        tz_web_write_http_resp(c, 405, "text/plain; charset=utf-8", 25u,
                                               "Method Not Allowed", 18u,
                                               c->extra_hdrs.data, c->extra_hdrs.len,
                                               close_req, ctx.method == TZ_M_HEAD);
                        ctx.resp_sent = 1;
                    } else {
                        tz_web_write_http_resp(c, 404, "text/plain; charset=utf-8", 25u,
                                               "Not Found", 9u,
                                               c->extra_hdrs.data, c->extra_hdrs.len,
                                               close_req, ctx.method == TZ_M_HEAD);
                        ctx.resp_sent = 1;
                    }
                }
            }
        }
        if (srv->mw_after) srv->mw_after((void*)&ctx);
        if (c->close_after_send) break;
    }
    if (batch_reqs > 0ULL) {
        __atomic_fetch_add(&srv->reqs_total, batch_reqs, __ATOMIC_RELAXED);
    }
    if (c->state == TZ_CONN_HTTP && c->parse_pos > 0u) {
        size_t rem = c->recv_buf.len - c->parse_pos;
        if (rem > 0u) memmove(c->recv_buf.data, c->recv_buf.data + c->parse_pos, rem);
        c->recv_buf.len = rem;
        c->parse_pos = 0;
    }
}

static tz_web_conn* tz_web_conn_acquire(tz_web_server* srv, tz_web_sock_t sock) {
    tz_web_lock_acq(&srv->conn_lock);
    if (srv->free_top == 0u) {
        tz_web_lock_rel(&srv->conn_lock);
        return NULL;
    }
    uint32_t idx = srv->free_stack[--srv->free_top];
    tz_web_lock_rel(&srv->conn_lock);
    tz_web_conn* c = &srv->conns[idx];
    c->sock = sock;
    c->state = TZ_CONN_HTTP;
    c->close_after_send = 0;
    c->parse_pos = 0;
    c->send_off = 0;
    c->ws = NULL;
    if (!c->recv_buf.data) tz_web_buf_init(&c->recv_buf, TZ_WEB_RECV_INIT);
    if (!c->send_buf.data) tz_web_buf_init(&c->send_buf, TZ_WEB_SEND_INIT);
    if (!c->extra_hdrs.data) tz_web_buf_init(&c->extra_hdrs, TZ_WEB_HDR_INIT);
    if (!c->body_buf.data) tz_web_buf_init(&c->body_buf, TZ_WEB_BODY_INIT);
    c->recv_buf.len = 0;
    c->send_buf.len = 0;
    c->extra_hdrs.len = 0;
    c->body_buf.len = 0;
    c->cur_date_slot = 0;
#ifdef TZ_USE_TLS
    c->ssl = NULL;
    c->rbio = NULL;
    c->wbio = NULL;
    c->tls_established = 0;
    c->tls_wire_out.len = 0;
    c->tls_wire_off = 0;
    c->tls_raw_in.len = 0;
    if (__builtin_expect(srv->tls_ctx != NULL, 0)) {
        if (!c->tls_wire_out.data) tz_web_buf_init(&c->tls_wire_out, TZ_WEB_SEND_INIT);
        if (!c->tls_raw_in.data) tz_web_buf_init(&c->tls_raw_in, TZ_WEB_RECV_INIT);
        void* ssl = tz_tls_api.ssl_new(srv->tls_ctx);
        void* rb = ssl ? tz_tls_api.bio_new(tz_tls_api.bio_s_mem()) : NULL;
        void* wb = (ssl && rb) ? tz_tls_api.bio_new(tz_tls_api.bio_s_mem()) : NULL;
        if (!ssl || !rb || !wb) {
            if (wb) tz_tls_api.bio_free(wb);
            if (rb) tz_tls_api.bio_free(rb);
            if (ssl) tz_tls_api.ssl_free(ssl);
            tz_web_lock_acq(&srv->conn_lock);
            srv->free_stack[srv->free_top++] = idx;
            tz_web_lock_rel(&srv->conn_lock);
            return NULL;
        }
        tz_tls_api.ssl_set_bio(ssl, rb, wb);
        tz_tls_api.ssl_set_accept(ssl);
        c->ssl = ssl;
        c->rbio = rb;
        c->wbio = wb;
    }
#endif
    __atomic_fetch_add(&srv->active_conns, 1ULL, __ATOMIC_RELAXED);
    return c;
}

static void tz_web_conn_release(tz_web_conn* c) {
    tz_web_server* srv = c->srv;
    if (c->ws) tz_web_ws_detach_conn(c, 1006);
#ifdef TZ_USE_TLS
    if (c->ssl) {
        tz_tls_api.ssl_free(c->ssl);
        c->ssl = NULL;
        c->rbio = NULL;
        c->wbio = NULL;
    }
    c->tls_established = 0;
    c->tls_wire_out.len = 0;
    c->tls_wire_off = 0;
    c->tls_raw_in.len = 0;
    tz_web_buf_shrink(&c->tls_wire_out, 65536u);
    tz_web_buf_shrink(&c->tls_raw_in, 65536u);
#endif
    if (c->sock != TZ_WEB_BAD_SOCK) {
        tz_web_sock_close(c->sock);
        c->sock = TZ_WEB_BAD_SOCK;
    }
    c->state = TZ_CONN_FREE;
    tz_web_buf_shrink(&c->recv_buf, 65536u);
    tz_web_buf_shrink(&c->send_buf, 65536u);
    tz_web_buf_shrink(&c->extra_hdrs, 8192u);
    tz_web_buf_shrink(&c->body_buf, 32768u);
    __atomic_fetch_sub(&srv->active_conns, 1ULL, __ATOMIC_RELAXED);
    tz_web_lock_acq(&srv->conn_lock);
    srv->free_stack[srv->free_top++] = c->slot_idx;
    tz_web_lock_rel(&srv->conn_lock);
}
)c";

inline constexpr const char* kServerTls = R"c(
#if defined(TZ_USE_TLS) && defined(TZ_HAS_SERVER)
static int tz_tls_feed_raw(tz_web_conn* c, const char* raw, size_t len, int* out_eof) {
    *out_eof = 0;
    tz_web_lock_acq(&c->tls_lock);
    size_t woff = 0u;
    while (woff < len) {
        size_t rem = len - woff;
        int chunk = rem > 1048576u ? 1048576 : (int)rem;
        int bw = tz_tls_api.bio_write(c->rbio, raw + woff, chunk);
        if (bw <= 0) { tz_web_lock_rel(&c->tls_lock); return 0; }
        woff += (size_t)bw;
    }
    if (!c->tls_established) {
        int rc = tz_tls_api.ssl_accept(c->ssl);
        tz_tls_drain_wbio_to(c, &c->tls_wire_out);
        if (rc == 1) {
            c->tls_established = 1;
        } else {
            int err = tz_tls_api.ssl_get_error(c->ssl, rc);
            if (err != TZ_SSL_ERROR_WANT_READ && err != TZ_SSL_ERROR_WANT_WRITE) {
                if (c->tls_wire_out.len > c->tls_wire_off) {
                    c->close_after_send = 1;
                    tz_web_lock_rel(&c->tls_lock);
                    return 1;
                }
                tz_web_lock_rel(&c->tls_lock);
                return 0;
            }
        }
    }
    if (c->tls_established) {
        char pt[16384];
        while (1) {
            int r = tz_tls_api.ssl_read(c->ssl, pt, (int)sizeof(pt));
            tz_tls_drain_wbio_to(c, &c->tls_wire_out);
            if (r > 0) {
                if (!tz_web_buf_append(&c->recv_buf, pt, (size_t)r)) {
                    tz_web_lock_rel(&c->tls_lock);
                    return 0;
                }
                continue;
            }
            int err = tz_tls_api.ssl_get_error(c->ssl, r);
            if (err == TZ_SSL_ERROR_ZERO_RETURN) { *out_eof = 1; break; }
            if (err == TZ_SSL_ERROR_WANT_READ || err == TZ_SSL_ERROR_WANT_WRITE) break;
            if (c->tls_wire_out.len > c->tls_wire_off) {
                c->close_after_send = 1;
                tz_web_lock_rel(&c->tls_lock);
                return 1;
            }
            tz_web_lock_rel(&c->tls_lock);
            return 0;
        }
    }
    tz_web_lock_rel(&c->tls_lock);

    int ws_keep = 1;
    if (c->recv_buf.len > 0u) {
        if (c->state == TZ_CONN_WS) {
            ws_keep = tz_web_ws_process_frames(c);
        } else {
            tz_web_dispatch_conn_http(c);
        }
    }

    tz_web_lock_acq(&c->tls_lock);
    if (c->send_buf.len > c->send_off) {
        int enc_ok = tz_tls_encrypt_to_buf(c, c->send_buf.data + c->send_off,
                                           c->send_buf.len - c->send_off, &c->tls_wire_out);
        c->send_buf.len = 0;
        c->send_off = 0;
        if (!enc_ok) { tz_web_lock_rel(&c->tls_lock); return 0; }
    }
    tz_tls_drain_wbio_to(c, &c->tls_wire_out);
    tz_web_lock_rel(&c->tls_lock);

    if (c->state == TZ_CONN_WS && !ws_keep) {
        if (c->tls_wire_out.len > c->tls_wire_off) c->close_after_send = 1;
        else return 0;
    }
    return 1;
}
#endif
)c";

inline constexpr const char* kServerPlatformWin = R"c(
#ifdef _WIN32
#define TZ_WAKE_KEY ((ULONG_PTR)1)
#define TZ_ACCEPT_KEY ((ULONG_PTR)2)
#define TZ_CONN_KEY ((ULONG_PTR)3)

static int tz_win_post_accept(tz_web_server* srv, tz_accept_slot* slot) {
    if (!__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE)) return 0;
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) return 0;
    memset(&slot->tag.ov, 0, sizeof(OVERLAPPED));
    slot->tag.kind = TZ_IO_ACCEPT;
    slot->accept_sock = s;
    DWORD bytes = 0;
    DWORD alen = (DWORD)(sizeof(struct sockaddr_storage) + 16);
    __atomic_fetch_add(&srv->pending_accepts, 1, __ATOMIC_ACQ_REL);
    BOOL ok = srv->fn_acceptex(srv->listen_sock, s, slot->addr_buf, 0, alen, alen, &bytes, &slot->tag.ov);
    if (!ok) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            closesocket(s);
            slot->accept_sock = INVALID_SOCKET;
            __atomic_fetch_sub(&srv->pending_accepts, 1, __ATOMIC_ACQ_REL);
            return 0;
        }
    }
    return 1;
}

static void tz_win_drive_conn(tz_web_conn* c, int from_completion, DWORD xfer_bytes, int io_err) {
    tz_web_server* srv = c->srv;
    int need_recv = 0;
    int need_send = 0;

    if (from_completion) {
        if (io_err != 0 || !__atomic_load_n(&srv->running, __ATOMIC_RELAXED)) {
            tz_web_conn_release(c);
            return;
        }
        if (c->io_tag.kind == TZ_IO_RECV) {
            if (xfer_bytes == 0) {
                tz_web_conn_release(c);
                return;
            }
#ifdef TZ_USE_TLS
            if (__builtin_expect(c->ssl != NULL, 0)) {
                c->tls_raw_in.len += (size_t)xfer_bytes;
                int tls_eof = 0;
                int ok = tz_tls_feed_raw(c, c->tls_raw_in.data, c->tls_raw_in.len, &tls_eof);
                c->tls_raw_in.len = 0;
                if (!ok) { tz_web_conn_release(c); return; }
                if (tls_eof) c->close_after_send = 1;
                if (c->tls_wire_out.len > c->tls_wire_off) need_send = 1;
                else if (c->close_after_send) { tz_web_conn_release(c); return; }
                else need_recv = 1;
            } else
#endif
            {
                c->recv_buf.len += (size_t)xfer_bytes;
                if (c->state == TZ_CONN_WS) {
                    int keep = tz_web_ws_process_frames(c);
                    if (c->send_buf.len > 0u) need_send = 1;
                    else if (keep) need_recv = 1;
                    else { tz_web_conn_release(c); return; }
                } else {
                    tz_web_dispatch_conn_http(c);
                    if (c->send_buf.len > 0u) need_send = 1;
                    else if (c->close_after_send) { tz_web_conn_release(c); return; }
                    else need_recv = 1;
                }
            }
        } else if (c->io_tag.kind == TZ_IO_SEND) {
            tz_web_buf* tx_buf = &c->send_buf;
            size_t* tx_off = &c->send_off;
#ifdef TZ_USE_TLS
            if (__builtin_expect(c->ssl != NULL, 0)) {
                tx_buf = &c->tls_wire_out;
                tx_off = &c->tls_wire_off;
            }
#endif
            *tx_off += (size_t)xfer_bytes;
            if (*tx_off < tx_buf->len) {
                need_send = 1;
            } else {
                tx_buf->len = 0;
                *tx_off = 0;
                if (c->close_after_send) {
                    tz_web_conn_release(c);
                    return;
                }
                need_recv = 1;
            }
        }
    } else {
        tz_web_buf* tx_buf = &c->send_buf;
        size_t tx_off = c->send_off;
#ifdef TZ_USE_TLS
        if (__builtin_expect(c->ssl != NULL, 0)) {
            tx_buf = &c->tls_wire_out;
            tx_off = c->tls_wire_off;
        }
#endif
        if (tx_buf->len > tx_off) need_send = 1;
        else need_recv = 1;
    }

    for (int iter = 0; iter < 64; ++iter) {
        if (!__atomic_load_n(&srv->running, __ATOMIC_RELAXED)) {
            tz_web_conn_release(c);
            return;
        }
        if (need_send) {
            need_send = 0;
            tz_web_buf* tx_buf = &c->send_buf;
            size_t* tx_off = &c->send_off;
#ifdef TZ_USE_TLS
            if (__builtin_expect(c->ssl != NULL, 0)) {
                tx_buf = &c->tls_wire_out;
                tx_off = &c->tls_wire_off;
            }
#endif
            while (*tx_off < tx_buf->len) {
                WSABUF wbuf;
                size_t rem = tx_buf->len - *tx_off;
                wbuf.buf = tx_buf->data + *tx_off;
                wbuf.len = (ULONG)(rem > 1048576u ? 1048576u : rem);
                memset(&c->io_tag.ov, 0, sizeof(OVERLAPPED));
                c->io_tag.kind = TZ_IO_SEND;
                DWORD sent = 0;
                int rc = WSASend(c->sock, &wbuf, 1, &sent, 0, &c->io_tag.ov, NULL);
                if (rc == 0) {
                    if (!c->skip_on_sync) return;
                    *tx_off += (size_t)sent;
                    if (sent == 0) { tz_web_conn_release(c); return; }
                } else {
                    int err = WSAGetLastError();
                    if (err == WSA_IO_PENDING) return;
                    tz_web_conn_release(c);
                    return;
                }
            }
            tx_buf->len = 0;
            *tx_off = 0;
            if (c->close_after_send) {
                tz_web_conn_release(c);
                return;
            }
            need_recv = 1;
        }
        if (need_recv) {
            need_recv = 0;
            tz_web_buf* rx_buf = &c->recv_buf;
#ifdef TZ_USE_TLS
            if (__builtin_expect(c->ssl != NULL, 0)) rx_buf = &c->tls_raw_in;
#endif
            if (!tz_web_buf_reserve(rx_buf, 8192u)) {
                tz_web_conn_release(c);
                return;
            }
            WSABUF rbuf;
            rbuf.buf = rx_buf->data + rx_buf->len;
            rbuf.len = (ULONG)(rx_buf->cap - rx_buf->len);
            if (rbuf.len > 65536u) rbuf.len = 65536u;
            memset(&c->io_tag.ov, 0, sizeof(OVERLAPPED));
            c->io_tag.kind = TZ_IO_RECV;
            DWORD recvd = 0;
            DWORD flags = 0;
            int rc = WSARecv(c->sock, &rbuf, 1, &recvd, &flags, &c->io_tag.ov, NULL);
            if (rc == 0) {
                if (!c->skip_on_sync) return;
                if (recvd == 0) { tz_web_conn_release(c); return; }
#ifdef TZ_USE_TLS
                if (__builtin_expect(c->ssl != NULL, 0)) {
                    c->tls_raw_in.len += (size_t)recvd;
                    int tls_eof = 0;
                    int ok = tz_tls_feed_raw(c, c->tls_raw_in.data, c->tls_raw_in.len, &tls_eof);
                    c->tls_raw_in.len = 0;
                    if (!ok) { tz_web_conn_release(c); return; }
                    if (tls_eof) c->close_after_send = 1;
                    if (c->tls_wire_out.len > c->tls_wire_off) need_send = 1;
                    else if (c->close_after_send) { tz_web_conn_release(c); return; }
                    else need_recv = 1;
                } else
#endif
                {
                    c->recv_buf.len += (size_t)recvd;
                    if (c->state == TZ_CONN_WS) {
                        int keep = tz_web_ws_process_frames(c);
                        if (c->send_buf.len > 0u) need_send = 1;
                        else if (keep) need_recv = 1;
                        else { tz_web_conn_release(c); return; }
                    } else {
                        tz_web_dispatch_conn_http(c);
                        if (c->send_buf.len > 0u) need_send = 1;
                        else if (c->close_after_send) { tz_web_conn_release(c); return; }
                        else need_recv = 1;
                    }
                }
            } else {
                int err = WSAGetLastError();
                if (err == WSA_IO_PENDING) return;
                tz_web_conn_release(c);
                return;
            }
        }
    }
    /* Re-queue cleanly after 64 synchronous turns to avoid starving other sockets */
    if (need_send || need_recv) {
        memset(&c->io_tag.ov, 0, sizeof(OVERLAPPED));
        c->io_tag.kind = TZ_IO_YIELD;
        if (!PostQueuedCompletionStatus(srv->iocp, 0, TZ_CONN_KEY, &c->io_tag.ov)) {
            tz_web_conn_release(c);
        }
    }
}
)c";

inline constexpr const char* kServerWinEngine = R"c(
static DWORD WINAPI tz_win_worker(LPVOID param) {
    tz_web_server* srv = (tz_web_server*)param;
    OVERLAPPED_ENTRY entries[64];
    while (1) {
        ULONG count = 0;
        BOOL ok = GetQueuedCompletionStatusEx(srv->iocp, entries, 64, &count, 250, FALSE);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == WAIT_TIMEOUT) {
                if (!__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE) &&
                    __atomic_load_n(&srv->pending_accepts, __ATOMIC_ACQUIRE) == 0 &&
                    __atomic_load_n(&srv->active_conns, __ATOMIC_ACQUIRE) == 0) {
                    PostQueuedCompletionStatus(srv->iocp, 0, TZ_WAKE_KEY, NULL);
                    break;
                }
                continue;
            }
            break;
        }
        for (ULONG i = 0; i < count; ++i) {
            OVERLAPPED_ENTRY* e = &entries[i];
            if (e->lpCompletionKey == TZ_WAKE_KEY || !e->lpOverlapped) {
                if (!__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE) &&
                    __atomic_load_n(&srv->pending_accepts, __ATOMIC_ACQUIRE) == 0 &&
                    __atomic_load_n(&srv->active_conns, __ATOMIC_ACQUIRE) == 0) {
                    PostQueuedCompletionStatus(srv->iocp, 0, TZ_WAKE_KEY, NULL);
                    return 0;
                }
                continue;
            }
            tz_io_ov* tag = (tz_io_ov*)e->lpOverlapped;
            if (e->lpCompletionKey == TZ_ACCEPT_KEY && tag->kind == TZ_IO_ACCEPT) {
                tz_accept_slot* slot = &srv->accept_slots[tag->slot_idx];
                SOCKET cli = slot->accept_sock;
                slot->accept_sock = INVALID_SOCKET;
                __atomic_fetch_sub(&srv->pending_accepts, 1, __ATOMIC_ACQ_REL);
                DWORD status = (DWORD)e->Internal;
                if (status == 0 && __atomic_load_n(&srv->running, __ATOMIC_ACQUIRE)) {
                    setsockopt(cli, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                               (const char*)&srv->listen_sock, sizeof(srv->listen_sock));
                    int nodelay = 1;
                    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
                    tz_web_conn* c = tz_web_conn_acquire(srv, cli);
                    if (c) {
                        if (CreateIoCompletionPort((HANDLE)cli, srv->iocp, TZ_CONN_KEY, 0)) {
                            c->skip_on_sync = SetFileCompletionNotificationModes(
                                (HANDLE)cli, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE) ? 1 : 0;
                            tz_win_post_accept(srv, slot);
                            tz_win_drive_conn(c, 0, 0, 0);
                            continue;
                        }
                        tz_web_conn_release(c);
                    } else {
                        closesocket(cli);
                    }
                } else {
                    if (cli != INVALID_SOCKET) closesocket(cli);
                }
                if (__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE)) tz_win_post_accept(srv, slot);
            } else if (e->lpCompletionKey == TZ_CONN_KEY) {
                tz_web_conn* c = &srv->conns[tag->slot_idx];
                int io_err = (e->Internal != 0) ? 1 : 0;
                if (tag->kind == TZ_IO_YIELD) {
                    if (!__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE)) {
                        tz_web_conn_release(c);
                    } else {
                        tz_win_drive_conn(c, 0, 0, 0);
                    }
                } else {
                    tz_win_drive_conn(c, 1, e->dwNumberOfBytesTransferred, io_err);
                }
            }
        }
        if (!__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&srv->pending_accepts, __ATOMIC_ACQUIRE) == 0 &&
            __atomic_load_n(&srv->active_conns, __ATOMIC_ACQUIRE) == 0) {
            PostQueuedCompletionStatus(srv->iocp, 0, TZ_WAKE_KEY, NULL);
            break;
        }
    }
    return 0;
}

static int tz_win_start_engine(tz_web_server* srv, const void** err_ptr, int64_t* err_len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char pstr[16];
    snprintf(pstr, sizeof(pstr), "%d", srv->req_port);
    if (getaddrinfo(srv->host && srv->host[0] ? srv->host : "0.0.0.0", pstr, &hints, &res) != 0 || !res) {
        tz_web_set_err(err_ptr, err_len, "No se pudo resolver host/puerto");
        return 0;
    }
    SOCKET ls = WSASocketW(res->ai_family, res->ai_socktype, res->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (ls == INVALID_SOCKET) { freeaddrinfo(res); tz_web_set_err(err_ptr, err_len, "Error creando socket"); return 0; }
    int reuse = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    if (bind(ls, res->ai_addr, (int)res->ai_addrlen) != 0 || listen(ls, SOMAXCONN) != 0) {
        freeaddrinfo(res); closesocket(ls); tz_web_set_err(err_ptr, err_len, "Error en bind/listen"); return 0;
    }
    freeaddrinfo(res);
    struct sockaddr_in sin;
    int slen = sizeof(sin);
    if (getsockname(ls, (struct sockaddr*)&sin, &slen) == 0) srv->bound_port = (int)ntohs(sin.sin_port);

    GUID guid = WSAID_ACCEPTEX;
    DWORD bytes = 0;
    if (WSAIoctl(ls, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                 &srv->fn_acceptex, sizeof(srv->fn_acceptex), &bytes, NULL, NULL) != 0) {
        closesocket(ls); tz_web_set_err(err_ptr, err_len, "AcceptEx no disponible"); return 0;
    }
    srv->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, (DWORD)srv->num_workers);
    if (!srv->iocp || !CreateIoCompletionPort((HANDLE)ls, srv->iocp, TZ_ACCEPT_KEY, 0)) {
        if (srv->iocp) CloseHandle(srv->iocp);
        srv->iocp = NULL;
        closesocket(ls);
        tz_web_set_err(err_ptr, err_len, "Error creando IOCP");
        return 0;
    }
    srv->listen_sock = ls;
    __atomic_store_n(&srv->running, 1, __ATOMIC_RELEASE);
    srv->accept_count = 64;
    srv->accept_slots = (tz_accept_slot*)calloc((size_t)srv->accept_count, sizeof(tz_accept_slot));
    for (int i = 0; i < srv->accept_count; ++i) {
        srv->accept_slots[i].tag.slot_idx = (uint32_t)i;
        srv->accept_slots[i].accept_sock = INVALID_SOCKET;
        tz_win_post_accept(srv, &srv->accept_slots[i]);
    }
    srv->worker_threads = (HANDLE*)calloc((size_t)srv->num_workers, sizeof(HANDLE));
    for (int i = 0; i < srv->num_workers; ++i) {
        srv->worker_threads[i] = CreateThread(NULL, 0, tz_win_worker, srv, 0, NULL);
    }
    return 1;
}

static void tz_win_stop_engine(tz_web_server* srv) {
    __atomic_store_n(&srv->running, 0, __ATOMIC_RELEASE);
    if (srv->listen_sock != INVALID_SOCKET) {
        closesocket(srv->listen_sock);
        srv->listen_sock = INVALID_SOCKET;
    }
    for (uint32_t i = 0; i < srv->conn_cap; ++i) {
        SOCKET s = srv->conns[i].sock;
        if (srv->conns[i].state != TZ_CONN_FREE && s != INVALID_SOCKET) {
            CancelIoEx((HANDLE)s, NULL);
            shutdown(s, SD_BOTH);
        }
    }
    if (srv->iocp) {
        for (int i = 0; i < srv->num_workers; ++i) {
            PostQueuedCompletionStatus(srv->iocp, 0, TZ_WAKE_KEY, NULL);
        }
    }
    if (srv->worker_threads) {
        for (int i = 0; i < srv->num_workers; ++i) {
            if (srv->worker_threads[i]) {
                WaitForSingleObject(srv->worker_threads[i], 5000);
                CloseHandle(srv->worker_threads[i]);
            }
        }
        free(srv->worker_threads);
        srv->worker_threads = NULL;
    }
    for (uint32_t i = 0; i < srv->conn_cap; ++i) {
        if (srv->conns[i].state != TZ_CONN_FREE) {
            srv->conns[i].sock = INVALID_SOCKET;
            tz_web_conn_release(&srv->conns[i]);
        }
    }
    if (srv->accept_slots) {
        for (int i = 0; i < srv->accept_count; ++i) {
            if (srv->accept_slots[i].accept_sock != INVALID_SOCKET) {
                closesocket(srv->accept_slots[i].accept_sock);
            }
        }
        free(srv->accept_slots);
        srv->accept_slots = NULL;
    }
    if (srv->iocp) {
        CloseHandle(srv->iocp);
        srv->iocp = NULL;
    }
}
#endif
)c";

inline constexpr const char* kServerPlatformPosix = R"c(
#ifndef _WIN32
#ifdef __linux__
static tz_web_sock_t tz_linux_make_listen(tz_web_server* srv, int port, int* out_port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char pstr[16];
    snprintf(pstr, sizeof(pstr), "%d", port);
    if (getaddrinfo(srv->host && srv->host[0] ? srv->host : "0.0.0.0", pstr, &hints, &res) != 0 || !res)
        return -1;
    int fd = socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 || listen(fd, 4096) != 0) {
        freeaddrinfo(res); close(fd); return -1;
    }
    freeaddrinfo(res);
    if (out_port) {
        struct sockaddr_in sin;
        socklen_t sl = sizeof(sin);
        if (getsockname(fd, (struct sockaddr*)&sin, &sl) == 0) *out_port = (int)ntohs(sin.sin_port);
    }
    return fd;
}

static int tz_linux_mod_ep(int epfd, int fd, void* ptr, uint32_t ev, int op) {
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = ev;
    e.data.ptr = ptr;
    return epoll_ctl(epfd, op, fd, &e);
}

static void* tz_linux_worker(void* arg) {
    tz_epoll_worker* w = (tz_epoll_worker*)arg;
    tz_web_server* srv = w->srv;
    struct epoll_event events[128];
    while (__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE)) {
        int n = epoll_wait(w->epfd, events, 128, 250);
        for (int i = 0; i < n; ++i) {
            void* tag = events[i].data.ptr;
            uint32_t ev = events[i].events;
            if (tag == (void*)&w->evfd) {
                uint64_t val = 0;
                (void)read(w->evfd, &val, sizeof(val));
                continue;
            }
            if (tag == (void*)&w->listen_sock) {
                while (__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE)) {
                    int cli = accept4(w->listen_sock, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cli < 0) break;
                    int nd = 1;
                    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
                    tz_web_conn* c = tz_web_conn_acquire(srv, cli);
                    if (!c) { close(cli); continue; }
                    c->ep_events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    tz_linux_mod_ep(w->epfd, cli, (void*)c, c->ep_events, EPOLL_CTL_ADD);
                }
                continue;
            }
            tz_web_conn* c = (tz_web_conn*)tag;
            if ((ev & (EPOLLERR | EPOLLHUP)) && !(ev & EPOLLIN)) {
                epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->sock, NULL);
                tz_web_conn_release(c);
                continue;
            }
            int closed = 0;
            int eof = 0;
            if (ev & EPOLLIN) {
#ifdef TZ_USE_TLS
                if (__builtin_expect(c->ssl != NULL, 0)) {
                    while (1) {
                        if (!tz_web_buf_reserve(&c->tls_raw_in, 8192u)) { closed = 1; break; }
                        size_t space = c->tls_raw_in.cap - c->tls_raw_in.len;
                        ssize_t r = recv(c->sock, c->tls_raw_in.data + c->tls_raw_in.len, space, 0);
                        if (r > 0) {
                            c->tls_raw_in.len += (size_t)r;
                            if ((size_t)r < space) break;
                            continue;
                        }
                        if (r == 0) { eof = 1; break; }
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        closed = 1; break;
                    }
                    if (!closed && c->tls_raw_in.len > 0u) {
                        int tls_eof = 0;
                        if (!tz_tls_feed_raw(c, c->tls_raw_in.data, c->tls_raw_in.len, &tls_eof)) closed = 1;
                        if (tls_eof) eof = 1;
                        c->tls_raw_in.len = 0;
                    }
                } else
#endif
                {
                    while (1) {
                        if (!tz_web_buf_reserve(&c->recv_buf, 8192u)) { closed = 1; break; }
                        size_t space = c->recv_buf.cap - c->recv_buf.len;
                        ssize_t r = recv(c->sock, c->recv_buf.data + c->recv_buf.len, space, 0);
                        if (r > 0) {
                            c->recv_buf.len += (size_t)r;
                            if ((size_t)r < space) break;
                            continue;
                        }
                        if (r == 0) { eof = 1; break; }
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        closed = 1; break;
                    }
                    if (!closed && c->recv_buf.len > 0u) {
                        if (c->state == TZ_CONN_WS) {
                            if (!tz_web_ws_process_frames(c) && c->send_buf.len == 0u) closed = 1;
                        } else {
                            tz_web_dispatch_conn_http(c);
                            if (!eof && !c->close_after_send && c->state == TZ_CONN_HTTP && c->recv_buf.len > 0u) {
                                int drained = 0;
                                while (1) {
                                    if (!tz_web_buf_reserve(&c->recv_buf, 8192u)) { closed = 1; break; }
                                    size_t sp = c->recv_buf.cap - c->recv_buf.len;
                                    ssize_t r2 = recv(c->sock, c->recv_buf.data + c->recv_buf.len, sp, 0);
                                    if (r2 > 0) {
                                        c->recv_buf.len += (size_t)r2;
                                        drained = 1;
                                        if ((size_t)r2 < sp) break;
                                        continue;
                                    }
                                    if (r2 == 0) { eof = 1; break; }
                                    if (errno == EINTR) continue;
                                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                                    closed = 1; break;
                                }
                                if (!closed && drained) tz_web_dispatch_conn_http(c);
                            }
                        }
                    }
                }
                if (!closed && (eof || (ev & EPOLLRDHUP))) {
                    size_t pend =
#ifdef TZ_USE_TLS
                        c->ssl ? (c->tls_wire_out.len - c->tls_wire_off) :
#endif
                        (c->send_buf.len - c->send_off);
                    if (pend > 0u) c->close_after_send = 1;
                    else closed = 1;
                }
            } else if (ev & EPOLLRDHUP) {
                size_t pend =
#ifdef TZ_USE_TLS
                    c->ssl ? (c->tls_wire_out.len - c->tls_wire_off) :
#endif
                    (c->send_buf.len - c->send_off);
                if (pend > 0u) c->close_after_send = 1;
                else closed = 1;
            }
            if (!closed) {
                tz_web_buf* tx_buf = &c->send_buf;
                size_t* tx_off = &c->send_off;
#ifdef TZ_USE_TLS
                if (__builtin_expect(c->ssl != NULL, 0)) {
                    tx_buf = &c->tls_wire_out;
                    tx_off = &c->tls_wire_off;
                }
#endif
                if ((ev & EPOLLOUT) || tx_buf->len > *tx_off) {
                    while (*tx_off < tx_buf->len) {
                        ssize_t s = send(c->sock, tx_buf->data + *tx_off,
                                         tx_buf->len - *tx_off, MSG_NOSIGNAL);
                        if (s > 0) { *tx_off += (size_t)s; continue; }
                        if (s < 0 && errno == EINTR) continue;
                        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                        closed = 1; break;
                    }
                    if (!closed && *tx_off >= tx_buf->len) {
                        tx_buf->len = 0;
                        *tx_off = 0;
                        if (c->close_after_send) closed = 1;
                    }
                }
            }
            if (closed) {
                epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->sock, NULL);
                tz_web_conn_release(c);
            } else {
                size_t pend =
#ifdef TZ_USE_TLS
                    c->ssl ? (c->tls_wire_out.len - c->tls_wire_off) :
#endif
                    (c->send_buf.len - c->send_off);
                uint32_t want = EPOLLIN | EPOLLET | EPOLLRDHUP | (pend > 0u ? EPOLLOUT : 0u);
                if (want != c->ep_events) {
                    c->ep_events = want;
                    tz_linux_mod_ep(w->epfd, c->sock, (void*)c, want, EPOLL_CTL_MOD);
                }
            }
        }
    }
    return NULL;
}

static int tz_posix_start_engine(tz_web_server* srv, const void** err_ptr, int64_t* err_len) {
    srv->ep_workers = (tz_epoll_worker*)calloc((size_t)srv->num_workers, sizeof(tz_epoll_worker));
    __atomic_store_n(&srv->running, 1, __ATOMIC_RELEASE);
    int bport = srv->req_port;
    for (int i = 0; i < srv->num_workers; ++i) {
        tz_epoll_worker* w = &srv->ep_workers[i];
        w->srv = srv;
        w->worker_id = i;
        w->epfd = epoll_create1(EPOLL_CLOEXEC);
        w->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        int actual_port = 0;
        w->listen_sock = tz_linux_make_listen(srv, i == 0 ? bport : srv->bound_port, &actual_port);
        if (w->epfd < 0 || w->evfd < 0 || w->listen_sock < 0) {
            tz_web_set_err(err_ptr, err_len, "Error iniciando epoll/SO_REUSEPORT");
            return 0;
        }
        if (i == 0) srv->bound_port = actual_port ? actual_port : bport;
        tz_linux_mod_ep(w->epfd, w->listen_sock, (void*)&w->listen_sock, EPOLLIN | EPOLLET, EPOLL_CTL_ADD);
        tz_linux_mod_ep(w->epfd, w->evfd, (void*)&w->evfd, EPOLLIN, EPOLL_CTL_ADD);
    }
    for (int i = 0; i < srv->num_workers; ++i) {
        pthread_create(&srv->ep_workers[i].thread, NULL, tz_linux_worker, &srv->ep_workers[i]);
    }
    return 1;
}

static void tz_posix_stop_engine(tz_web_server* srv) {
    __atomic_store_n(&srv->running, 0, __ATOMIC_RELEASE);
    if (srv->ep_workers) {
        for (int i = 0; i < srv->num_workers; ++i) {
            uint64_t one = 1;
            if (srv->ep_workers[i].evfd >= 0) (void)write(srv->ep_workers[i].evfd, &one, sizeof(one));
        }
        for (int i = 0; i < srv->num_workers; ++i) {
            if (srv->ep_workers[i].thread) pthread_join(srv->ep_workers[i].thread, NULL);
            if (srv->ep_workers[i].listen_sock >= 0) close(srv->ep_workers[i].listen_sock);
            if (srv->ep_workers[i].evfd >= 0) close(srv->ep_workers[i].evfd);
            if (srv->ep_workers[i].epfd >= 0) close(srv->ep_workers[i].epfd);
        }
        free(srv->ep_workers);
        srv->ep_workers = NULL;
    }
    for (uint32_t i = 0; i < srv->conn_cap; ++i) {
        if (srv->conns[i].state != TZ_CONN_FREE) tz_web_conn_release(&srv->conns[i]);
    }
}
#else
)c";

inline constexpr const char* kServerPosixPoll = R"c(
static void* tz_poll_bg_loop(void* arg) {
    tz_web_server* srv = (tz_web_server*)arg;
    struct pollfd pfds[512];
    tz_web_conn* map[512];
    while (__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE)) {
        nfds_t n = 0;
        pfds[n].fd = srv->listen_sock;
        pfds[n].events = POLLIN;
        map[n++] = NULL;
        for (uint32_t i = 0; i < srv->conn_cap && n < 512; ++i) {
            if (srv->conns[i].state != TZ_CONN_FREE && srv->conns[i].sock >= 0) {
                size_t pend =
#ifdef TZ_USE_TLS
                    srv->conns[i].ssl ? (srv->conns[i].tls_wire_out.len - srv->conns[i].tls_wire_off) :
#endif
                    (srv->conns[i].send_buf.len - srv->conns[i].send_off);
                pfds[n].fd = srv->conns[i].sock;
                pfds[n].events = POLLIN | (pend > 0u ? POLLOUT : 0);
                map[n++] = &srv->conns[i];
            }
        }
        int pr = poll(pfds, n, 100);
        if (pr <= 0) continue;
        if (pfds[0].revents & POLLIN) {
            int cli = accept(srv->listen_sock, NULL, NULL);
            if (cli >= 0) {
                fcntl(cli, F_SETFL, fcntl(cli, F_GETFL, 0) | O_NONBLOCK);
                int nd = 1;
                setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
                if (!tz_web_conn_acquire(srv, cli)) close(cli);
            }
        }
        for (nfds_t k = 1; k < n; ++k) {
            tz_web_conn* c = map[k];
            int dead = 0;
            if (pfds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) dead = 1;
            if (!dead && (pfds[k].revents & POLLIN)) {
#ifdef TZ_USE_TLS
                if (__builtin_expect(c->ssl != NULL, 0)) {
                    if (!tz_web_buf_reserve(&c->tls_raw_in, 8192u)) dead = 1;
                    else {
                        ssize_t r = recv(c->sock, c->tls_raw_in.data + c->tls_raw_in.len,
                                         c->tls_raw_in.cap - c->tls_raw_in.len, 0);
                        if (r > 0) {
                            int tls_eof = 0;
                            c->tls_raw_in.len += (size_t)r;
                            if (!tz_tls_feed_raw(c, c->tls_raw_in.data, c->tls_raw_in.len, &tls_eof)) dead = 1;
                            c->tls_raw_in.len = 0;
                            if (!dead && tls_eof) {
                                if (c->tls_wire_out.len > c->tls_wire_off) c->close_after_send = 1;
                                else dead = 1;
                            }
                        } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) dead = 1;
                    }
                } else
#endif
                {
                    if (!tz_web_buf_reserve(&c->recv_buf, 8192u)) dead = 1;
                    else {
                        ssize_t r = recv(c->sock, c->recv_buf.data + c->recv_buf.len, c->recv_buf.cap - c->recv_buf.len, 0);
                        if (r > 0) {
                            c->recv_buf.len += (size_t)r;
                            if (c->state == TZ_CONN_WS) { if (!tz_web_ws_process_frames(c) && !c->send_buf.len) dead = 1; }
                            else tz_web_dispatch_conn_http(c);
                        } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) dead = 1;
                    }
                }
            }
            if (!dead) {
                tz_web_buf* tx_buf = &c->send_buf;
                size_t* tx_off = &c->send_off;
#ifdef TZ_USE_TLS
                if (__builtin_expect(c->ssl != NULL, 0)) {
                    tx_buf = &c->tls_wire_out;
                    tx_off = &c->tls_wire_off;
                }
#endif
                if (tx_buf->len > *tx_off) {
                    ssize_t s = send(c->sock, tx_buf->data + *tx_off, tx_buf->len - *tx_off, 0);
                    if (s > 0) {
                        *tx_off += (size_t)s;
                        if (*tx_off >= tx_buf->len) {
                            tx_buf->len = 0; *tx_off = 0;
                            if (c->close_after_send) dead = 1;
                        }
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) dead = 1;
                }
            }
            if (dead) tz_web_conn_release(c);
        }
    }
    return NULL;
}
static int tz_posix_start_engine(tz_web_server* srv, const void** err_ptr, int64_t* err_len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    char pstr[16]; snprintf(pstr, sizeof(pstr), "%d", srv->req_port);
    if (getaddrinfo(srv->host && srv->host[0] ? srv->host : "0.0.0.0", pstr, &hints, &res) != 0 || !res) {
        tz_web_set_err(err_ptr, err_len, "Error resolviendo host/puerto"); return 0;
    }
    int ls = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (ls < 0) { freeaddrinfo(res); tz_web_set_err(err_ptr, err_len, "Error creando socket"); return 0; }
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fcntl(ls, F_SETFL, fcntl(ls, F_GETFL, 0) | O_NONBLOCK);
    if (bind(ls, res->ai_addr, res->ai_addrlen) != 0 || listen(ls, 512) != 0) {
        freeaddrinfo(res); close(ls); tz_web_set_err(err_ptr, err_len, "Error en bind/listen"); return 0;
    }
    freeaddrinfo(res);
    struct sockaddr_in sin; socklen_t sl = sizeof(sin);
    if (getsockname(ls, (struct sockaddr*)&sin, &sl) == 0) srv->bound_port = (int)ntohs(sin.sin_port);
    srv->listen_sock = ls;
    __atomic_store_n(&srv->running, 1, __ATOMIC_RELEASE);
    pthread_create(&srv->bg_thread, NULL, tz_poll_bg_loop, srv);
    srv->has_bg_thread = 1;
    return 1;
}
static void tz_posix_stop_engine(tz_web_server* srv) {
    __atomic_store_n(&srv->running, 0, __ATOMIC_RELEASE);
    if (srv->has_bg_thread) { pthread_join(srv->bg_thread, NULL); srv->has_bg_thread = 0; }
    if (srv->listen_sock >= 0) { close(srv->listen_sock); srv->listen_sock = -1; }
    for (uint32_t i = 0; i < srv->conn_cap; ++i) {
        if (srv->conns[i].state != TZ_CONN_FREE) tz_web_conn_release(&srv->conns[i]);
    }
}
#endif
#endif
)c";

inline constexpr const char* kServerApi = R"c(
void* tz_web_srv_new(const void* host_tstr, int32_t port, int32_t workers) {
    tz_web_net_init();
    tz_web_server* srv = (tz_web_server*)calloc(1, sizeof(tz_web_server));
    if (!srv) return NULL;
    size_t hlen = 0;
    const char* h = tz_web_str_view(host_tstr, &hlen);
    srv->host = tz_web_dup_cstr(hlen ? h : "0.0.0.0", hlen ? hlen : 7u);
    srv->req_port = port;
    srv->bound_port = port;
    srv->num_workers = workers > 0 ? workers : tz_web_cpu_count();
    if (srv->num_workers > 16) srv->num_workers = 16;
    tz_web_lock_init(&srv->static_cache_lock);
    tz_web_lock_init(&srv->ws_lock);
    tz_web_lock_init(&srv->conn_lock);
    srv->conn_cap = TZ_WEB_MAX_CONNS;
    srv->conns = (tz_web_conn*)calloc(srv->conn_cap, sizeof(tz_web_conn));
    srv->free_stack = (uint32_t*)malloc(srv->conn_cap * sizeof(uint32_t));
    for (uint32_t i = 0; i < srv->conn_cap; ++i) {
        tz_web_conn* c = &srv->conns[i];
        c->srv = srv;
        c->sock = TZ_WEB_BAD_SOCK;
        c->slot_idx = i;
        c->state = TZ_CONN_FREE;
#ifdef TZ_USE_TLS
        tz_web_lock_init(&c->tls_lock);
#endif
#ifdef _WIN32
        c->io_tag.slot_idx = i;
#endif
        srv->free_stack[i] = srv->conn_cap - 1u - i;
    }
    srv->free_top = srv->conn_cap;
#ifdef _WIN32
    srv->listen_sock = INVALID_SOCKET;
#elif !defined(__linux__)
    srv->listen_sock = -1;
#endif
    return (void*)srv;
}

int32_t tz_web_srv_port(void* sp) {
    tz_web_server* srv = (tz_web_server*)sp;
    return srv ? (int32_t)srv->bound_port : 0;
}

static void tz_web_add_route_impl(tz_web_server* srv, int32_t method, const char* pat, size_t plen,
                                  tz_web_handler_fn fn, int is_static_resp,
                                  int status, const char* ct, size_t ct_len, const char* body, size_t body_len) {
    if (srv->route_count >= srv->route_cap) {
        size_t nc = srv->route_cap ? srv->route_cap * 2u : 16u;
        srv->routes = (tz_web_route_entry*)realloc(srv->routes, nc * sizeof(tz_web_route_entry));
        srv->route_cap = nc;
    }
    tz_web_route_entry* r = &srv->routes[srv->route_count++];
    memset(r, 0, sizeof(*r));
    r->method = method;
    r->pattern = tz_web_dup_cstr(pat, plen);
    r->pattern_len = plen;
    r->hash = tz_web_hash(pat, plen);
    r->fn = fn;
    r->is_static_resp = is_static_resp;
    r->is_param = (memchr(pat, ':', plen) != NULL || memchr(pat, '*', plen) != NULL);
    if (r->is_param) {
        size_t pos = (plen > 0u && pat[0] == '/') ? 1u : 0u;
        while (pos < plen && r->seg_count < TZ_WEB_MAX_SEGS) {
            const char* slash = (const char*)memchr(pat + pos, '/', plen - pos);
            size_t slen = slash ? (size_t)(slash - (pat + pos)) : (plen - pos);
            if (slen > 0u && (r->pattern[pos] == ':' || r->pattern[pos] == '*')) {
                r->seg_is_param[r->seg_count] = (r->pattern[pos] == ':');
                r->seg_is_wild[r->seg_count] = (r->pattern[pos] == '*');
                r->segs[r->seg_count].ptr = r->pattern + pos + 1u;
                r->segs[r->seg_count].len = slen - 1u;
            } else {
                r->segs[r->seg_count].ptr = r->pattern + pos;
                r->segs[r->seg_count].len = slen;
            }
            r->seg_count++;
            pos += slen + (slash ? 1u : 0u);
        }
        if (plen > 0u && pat[0] == '/' && pat[plen - 1u] != '/' &&
            r->seg_count >= 1 && r->seg_is_param[r->seg_count - 1]) {
            int ok_fast = 1;
            for (int si = 0; si + 1 < r->seg_count; ++si) {
                if (r->seg_is_param[si] || r->seg_is_wild[si] || r->segs[si].len == 0u) {
                    ok_fast = 0;
                    break;
                }
            }
            if (ok_fast) {
                int last = r->seg_count - 1;
                r->fast_prefix = r->pattern;
                r->fast_prefix_len = (size_t)((r->segs[last].ptr - 1u) - r->pattern);
                r->fast_param_name = r->segs[last];
            }
        }
    }
    if (is_static_resp) {
        const char* reason = tz_web_reason(status);
        char clen_buf[32];
        size_t clen_len = tz_web_fmt_u64(clen_buf, (uint64_t)body_len);
        size_t plen_est = 9u + strlen(reason) + 2u + (ct_len ? 14u + ct_len + 2u : 0u) + 16u + clen_len + 2u + 6u;
        r->static_prefix = (char*)malloc(plen_est + 1u);
        size_t wp = 0;
        memcpy(r->static_prefix + wp, "HTTP/1.1 ", 9u); wp += 9u;
        memcpy(r->static_prefix + wp, reason, strlen(reason)); wp += strlen(reason);
        r->static_prefix[wp++] = '\r'; r->static_prefix[wp++] = '\n';
        if (ct_len > 0u && ct) {
            memcpy(r->static_prefix + wp, "Content-Type: ", 14u); wp += 14u;
            memcpy(r->static_prefix + wp, ct, ct_len); wp += ct_len;
            r->static_prefix[wp++] = '\r'; r->static_prefix[wp++] = '\n';
        }
        memcpy(r->static_prefix + wp, "Content-Length: ", 16u); wp += 16u;
        memcpy(r->static_prefix + wp, clen_buf, clen_len); wp += clen_len;
        r->static_prefix[wp++] = '\r'; r->static_prefix[wp++] = '\n';
        memcpy(r->static_prefix + wp, "Date: ", 6u); wp += 6u;
        r->static_prefix[wp] = '\0';
        r->static_prefix_len = wp;

        r->static_suffix = (char*)malloc(4u + body_len + 1u);
        memcpy(r->static_suffix, "\r\n\r\n", 4u);
        if (body_len > 0u && body) memcpy(r->static_suffix + 4u, body, body_len);
        r->static_suffix[4u + body_len] = '\0';
        r->static_suffix_len = 4u + body_len;
    }
}

void tz_web_srv_route(void* sp, int32_t method, const void* pat_tstr, void* fn_ptr) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) return;
    size_t plen = 0;
    const char* pat = tz_web_str_view(pat_tstr, &plen);
    tz_web_add_route_impl(srv, method, pat, plen, (tz_web_handler_fn)fn_ptr, 0, 200, NULL, 0u, NULL, 0u);
}

void tz_web_srv_route_static(void* sp, int32_t method, const void* pat_tstr, int32_t status,
                             const void* ct_tstr, const void* body_tstr) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) return;
    size_t plen = 0, ctlen = 0, blen = 0;
    const char* pat = tz_web_str_view(pat_tstr, &plen);
    const char* ct = tz_web_str_view(ct_tstr, &ctlen);
    const char* body = tz_web_str_view(body_tstr, &blen);
    tz_web_add_route_impl(srv, method, pat, plen, NULL, 1, status, ct, ctlen, body, blen);
}

void tz_web_srv_mount_static(void* sp, const void* pfx_tstr, const void* dir_tstr) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) return;
    size_t plen = 0, dlen = 0;
    const char* pfx = tz_web_str_view(pfx_tstr, &plen);
    const char* dir = tz_web_str_view(dir_tstr, &dlen);
    if (srv->mount_count >= srv->mount_cap) {
        size_t nc = srv->mount_cap ? srv->mount_cap * 2u : 4u;
        srv->mounts = (tz_web_mount*)realloc(srv->mounts, nc * sizeof(tz_web_mount));
        srv->mount_cap = nc;
    }
    tz_web_mount* m = &srv->mounts[srv->mount_count++];
    m->prefix = tz_web_dup_cstr(pfx, plen);
    m->prefix_len = plen;
    m->root_dir = tz_web_dup_cstr(dir, dlen);
    m->root_dir_len = dlen;
}

void tz_web_srv_middleware(void* sp, void* before_fn, void* after_fn) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) return;
    srv->mw_before = (tz_web_mw_fn)before_fn;
    srv->mw_after = (tz_web_mw_fn)after_fn;
}

int32_t tz_web_srv_start_bg(void* sp, const void** err_ptr, int64_t* err_len) {
    tz_web_set_err(err_ptr, err_len, NULL);
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) { tz_web_set_err(err_ptr, err_len, "Servidor nulo"); return 0; }
    if (srv->started) return 1;
    tz_web_build_route_tables(srv);
    srv->started = 1;
#ifdef _WIN32
    return tz_win_start_engine(srv, err_ptr, err_len);
#else
    return tz_posix_start_engine(srv, err_ptr, err_len);
#endif
}

int32_t tz_web_srv_run(void* sp, const void** err_ptr, int64_t* err_len) {
    if (!tz_web_srv_start_bg(sp, err_ptr, err_len)) return 0;
    tz_web_server* srv = (tz_web_server*)sp;
    while (__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE)) {
#ifdef _WIN32
        Sleep(100);
#else
        struct timespec ts = { 0, 100000000L };
        nanosleep(&ts, NULL);
#endif
    }
    return 1;
}

void tz_web_srv_stop(void* sp) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv || !srv->started) return;
#ifdef _WIN32
    tz_win_stop_engine(srv);
#else
    tz_posix_stop_engine(srv);
#endif
    srv->started = 0;
}

void tz_web_srv_free(void* sp) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) return;
    tz_web_srv_stop(sp);
    for (uint32_t i = 0; i < srv->conn_cap; ++i) {
        tz_web_buf_free(&srv->conns[i].recv_buf);
        tz_web_buf_free(&srv->conns[i].send_buf);
        tz_web_buf_free(&srv->conns[i].extra_hdrs);
        tz_web_buf_free(&srv->conns[i].body_buf);
#ifdef TZ_USE_TLS
        tz_web_buf_free(&srv->conns[i].tls_wire_out);
        tz_web_buf_free(&srv->conns[i].tls_raw_in);
        tz_web_lock_destroy(&srv->conns[i].tls_lock);
#endif
    }
#ifdef TZ_USE_TLS
    if (srv->tls_ctx) {
        tz_tls_api.ctx_free(srv->tls_ctx);
        srv->tls_ctx = NULL;
    }
#endif
    free(srv->conns);
    free(srv->free_stack);
    for (size_t i = 0; i < srv->route_count; ++i) {
        free(srv->routes[i].pattern);
        free(srv->routes[i].static_prefix);
        free(srv->routes[i].static_suffix);
    }
    free(srv->routes);
    free(srv->hash_slots);
    free(srv->param_routes);
    for (size_t i = 0; i < srv->mount_count; ++i) {
        free(srv->mounts[i].prefix);
        free(srv->mounts[i].root_dir);
    }
    free(srv->mounts);
    for (size_t i = 0; i < srv->static_cache_count; ++i) {
        free(srv->static_cache[i].full_path);
        free(srv->static_cache[i].data);
    }
    for (size_t i = 0; i < srv->ws_route_count; ++i) {
        free(srv->ws_routes[i].pattern);
    }
    free(srv->ws_routes);
    free(srv->host);
    tz_web_lock_destroy(&srv->static_cache_lock);
    tz_web_lock_destroy(&srv->ws_lock);
    tz_web_lock_destroy(&srv->conn_lock);
    free(srv);
}
)c";

inline constexpr const char* kServerCtxApi = R"c(
void tz_web_srv_stats(void* sp, int64_t* out_reqs, int64_t* out_active) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) {
        if (out_reqs) *out_reqs = 0;
        if (out_active) *out_active = 0;
        return;
    }
    if (out_reqs) *out_reqs = (int64_t)__atomic_load_n(&srv->reqs_total, __ATOMIC_RELAXED);
    if (out_active) *out_active = (int64_t)__atomic_load_n(&srv->active_conns, __ATOMIC_RELAXED);
}

void tz_web_srv_set_data(void* sp, void* data) {
    tz_web_server* srv = (tz_web_server*)sp;
    if (srv) srv->user_data = data;
}

void* tz_web_srv_ctx_data(void* cp) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    return (ctx && ctx->conn && ctx->conn->srv) ? ctx->conn->srv->user_data : NULL;
}

int32_t tz_web_srv_req_method(void* cp) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    return ctx ? ctx->method : 0;
}

const uint8_t* tz_web_srv_req_path(void* cp, int64_t* out_len) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx) { if (out_len) *out_len = 0; return (const uint8_t*)""; }
    if (out_len) *out_len = (int64_t)ctx->path.len;
    return (const uint8_t*)ctx->path.ptr;
}

const uint8_t* tz_web_srv_req_query(void* cp, int64_t* out_len) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx) { if (out_len) *out_len = 0; return (const uint8_t*)""; }
    if (out_len) *out_len = (int64_t)ctx->query.len;
    return (const uint8_t*)ctx->query.ptr;
}

const uint8_t* tz_web_srv_req_body(void* cp, int64_t* out_len) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx) { if (out_len) *out_len = 0; return (const uint8_t*)""; }
    if (out_len) *out_len = (int64_t)ctx->body.len;
    return (const uint8_t*)ctx->body.ptr;
}

const uint8_t* tz_web_srv_req_header(void* cp, const void* name_tstr, int64_t* out_len) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (out_len) *out_len = 0;
    if (!ctx) return (const uint8_t*)"";
    size_t nlen = 0;
    const char* name = tz_web_str_view(name_tstr, &nlen);
    tz_web_slice val;
    if (!tz_web_ctx_find_hdr(ctx, name, nlen, &val)) return (const uint8_t*)"";
    if (out_len) *out_len = (int64_t)val.len;
    return (const uint8_t*)val.ptr;
}

const uint8_t* tz_web_srv_req_param(void* cp, const void* name_tstr, int64_t* out_len) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (out_len) *out_len = 0;
    if (!ctx) return (const uint8_t*)"";
    size_t nlen = 0;
    const char* name = tz_web_str_view(name_tstr, &nlen);
    for (int i = 0; i < ctx->param_count; ++i) {
        if (ctx->param_names[i].len == nlen &&
            (nlen == 0u || memcmp(ctx->param_names[i].ptr, name, nlen) == 0)) {
            if (out_len) *out_len = (int64_t)ctx->param_vals[i].len;
            return (const uint8_t*)ctx->param_vals[i].ptr;
        }
    }
    return (const uint8_t*)"";
}

int32_t tz_web_srv_req_param_i64(void* cp, const void* name_tstr, int64_t* out_val) {
    int64_t n = 0;
    const uint8_t* p = tz_web_srv_req_param(cp, name_tstr, &n);
    if (n <= 0) return 0;
    return tz_web_parse_i64((const char*)p, (size_t)n, out_val) ? 1 : 0;
}

static const char* tz_web_json_find_field(const char* j, size_t jlen, const char* key, size_t klen, size_t* vlen_out) {
    size_t i = 0;
    while (i < jlen) {
        const char* q = (const char*)memchr(j + i, '"', jlen - i);
        if (!q) break;
        i = (size_t)(q - j) + 1u;
        size_t s = i;
        while (i < jlen && j[i] != '"') {
            if (j[i] == '\\' && i + 1u < jlen) i += 2u;
            else i++;
        }
        size_t flen = i - s;
        if (i < jlen) i++;
        while (i < jlen && (j[i] == ' ' || j[i] == '\t' || j[i] == '\r' || j[i] == '\n')) i++;
        if (i < jlen && j[i] == ':') {
            i++;
            while (i < jlen && (j[i] == ' ' || j[i] == '\t' || j[i] == '\r' || j[i] == '\n')) i++;
            if (flen == klen && memcmp(j + s, key, klen) == 0 && i < jlen) {
                if (j[i] == '"') {
                    i++;
                    size_t vs = i;
                    while (i < jlen && j[i] != '"') {
                        if (j[i] == '\\' && i + 1u < jlen) i += 2u;
                        else i++;
                    }
                    *vlen_out = i - vs;
                    return j + vs;
                } else {
                    size_t vs = i;
                    while (i < jlen && j[i] != ',' && j[i] != '}' && j[i] != ']' &&
                           j[i] != ' ' && j[i] != '\t' && j[i] != '\r' && j[i] != '\n') i++;
                    *vlen_out = i - vs;
                    return j + vs;
                }
            }
        }
    }
    return NULL;
}

int32_t tz_web_srv_req_json_i64(void* cp, const void* key_tstr, int64_t* out_val) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx || ctx->body.len == 0u) return 0;
    size_t klen = 0;
    const char* key = tz_web_str_view(key_tstr, &klen);
    size_t vlen = 0;
    const char* val = tz_web_json_find_field(ctx->body.ptr, ctx->body.len, key, klen, &vlen);
    if (!val || vlen == 0u) return 0;
    return tz_web_parse_i64(val, vlen, out_val) ? 1 : 0;
}

const uint8_t* tz_web_srv_req_json_str(void* cp, const void* key_tstr, int64_t* out_len) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (out_len) *out_len = 0;
    if (!ctx || ctx->body.len == 0u) return (const uint8_t*)"";
    size_t klen = 0;
    const char* key = tz_web_str_view(key_tstr, &klen);
    size_t vlen = 0;
    const char* val = tz_web_json_find_field(ctx->body.ptr, ctx->body.len, key, klen, &vlen);
    if (!val) return (const uint8_t*)"";
    if (out_len) *out_len = (int64_t)vlen;
    return (const uint8_t*)val;
}

void tz_web_srv_resp_status(void* cp, int32_t code) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (ctx) ctx->resp_status = code;
}

void tz_web_srv_resp_header(void* cp, const void* k_tstr, const void* v_tstr) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx) return;
    size_t kl = 0, vl = 0;
    const char* k = tz_web_str_view(k_tstr, &kl);
    const char* v = tz_web_str_view(v_tstr, &vl);
    if (kl == 0u) return;
    tz_web_buf_append(&ctx->conn->extra_hdrs, k, kl);
    tz_web_buf_append(&ctx->conn->extra_hdrs, ": ", 2u);
    tz_web_buf_append(&ctx->conn->extra_hdrs, v, vl);
    tz_web_buf_append(&ctx->conn->extra_hdrs, "\r\n", 2u);
}

void tz_web_srv_resp_send(void* cp, int32_t status, const void* ct_tstr, const void* body_tstr) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx || ctx->resp_sent) return;
    size_t ctl = 0, bl = 0;
    const char* ct = tz_web_str_view(ct_tstr, &ctl);
    const char* body = tz_web_str_view(body_tstr, &bl);
    tz_web_write_http_resp(ctx->conn, status, ct, ctl, body, bl,
                           ctx->conn->extra_hdrs.data, ctx->conn->extra_hdrs.len,
                           ctx->conn->close_after_send, ctx->method == TZ_M_HEAD);
    ctx->resp_sent = 1;
}

void tz_web_srv_resp_send_raw(void* cp, int32_t status, const void* ct_tstr, const uint8_t* data, int64_t len) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx || ctx->resp_sent) return;
    size_t ctl = 0;
    const char* ct = tz_web_str_view(ct_tstr, &ctl);
    size_t bl = len > 0 ? (size_t)len : 0u;
    tz_web_write_http_resp(ctx->conn, status, ct, ctl, (const char*)data, bl,
                           ctx->conn->extra_hdrs.data, ctx->conn->extra_hdrs.len,
                           ctx->conn->close_after_send, ctx->method == TZ_M_HEAD);
    ctx->resp_sent = 1;
}

void tz_web_srv_json_begin(void* cp) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx) return;
    tz_web_buf* b = &ctx->conn->body_buf;
    if (!tz_web_buf_reserve(b, 256u)) return;
    b->data[0] = '{';
    b->len = 1u;
    ctx->json_active = 1;
    ctx->json_fields = 0;
}

void tz_web_srv_json_str(void* cp, const void* k_tstr, const void* v_tstr) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx || !ctx->json_active) return;
    tz_web_buf* b = &ctx->conn->body_buf;
    size_t kl = 0, vl = 0;
    const char* k = tz_web_str_view(k_tstr, &kl);
    const char* v = tz_web_str_view(v_tstr, &vl);
    if (ctx->json_fields > 0) {
        if (!tz_web_buf_reserve(b, 1u)) return;
        b->data[b->len++] = ',';
    }
    tz_web_buf_append_json_str(b, k, kl);
    if (!tz_web_buf_reserve(b, 1u)) return;
    b->data[b->len++] = ':';
    tz_web_buf_append_json_str(b, v, vl);
    ctx->json_fields++;
}

void tz_web_srv_json_i64(void* cp, const void* k_tstr, int64_t val) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx || !ctx->json_active) return;
    tz_web_buf* b = &ctx->conn->body_buf;
    size_t kl = 0;
    const char* k = tz_web_str_view(k_tstr, &kl);
    if (ctx->json_fields > 0) {
        if (!tz_web_buf_reserve(b, 1u)) return;
        b->data[b->len++] = ',';
    }
    size_t clean = (kl > 0u && k) ? tz_web_first_json_esc(k, kl) : kl;
    if (clean == kl && tz_web_buf_reserve(b, kl + 36u)) {
        char* dst = b->data + b->len;
        size_t p = 0;
        dst[p++] = '"';
        if (kl > 0u && k) { memcpy(dst + p, k, kl); p += kl; }
        dst[p++] = '"';
        dst[p++] = ':';
        p += tz_web_fmt_i64(dst + p, val);
        b->len += p;
    } else {
        tz_web_buf_append_json_str(b, k, kl);
        if (tz_web_buf_reserve(b, 32u)) {
            b->data[b->len++] = ':';
            b->len += tz_web_fmt_i64(b->data + b->len, val);
        }
    }
    ctx->json_fields++;
}

void tz_web_srv_json_bool(void* cp, const void* k_tstr, int32_t val) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx || !ctx->json_active) return;
    tz_web_buf* b = &ctx->conn->body_buf;
    size_t kl = 0;
    const char* k = tz_web_str_view(k_tstr, &kl);
    if (ctx->json_fields > 0) {
        if (!tz_web_buf_reserve(b, 1u)) return;
        b->data[b->len++] = ',';
    }
    size_t clean = (kl > 0u && k) ? tz_web_first_json_esc(k, kl) : kl;
    if (clean == kl && tz_web_buf_reserve(b, kl + 16u)) {
        char* dst = b->data + b->len;
        size_t p = 0;
        dst[p++] = '"';
        if (kl > 0u && k) { memcpy(dst + p, k, kl); p += kl; }
        dst[p++] = '"';
        if (val) { memcpy(dst + p, ":true", 5u); p += 5u; }
        else { memcpy(dst + p, ":false", 6u); p += 6u; }
        b->len += p;
    } else {
        tz_web_buf_append_json_str(b, k, kl);
        tz_web_buf_append(b, val ? ":true" : ":false", val ? 5u : 6u);
    }
    ctx->json_fields++;
}

void tz_web_srv_json_end(void* cp, int32_t status) {
    tz_web_ctx* ctx = (tz_web_ctx*)cp;
    if (!ctx || !ctx->json_active || ctx->resp_sent) return;
    tz_web_conn* c = ctx->conn;
    tz_web_buf* b = &c->body_buf;
    if (!tz_web_buf_reserve(b, 1u)) return;
    b->data[b->len++] = '}';
    if (status == 200 && c->extra_hdrs.len == 0u && !c->close_after_send && ctx->method != TZ_M_HEAD) {
        size_t blen = b->len;
        if (tz_web_buf_reserve(&c->send_buf, TZ_JSON_200_HDR_LEN + 24u + blen)) {
            char* dst = c->send_buf.data + c->send_buf.len;
            uint32_t slot = c->cur_date_slot & 1u;
            memcpy(dst, tz_web_date_cache.json_200_hdr[slot], TZ_JSON_200_HDR_LEN);
            size_t pos = TZ_JSON_200_HDR_LEN;
            pos += tz_web_fmt_u64(dst + pos, (uint64_t)blen);
            dst[pos++] = '\r'; dst[pos++] = '\n';
            dst[pos++] = '\r'; dst[pos++] = '\n';
            if (blen > 0u) memcpy(dst + pos, b->data, blen);
            pos += blen;
            c->send_buf.len += pos;
        }
    } else {
        tz_web_write_http_resp(c, status, "application/json; charset=utf-8", 31u,
                               b->data, b->len, c->extra_hdrs.data, c->extra_hdrs.len,
                               c->close_after_send, ctx->method == TZ_M_HEAD);
    }
    ctx->json_active = 0;
    ctx->resp_sent = 1;
}
)c";

inline constexpr const char* kClientRuntime = R"c(
#define TZ_CLI_POOL_MAX 16

typedef struct tz_cli_pool_entry {
    char host[128];
    int port;
    tz_web_sock_t sock;
    int64_t last_used;
} tz_cli_pool_entry;

static tz_cli_pool_entry tz_cli_pool[TZ_CLI_POOL_MAX];
static tz_web_lock_t tz_cli_pool_lock;
static int32_t tz_cli_pool_inited = 0;

static void tz_cli_cleanup(void) {
    if (__atomic_load_n(&tz_cli_pool_inited, __ATOMIC_ACQUIRE) != 2) return;
    tz_web_lock_acq(&tz_cli_pool_lock);
    for (int i = 0; i < TZ_CLI_POOL_MAX; ++i) {
        if (tz_cli_pool[i].sock != TZ_WEB_BAD_SOCK) {
            tz_web_sock_close(tz_cli_pool[i].sock);
            tz_cli_pool[i].sock = TZ_WEB_BAD_SOCK;
        }
    }
    tz_web_lock_rel(&tz_cli_pool_lock);
}

static void tz_cli_pool_init(void) {
    if (__atomic_load_n(&tz_cli_pool_inited, __ATOMIC_ACQUIRE) == 2) return;
    int32_t exp = 0;
    if (__atomic_compare_exchange_n(&tz_cli_pool_inited, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        tz_web_lock_init(&tz_cli_pool_lock);
        for (int i = 0; i < TZ_CLI_POOL_MAX; ++i) tz_cli_pool[i].sock = TZ_WEB_BAD_SOCK;
        atexit(tz_cli_cleanup);
        __atomic_store_n(&tz_cli_pool_inited, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&tz_cli_pool_inited, __ATOMIC_ACQUIRE) != 2) {
#ifdef _WIN32
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
}

static tz_web_sock_t tz_cli_borrow(const char* host, int port) {
    tz_cli_pool_init();
    tz_web_sock_t found = TZ_WEB_BAD_SOCK;
    int64_t now = (int64_t)time(NULL);
    tz_web_lock_acq(&tz_cli_pool_lock);
    for (int i = 0; i < TZ_CLI_POOL_MAX; ++i) {
        if (tz_cli_pool[i].sock != TZ_WEB_BAD_SOCK && tz_cli_pool[i].port == port &&
            strcmp(tz_cli_pool[i].host, host) == 0) {
            if (now - tz_cli_pool[i].last_used <= 30) {
                found = tz_cli_pool[i].sock;
                tz_cli_pool[i].sock = TZ_WEB_BAD_SOCK;
                break;
            } else {
                tz_web_sock_close(tz_cli_pool[i].sock);
                tz_cli_pool[i].sock = TZ_WEB_BAD_SOCK;
            }
        }
    }
    tz_web_lock_rel(&tz_cli_pool_lock);
    return found;
}

static void tz_cli_return(const char* host, int port, tz_web_sock_t s) {
    if (s == TZ_WEB_BAD_SOCK) return;
    tz_cli_pool_init();
    tz_web_lock_acq(&tz_cli_pool_lock);
    int placed = 0;
    for (int i = 0; i < TZ_CLI_POOL_MAX; ++i) {
        if (tz_cli_pool[i].sock == TZ_WEB_BAD_SOCK) {
            snprintf(tz_cli_pool[i].host, sizeof(tz_cli_pool[i].host), "%s", host);
            tz_cli_pool[i].port = port;
            tz_cli_pool[i].sock = s;
            tz_cli_pool[i].last_used = (int64_t)time(NULL);
            placed = 1;
            break;
        }
    }
    tz_web_lock_rel(&tz_cli_pool_lock);
    if (!placed) tz_web_sock_close(s);
}

static tz_web_sock_t tz_cli_connect_host(const char* host, int port, int timeout_ms) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char pstr[16];
    snprintf(pstr, sizeof(pstr), "%d", port);
    if (getaddrinfo(host, pstr, &hints, &res) != 0 || !res) return TZ_WEB_BAD_SOCK;
    tz_web_sock_t sock = TZ_WEB_BAD_SOCK;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        tz_web_sock_t s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == TZ_WEB_BAD_SOCK) continue;
        int nd = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
#ifdef _WIN32
        DWORD tv = (DWORD)(timeout_ms > 0 ? timeout_ms : 5000);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
        int ms = timeout_ms > 0 ? timeout_ms : 5000;
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) { sock = s; break; }
        tz_web_sock_close(s);
    }
    freeaddrinfo(res);
    return sock;
}

static int tz_cli_send_all(tz_web_sock_t s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(s, data + sent, (int)(len - sent), 0);
        if (n <= 0) return 0;
#else
        ssize_t n = send(s, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
#endif
        sent += (size_t)n;
    }
    return 1;
}

void tz_web_cli_free_buf(void* p) {
    free(p);
}
)c";

inline constexpr const char* kClientExec = R"c(
static int tz_cli_io_send_all(tz_web_sock_t s, void* ssl, const char* data, size_t len) {
#ifdef TZ_USE_CLIENT_TLS
    if (ssl) {
        size_t sent = 0;
        while (sent < len) {
            size_t rem = len - sent;
            int chunk = rem > 65536u ? 65536 : (int)rem;
            int w = tz_tls_api.ssl_write(ssl, data + sent, chunk);
            if (w <= 0) return 0;
            sent += (size_t)w;
        }
        return 1;
    }
#else
    (void)ssl;
#endif
    return tz_cli_send_all(s, data, len);
}

static int tz_cli_io_recv(tz_web_sock_t s, void* ssl, char* buf, size_t cap) {
#ifdef TZ_USE_CLIENT_TLS
    if (ssl) {
        int chunk = cap > 65536u ? 65536 : (int)cap;
        return tz_tls_api.ssl_read(ssl, buf, chunk);
    }
#else
    (void)ssl;
#endif
#ifdef _WIN32
    return recv(s, buf, (int)(cap > 65536u ? 65536u : cap), 0);
#else
    while (1) {
        ssize_t sr = recv(s, buf, cap > 65536u ? 65536u : cap, 0);
        if (sr < 0 && errno == EINTR) continue;
        return (int)sr;
    }
#endif
}

int32_t tz_web_cli_request(const void* method_tstr, const void* url_tstr, const void* hdrs_tstr,
                           const void* body_tstr, int32_t timeout_ms,
                           int32_t* out_status, uint8_t** out_body_ptr, int64_t* out_body_len,
                           uint8_t** out_hdr_ptr, int64_t* out_hdr_len,
                           const void** out_err_ptr, int64_t* out_err_len) {
    tz_web_net_init();
    tz_web_set_err(out_err_ptr, out_err_len, NULL);
    if (out_status) *out_status = 0;
    if (out_body_ptr) *out_body_ptr = NULL;
    if (out_body_len) *out_body_len = 0;
    if (out_hdr_ptr) *out_hdr_ptr = NULL;
    if (out_hdr_len) *out_hdr_len = 0;

    size_t mlen = 0, ulen = 0, hlen = 0, blen = 0;
    const char* method = tz_web_str_view(method_tstr, &mlen);
    const char* url = tz_web_str_view(url_tstr, &ulen);
    const char* extra_hdrs = tz_web_str_view(hdrs_tstr, &hlen);
    const char* body = tz_web_str_view(body_tstr, &blen);

    int is_https = 0;
    size_t scheme_len = 0u;
    if (ulen >= 8u && memcmp(url, "https://", 8u) == 0) {
        is_https = 1;
        scheme_len = 8u;
    } else if (ulen >= 7u && memcmp(url, "http://", 7u) == 0) {
        is_https = 0;
        scheme_len = 7u;
    } else {
        tz_web_set_err(out_err_ptr, out_err_len, "Solo se admite esquema http:// o https://");
        return 0;
    }
    const char* hp = url + scheme_len;
    const char* slash = (const char*)memchr(hp, '/', ulen - scheme_len);
    size_t hostport_len = slash ? (size_t)(slash - hp) : (ulen - scheme_len);
    const char* path = slash ? slash : "/";
    size_t path_len = slash ? (ulen - (size_t)(slash - url)) : 1u;

    const char* colon = (const char*)memchr(hp, ':', hostport_len);
    size_t host_len = colon ? (size_t)(colon - hp) : hostport_len;
    if (host_len == 0u || host_len >= 128u) {
        tz_web_set_err(out_err_ptr, out_err_len, "Host inválido en URL");
        return 0;
    }
    char host[128];
    memcpy(host, hp, host_len);
    host[host_len] = '\0';
    int port = is_https ? 443 : 80;
    if (colon) {
        int64_t pval = 0;
        if (!tz_web_parse_i64(colon + 1, hostport_len - host_len - 1u, &pval) || pval <= 0 || pval > 65535) {
            tz_web_set_err(out_err_ptr, out_err_len, "Puerto inválido en URL");
            return 0;
        }
        port = (int)pval;
    }

    tz_web_buf req;
    tz_web_buf_init(&req, 512u + blen);
    tz_web_buf_append(&req, method, mlen);
    tz_web_buf_append(&req, " ", 1u);
    tz_web_buf_append(&req, path, path_len);
    tz_web_buf_append(&req, " HTTP/1.1\r\nHost: ", 17u);
    tz_web_buf_append(&req, hp, hostport_len);
    if (is_https) tz_web_buf_append(&req, "\r\nConnection: close\r\n", 21u);
    else tz_web_buf_append(&req, "\r\nConnection: keep-alive\r\n", 26u);
    if (hlen > 0u) tz_web_buf_append(&req, extra_hdrs, hlen);
    if (blen > 0u) {
        char cl[48];
        int cln = snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", blen);
        tz_web_buf_append(&req, cl, (size_t)cln);
    }
    tz_web_buf_append(&req, "\r\n", 2u);
    if (blen > 0u) tz_web_buf_append(&req, body, blen);

    int reused = 0;
    tz_web_sock_t sock = TZ_WEB_BAD_SOCK;
    if (!is_https) {
        sock = tz_cli_borrow(host, port);
        if (sock != TZ_WEB_BAD_SOCK) reused = 1;
    }
    if (sock == TZ_WEB_BAD_SOCK) sock = tz_cli_connect_host(host, port, timeout_ms);
    if (sock == TZ_WEB_BAD_SOCK) {
        tz_web_buf_free(&req);
        tz_web_set_err(out_err_ptr, out_err_len, "No se pudo conectar al servidor");
        return 0;
    }

    void* ssl_ctx = NULL;
    void* ssl = NULL;
#ifdef TZ_USE_CLIENT_TLS
    if (is_https) {
        if (!tz_tls_init_api()) {
            tz_web_sock_close(sock);
            tz_web_buf_free(&req);
            tz_web_set_err(out_err_ptr, out_err_len, "OpenSSL (libssl) no disponible para https://");
            return 0;
        }
        ssl_ctx = tz_tls_api.ctx_new(tz_tls_api.tls_cli_method());
        if (!ssl_ctx) {
            tz_web_sock_close(sock);
            tz_web_buf_free(&req);
            tz_web_set_err(out_err_ptr, out_err_len, "Error creando SSL_CTX cliente");
            return 0;
        }
        tz_tls_api.ctx_ctrl(ssl_ctx, TZ_SSL_CTRL_SET_MIN_PROTO_VERSION, TZ_TLS1_2_VERSION, NULL);
        (void)tz_tls_api.ctx_default_verify(ssl_ctx);
        const char* ca_file = getenv("TURI_TLS_CA_FILE");
        if (ca_file && ca_file[0]) (void)tz_tls_api.ctx_load_verify(ssl_ctx, ca_file, NULL);
        tz_tls_api.ctx_set_verify(ssl_ctx, TZ_SSL_VERIFY_PEER, NULL);
        ssl = tz_tls_api.ssl_new(ssl_ctx);
        if (!ssl) {
            tz_tls_api.ctx_free(ssl_ctx);
            tz_web_sock_close(sock);
            tz_web_buf_free(&req);
            tz_web_set_err(out_err_ptr, out_err_len, "Error creando SSL cliente");
            return 0;
        }
        void* vp = tz_tls_api.ssl_get0_param ? tz_tls_api.ssl_get0_param(ssl) : NULL;
        int is_ip = (vp && tz_tls_api.param_set1_ip_asc && tz_tls_api.param_set1_ip_asc(vp, host) == 1);
        int id_ok = is_ip ? 1 : (tz_tls_api.ssl_set1_host(ssl, host) == 1);
        if (!is_ip) (void)tz_tls_api.ssl_ctrl(ssl, TZ_SSL_CTRL_SET_TLSEXT_HOSTNAME, 0L, (void*)host);
        if (!id_ok || tz_tls_api.ssl_set_fd(ssl, (int)sock) != 1 || tz_tls_api.ssl_connect(ssl) != 1 ||
            tz_tls_api.ssl_get_verify_result(ssl) != TZ_X509_V_OK) {
            tz_tls_api.ssl_free(ssl);
            tz_tls_api.ctx_free(ssl_ctx);
            tz_web_sock_close(sock);
            tz_web_buf_free(&req);
            tz_web_set_err(out_err_ptr, out_err_len, "Error de verificacion o handshake TLS en https://");
            return 0;
        }
    }
    #define TZ_CLI_CLOSE() do { \
        if (ssl) { tz_tls_api.ssl_shutdown(ssl); tz_tls_api.ssl_free(ssl); ssl = NULL; } \
        if (ssl_ctx) { tz_tls_api.ctx_free(ssl_ctx); ssl_ctx = NULL; } \
        tz_web_sock_close(sock); \
    } while (0)
    #define TZ_CLI_FINISH(cf) do { \
        if ((cf) || is_https) { TZ_CLI_CLOSE(); } \
        else { tz_cli_return(host, port, sock); } \
    } while (0)
#else
    if (is_https) {
        tz_web_sock_close(sock);
        tz_web_buf_free(&req);
        tz_web_set_err(out_err_ptr, out_err_len, "TLS cliente no habilitado");
        return 0;
    }
    #define TZ_CLI_CLOSE() tz_web_sock_close(sock)
    #define TZ_CLI_FINISH(cf) do { \
        if (cf) tz_web_sock_close(sock); \
        else tz_cli_return(host, port, sock); \
    } while (0)
#endif

    if (!tz_cli_io_send_all(sock, ssl, req.data, req.len)) {
        TZ_CLI_CLOSE();
        if (reused && !is_https) {
            sock = tz_cli_connect_host(host, port, timeout_ms);
            if (sock == TZ_WEB_BAD_SOCK || !tz_cli_io_send_all(sock, NULL, req.data, req.len)) {
                if (sock != TZ_WEB_BAD_SOCK) tz_web_sock_close(sock);
                tz_web_buf_free(&req);
                tz_web_set_err(out_err_ptr, out_err_len, "Error enviando solicitud HTTP");
                return 0;
            }
        } else {
            tz_web_buf_free(&req);
            tz_web_set_err(out_err_ptr, out_err_len, "Error enviando solicitud HTTP");
            return 0;
        }
    }
    tz_web_buf_free(&req);

    tz_web_buf resp;
    tz_web_buf_init(&resp, 8192u);
    size_t hdr_end_pos = 0;
    while (1) {
        if (!tz_web_buf_reserve(&resp, 4096u)) { TZ_CLI_CLOSE(); tz_web_buf_free(&resp); return 0; }
        int r = tz_cli_io_recv(sock, ssl, resp.data + resp.len, resp.cap - resp.len);
        if (r <= 0) break;
        resp.len += (size_t)r;
        for (size_t i = 0; i + 3u < resp.len; ++i) {
            if (resp.data[i] == '\r' && resp.data[i+1] == '\n' && resp.data[i+2] == '\r' && resp.data[i+3] == '\n') {
                hdr_end_pos = i + 4u;
                break;
            }
        }
        if (hdr_end_pos > 0u || resp.len > TZ_WEB_MAX_HDR_BYTES) break;
    }
    if (hdr_end_pos == 0u) {
        TZ_CLI_CLOSE();
        tz_web_buf_free(&resp);
        tz_web_set_err(out_err_ptr, out_err_len, "Respuesta HTTP inválida o timeout");
        return 0;
    }

    int status = 0;
    const char* sp1 = (const char*)memchr(resp.data, ' ', hdr_end_pos);
    if (sp1 && (size_t)(sp1 + 4 - resp.data) <= hdr_end_pos) {
        int64_t sv = 0;
        if (tz_web_parse_i64(sp1 + 1, 3u, &sv)) status = (int)sv;
    }
    int64_t content_len = -1;
    int chunked = 0;
    int close_conn = 0;
    const char* hp_cur = (const char*)memchr(resp.data, '\n', hdr_end_pos);
    hp_cur = hp_cur ? hp_cur + 1 : resp.data + hdr_end_pos;
    const char* hdr_start = hp_cur;
    while (hp_cur < resp.data + hdr_end_pos - 2u) {
        const char* nl = (const char*)memchr(hp_cur, '\n', (size_t)(resp.data + hdr_end_pos - hp_cur));
        size_t l = nl ? (size_t)(nl - hp_cur) : (size_t)(resp.data + hdr_end_pos - hp_cur);
        const char* nx = nl ? nl + 1 : resp.data + hdr_end_pos;
        if (l > 0u && hp_cur[l - 1u] == '\r') l--;
        if (l > 0u) {
            const char* col = (const char*)memchr(hp_cur, ':', l);
            if (col) {
                size_t kn = (size_t)(col - hp_cur);
                const char* vv = col + 1;
                size_t vn = l - kn - 1u;
                while (vn > 0u && (*vv == ' ' || *vv == '\t')) { vv++; vn--; }
                if (tz_web_eq_nocase(hp_cur, kn, "content-length", 14u)) tz_web_parse_i64(vv, vn, &content_len);
                else if (tz_web_eq_nocase(hp_cur, kn, "transfer-encoding", 17u) && tz_web_eq_nocase(vv, vn, "chunked", 7u)) chunked = 1;
                else if (tz_web_eq_nocase(hp_cur, kn, "connection", 10u) && tz_web_eq_nocase(vv, vn, "close", 5u)) close_conn = 1;
            }
        }
        hp_cur = nx;
    }

    if (chunked) {
        size_t cpos = hdr_end_pos;
        tz_web_buf decoded;
        tz_web_buf_init(&decoded, 4096u);
        while (1) {
            const char* nl = NULL;
            while (1) {
                if (cpos < resp.len) nl = (const char*)memchr(resp.data + cpos, '\n', resp.len - cpos);
                if (nl) break;
                if (!tz_web_buf_reserve(&resp, 4096u)) break;
                int r = tz_cli_io_recv(sock, ssl, resp.data + resp.len, resp.cap - resp.len);
                if (r <= 0) { close_conn = 1; break; }
                resp.len += (size_t)r;
            }
            if (!nl) break;
            size_t slen = (size_t)(nl - (resp.data + cpos));
            if (slen > 0u && resp.data[cpos + slen - 1u] == '\r') slen--;
            size_t csz = 0;
            for (size_t k = 0; k < slen; ++k) {
                if (resp.data[cpos + k] == ';') break;
                int hv = tz_web_hexval(resp.data[cpos + k]);
                if (hv >= 0) csz = (csz << 4) | (size_t)hv;
            }
            cpos = (size_t)(nl - resp.data) + 1u;
            if (csz == 0u) break;
            while (resp.len < cpos + csz + 2u) {
                if (!tz_web_buf_reserve(&resp, 4096u)) break;
                int r = tz_cli_io_recv(sock, ssl, resp.data + resp.len, resp.cap - resp.len);
                if (r <= 0) { close_conn = 1; break; }
                resp.len += (size_t)r;
            }
            size_t avail = resp.len >= cpos ? (resp.len - cpos) : 0u;
            size_t take = avail < csz ? avail : csz;
            tz_web_buf_append(&decoded, resp.data + cpos, take);
            cpos += csz + 2u;
        }
        TZ_CLI_FINISH(close_conn);
        if (out_status) *out_status = status;
        if (out_body_ptr) *out_body_ptr = (uint8_t*)decoded.data;
        if (out_body_len) *out_body_len = (int64_t)decoded.len;
        size_t hl = (size_t)((resp.data + hdr_end_pos) - hdr_start);
        if (out_hdr_ptr) *out_hdr_ptr = (uint8_t*)tz_web_dup_cstr(hdr_start, hl);
        if (out_hdr_len) *out_hdr_len = (int64_t)hl;
        tz_web_buf_free(&resp);
        return 1;
    }

    if (content_len >= 0) {
        size_t need_total = hdr_end_pos + (size_t)content_len;
        while (resp.len < need_total) {
            if (!tz_web_buf_reserve(&resp, need_total - resp.len)) break;
            int r = tz_cli_io_recv(sock, ssl, resp.data + resp.len, need_total - resp.len);
            if (r <= 0) { close_conn = 1; break; }
            resp.len += (size_t)r;
        }
    } else if (!close_conn) {
        /* No Content-Length and not chunked: read until EOF */
        close_conn = 1;
        while (1) {
            if (!tz_web_buf_reserve(&resp, 4096u)) break;
            int r = tz_cli_io_recv(sock, ssl, resp.data + resp.len, resp.cap - resp.len);
            if (r <= 0) break;
            resp.len += (size_t)r;
        }
    }
    TZ_CLI_FINISH(close_conn);
    #undef TZ_CLI_CLOSE
    #undef TZ_CLI_FINISH

    size_t bl_out = resp.len > hdr_end_pos ? (resp.len - hdr_end_pos) : 0u;
    if (content_len >= 0 && bl_out > (size_t)content_len) bl_out = (size_t)content_len;
    if (out_status) *out_status = status;
    if (out_body_ptr) *out_body_ptr = (uint8_t*)tz_web_dup_cstr(resp.data + hdr_end_pos, bl_out);
    if (out_body_len) *out_body_len = (int64_t)bl_out;
    size_t hl = (size_t)((resp.data + hdr_end_pos) - hdr_start);
    if (out_hdr_ptr) *out_hdr_ptr = (uint8_t*)tz_web_dup_cstr(hdr_start, hl);
    if (out_hdr_len) *out_hdr_len = (int64_t)hl;
    tz_web_buf_free(&resp);
    return 1;
}
)c";

inline constexpr const char* kSqliteRuntime = R"c(
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

static struct {
    int loaded;
    int ok;
    tz_web_lock_t lock;
    int (*open_v2)(const char*, sqlite3**, int, const char*);
    int (*close_v2)(sqlite3*);
    int (*exec)(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
    int (*prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
    int (*step)(sqlite3_stmt*);
    int (*reset)(sqlite3_stmt*);
    int (*finalize)(sqlite3_stmt*);
    int (*bind_int64)(sqlite3_stmt*, int, int64_t);
    int (*bind_double)(sqlite3_stmt*, int, double);
    int (*bind_text)(sqlite3_stmt*, int, const char*, int, void(*)(void*));
    int (*bind_null)(sqlite3_stmt*, int);
    int (*column_count)(sqlite3_stmt*);
    int (*column_type)(sqlite3_stmt*, int);
    int64_t (*column_int64)(sqlite3_stmt*, int);
    double (*column_double)(sqlite3_stmt*, int);
    const unsigned char* (*column_text)(sqlite3_stmt*, int);
    int (*column_bytes)(sqlite3_stmt*, int);
    const char* (*errmsg)(sqlite3*);
    void (*sql_free)(void*);
} tz_sql_api = {0};

static int tz_sql_init_api(void) {
    if (__atomic_load_n(&tz_sql_api.loaded, __ATOMIC_ACQUIRE) == 2) return tz_sql_api.ok;
    int exp = 0;
    if (__atomic_compare_exchange_n(&tz_sql_api.loaded, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        tz_web_lock_init(&tz_sql_api.lock);
        int ok = 0;
#ifdef _WIN32
        HMODULE mod = NULL;
        char env_buf[512];
        if (GetEnvironmentVariableA("TURI_SQLITE_PATH", env_buf, sizeof(env_buf)) > 0) {
            mod = LoadLibraryA(env_buf);
        }
        if (!mod) {
            char exe_dir[512];
            DWORD n = GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
            if (n > 0 && n < sizeof(exe_dir)) {
                char* sep = strrchr(exe_dir, '\\');
                if (sep) {
                    snprintf(sep + 1, sizeof(exe_dir) - (size_t)(sep + 1 - exe_dir), "sqlite3.dll");
                    mod = LoadLibraryA(exe_dir);
                }
            }
        }
        static const char* kCandidates[] = {
            "sqlite3.dll",
            "C:\\Python311\\DLLs\\sqlite3.dll",
            "C:\\Python312\\DLLs\\sqlite3.dll",
            "C:\\Python313\\DLLs\\sqlite3.dll",
            NULL
        };
        for (int i = 0; !mod && kCandidates[i]; ++i) mod = LoadLibraryA(kCandidates[i]);
        if (mod) {
            int (*init_fn)(void) = NULL;
            (void)tz_web_win_sym(mod, "sqlite3_initialize", &init_fn, sizeof(init_fn));
            #define TZ_SQL_SYM(field, name) tz_web_win_sym(mod, name, &tz_sql_api.field, sizeof(tz_sql_api.field))
            if (TZ_SQL_SYM(open_v2, "sqlite3_open_v2") &&
                TZ_SQL_SYM(close_v2, "sqlite3_close_v2") &&
                TZ_SQL_SYM(exec, "sqlite3_exec") &&
                TZ_SQL_SYM(prepare_v2, "sqlite3_prepare_v2") &&
                TZ_SQL_SYM(step, "sqlite3_step") &&
                TZ_SQL_SYM(reset, "sqlite3_reset") &&
                TZ_SQL_SYM(finalize, "sqlite3_finalize") &&
                TZ_SQL_SYM(bind_int64, "sqlite3_bind_int64") &&
                TZ_SQL_SYM(bind_double, "sqlite3_bind_double") &&
                TZ_SQL_SYM(bind_text, "sqlite3_bind_text") &&
                TZ_SQL_SYM(bind_null, "sqlite3_bind_null") &&
                TZ_SQL_SYM(column_count, "sqlite3_column_count") &&
                TZ_SQL_SYM(column_type, "sqlite3_column_type") &&
                TZ_SQL_SYM(column_int64, "sqlite3_column_int64") &&
                TZ_SQL_SYM(column_double, "sqlite3_column_double") &&
                TZ_SQL_SYM(column_text, "sqlite3_column_text") &&
                TZ_SQL_SYM(column_bytes, "sqlite3_column_bytes") &&
                TZ_SQL_SYM(errmsg, "sqlite3_errmsg") &&
                TZ_SQL_SYM(sql_free, "sqlite3_free")) {
                if (init_fn) (void)init_fn();
                ok = 1;
            }
            #undef TZ_SQL_SYM
        }
#else
        void* mod = NULL;
        const char* env_p = getenv("TURI_SQLITE_PATH");
        if (env_p && *env_p) mod = dlopen(env_p, RTLD_LAZY | RTLD_LOCAL);
        static const char* kCandidates[] = { "libsqlite3.so.0", "libsqlite3.so", "libsqlite3.dylib", NULL };
        for (int i = 0; !mod && kCandidates[i]; ++i) mod = dlopen(kCandidates[i], RTLD_LAZY | RTLD_LOCAL);
        if (mod) {
            int (*init_fn)(void) = NULL;
            (void)tz_web_posix_sym(mod, "sqlite3_initialize", &init_fn, sizeof(init_fn));
            #define TZ_SQL_SYM(field, name) tz_web_posix_sym(mod, name, &tz_sql_api.field, sizeof(tz_sql_api.field))
            if (TZ_SQL_SYM(open_v2, "sqlite3_open_v2") &&
                TZ_SQL_SYM(close_v2, "sqlite3_close_v2") &&
                TZ_SQL_SYM(exec, "sqlite3_exec") &&
                TZ_SQL_SYM(prepare_v2, "sqlite3_prepare_v2") &&
                TZ_SQL_SYM(step, "sqlite3_step") &&
                TZ_SQL_SYM(reset, "sqlite3_reset") &&
                TZ_SQL_SYM(finalize, "sqlite3_finalize") &&
                TZ_SQL_SYM(bind_int64, "sqlite3_bind_int64") &&
                TZ_SQL_SYM(bind_double, "sqlite3_bind_double") &&
                TZ_SQL_SYM(bind_text, "sqlite3_bind_text") &&
                TZ_SQL_SYM(bind_null, "sqlite3_bind_null") &&
                TZ_SQL_SYM(column_count, "sqlite3_column_count") &&
                TZ_SQL_SYM(column_type, "sqlite3_column_type") &&
                TZ_SQL_SYM(column_int64, "sqlite3_column_int64") &&
                TZ_SQL_SYM(column_double, "sqlite3_column_double") &&
                TZ_SQL_SYM(column_text, "sqlite3_column_text") &&
                TZ_SQL_SYM(column_bytes, "sqlite3_column_bytes") &&
                TZ_SQL_SYM(errmsg, "sqlite3_errmsg") &&
                TZ_SQL_SYM(sql_free, "sqlite3_free")) {
                if (init_fn) (void)init_fn();
                ok = 1;
            }
            #undef TZ_SQL_SYM
        }
#endif
        tz_sql_api.ok = ok;
        __atomic_store_n(&tz_sql_api.loaded, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&tz_sql_api.loaded, __ATOMIC_ACQUIRE) != 2) {
#ifdef _WIN32
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
    return tz_sql_api.ok;
}

int32_t tz_web_sql_available(void) {
    return tz_sql_init_api() ? 1 : 0;
}

void* tz_web_sql_open(const void* path_tstr, int32_t readonly, const void** err_ptr, int64_t* err_len) {
    tz_web_set_err(err_ptr, err_len, NULL);
    if (!tz_sql_init_api()) {
        tz_web_set_err(err_ptr, err_len, "sqlite3.dll / libsqlite3.so no encontrado");
        return NULL;
    }
    size_t plen = 0;
    const char* path = tz_web_str_view(path_tstr, &plen);
    char* cpath = tz_web_dup_cstr(path, plen);
    sqlite3* db = NULL;
    int flags = readonly ? 0x00010001 : 0x00010006; /* READONLY | FULLMUTEX or RW|CREATE|FULLMUTEX */
    int rc = tz_sql_api.open_v2(cpath, &db, flags, NULL);
    free(cpath);
    if (rc != 0 || !db) {
        tz_web_set_err(err_ptr, err_len, db ? tz_sql_api.errmsg(db) : "Error abriendo SQLite");
        if (db) tz_sql_api.close_v2(db);
        return NULL;
    }
    if (!readonly) {
        char* em = NULL;
        tz_sql_api.exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=5000; PRAGMA foreign_keys=ON;", NULL, NULL, &em);
        if (em) tz_sql_api.sql_free(em);
    } else {
        char* em = NULL;
        tz_sql_api.exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, &em);
        if (em) tz_sql_api.sql_free(em);
    }
    return (void*)db;
}

void tz_web_sql_close(void* db) {
    if (db && tz_sql_init_api()) tz_sql_api.close_v2((sqlite3*)db);
}

int32_t tz_web_sql_exec(void* db, const void* sql_tstr, const void** err_ptr, int64_t* err_len) {
    tz_web_set_err(err_ptr, err_len, NULL);
    if (!db || !tz_sql_init_api()) { tz_web_set_err(err_ptr, err_len, "BD cerrada"); return 0; }
    size_t slen = 0;
    const char* sql = tz_web_str_view(sql_tstr, &slen);
    char* csql = tz_web_dup_cstr(sql, slen);
    char* em = NULL;
    int rc = tz_sql_api.exec((sqlite3*)db, csql, NULL, NULL, &em);
    free(csql);
    if (rc != 0) {
        tz_web_set_err(err_ptr, err_len, em ? em : tz_sql_api.errmsg((sqlite3*)db));
        if (em) tz_sql_api.sql_free(em);
        return 0;
    }
    return 1;
}

void* tz_web_sql_prepare(void* db, const void* sql_tstr, const void** err_ptr, int64_t* err_len) {
    tz_web_set_err(err_ptr, err_len, NULL);
    if (!db || !tz_sql_init_api()) { tz_web_set_err(err_ptr, err_len, "BD cerrada"); return NULL; }
    size_t slen = 0;
    const char* sql = tz_web_str_view(sql_tstr, &slen);
    sqlite3_stmt* stmt = NULL;
    int rc = tz_sql_api.prepare_v2((sqlite3*)db, sql, (int)slen, &stmt, NULL);
    if (rc != 0 || !stmt) {
        tz_web_set_err(err_ptr, err_len, tz_sql_api.errmsg((sqlite3*)db));
        return NULL;
    }
    return (void*)stmt;
}

int32_t tz_web_sql_bind_i64(void* stmt, int32_t idx, int64_t val) {
    if (!stmt || !tz_sql_init_api()) return 0;
    return tz_sql_api.bind_int64((sqlite3_stmt*)stmt, idx, val) == 0 ? 1 : 0;
}

int32_t tz_web_sql_bind_f64(void* stmt, int32_t idx, double val) {
    if (!stmt || !tz_sql_init_api()) return 0;
    return tz_sql_api.bind_double((sqlite3_stmt*)stmt, idx, val) == 0 ? 1 : 0;
}

int32_t tz_web_sql_bind_str(void* stmt, int32_t idx, const void* val_tstr) {
    if (!stmt || !tz_sql_init_api()) return 0;
    size_t n = 0;
    const char* p = tz_web_str_view(val_tstr, &n);
    return tz_sql_api.bind_text((sqlite3_stmt*)stmt, idx, p, (int)n, (void(*)(void*))(intptr_t)-1) == 0 ? 1 : 0;
}

int32_t tz_web_sql_bind_null(void* stmt, int32_t idx) {
    if (!stmt || !tz_sql_init_api()) return 0;
    return tz_sql_api.bind_null((sqlite3_stmt*)stmt, idx) == 0 ? 1 : 0;
}

int32_t tz_web_sql_step(void* stmt) {
    if (!stmt || !tz_sql_init_api()) return -1;
    int rc = tz_sql_api.step((sqlite3_stmt*)stmt);
    if (rc == 100) return 1; /* SQLITE_ROW */
    if (rc == 101) return 0; /* SQLITE_DONE */
    return -1;
}

void tz_web_sql_reset(void* stmt) {
    if (stmt && tz_sql_init_api()) tz_sql_api.reset((sqlite3_stmt*)stmt);
}

void tz_web_sql_finalize(void* stmt) {
    if (stmt && tz_sql_init_api()) tz_sql_api.finalize((sqlite3_stmt*)stmt);
}

int32_t tz_web_sql_col_count(void* stmt) {
    return (stmt && tz_sql_init_api()) ? tz_sql_api.column_count((sqlite3_stmt*)stmt) : 0;
}

int32_t tz_web_sql_col_is_null(void* stmt, int32_t col) {
    if (!stmt || !tz_sql_init_api()) return 1;
    return tz_sql_api.column_type((sqlite3_stmt*)stmt, col) == 5 ? 1 : 0;
}

int64_t tz_web_sql_col_i64(void* stmt, int32_t col) {
    return (stmt && tz_sql_init_api()) ? tz_sql_api.column_int64((sqlite3_stmt*)stmt, col) : 0;
}

double tz_web_sql_col_f64(void* stmt, int32_t col) {
    return (stmt && tz_sql_init_api()) ? tz_sql_api.column_double((sqlite3_stmt*)stmt, col) : 0.0;
}

const uint8_t* tz_web_sql_col_str(void* stmt, int32_t col, int64_t* out_len) {
    if (out_len) *out_len = 0;
    if (!stmt || !tz_sql_init_api()) return (const uint8_t*)"";
    const unsigned char* txt = tz_sql_api.column_text((sqlite3_stmt*)stmt, col);
    if (!txt) return (const uint8_t*)"";
    int n = tz_sql_api.column_bytes((sqlite3_stmt*)stmt, col);
    if (out_len) *out_len = (int64_t)n;
    return (const uint8_t*)txt;
}

const uint8_t* tz_web_sql_errmsg(void* db, int64_t* out_len) {
    if (out_len) *out_len = 0;
    if (!db || !tz_sql_init_api()) return (const uint8_t*)"";
    const char* m = tz_sql_api.errmsg((sqlite3*)db);
    if (!m) return (const uint8_t*)"";
    if (out_len) *out_len = (int64_t)strlen(m);
    return (const uint8_t*)m;
}
)c";

inline constexpr const char* kTlsRuntime = R"c(
int32_t tz_web_tls_available(void) {
    return tz_tls_init_api() ? 1 : 0;
}

int32_t tz_web_tls_configure(void* sp, const void* cert_tstr, const void* key_tstr,
                             const void** err_ptr, int64_t* err_len) {
    tz_web_set_err(err_ptr, err_len, NULL);
    if (!tz_tls_init_api()) {
        tz_web_set_err(err_ptr, err_len, "OpenSSL (libssl) no disponible en el sistema");
        return 0;
    }
#ifdef TZ_HAS_SERVER
    tz_web_server* srv = (tz_web_server*)sp;
    if (!srv) {
        tz_web_set_err(err_ptr, err_len, "Servidor nulo al configurar TLS");
        return 0;
    }
    if (srv->started) {
        tz_web_set_err(err_ptr, err_len, "Configura TLS antes de iniciar el servidor");
        return 0;
    }
#else
    (void)sp;
#endif
    size_t cl = 0, kl = 0;
    const char* cert = tz_web_str_view(cert_tstr, &cl);
    const char* key = tz_web_str_view(key_tstr, &kl);
    char* cpath = tz_web_dup_cstr(cert, cl);
    char* kpath = tz_web_dup_cstr(key, kl);
    void* ctx = tz_tls_api.ctx_new(tz_tls_api.tls_srv_method());
    if (!ctx) {
        free(cpath); free(kpath);
        tz_web_set_err(err_ptr, err_len, "Error creando SSL_CTX");
        return 0;
    }
    if (tz_tls_api.ctx_ctrl(ctx, TZ_SSL_CTRL_SET_MIN_PROTO_VERSION, TZ_TLS1_3_VERSION, NULL) != 1) {
        tz_tls_api.ctx_free(ctx);
        free(cpath); free(kpath);
        tz_web_set_err(err_ptr, err_len, "OpenSSL no soporta TLS 1.3 minimo");
        return 0;
    }
    int ok1 = tz_tls_api.use_cert_chain
        ? tz_tls_api.use_cert_chain(ctx, cpath)
        : tz_tls_api.use_cert(ctx, cpath, TZ_SSL_FILETYPE_PEM);
    int ok2 = tz_tls_api.use_key(ctx, kpath, TZ_SSL_FILETYPE_PEM);
    int ok3 = (ok1 == 1 && ok2 == 1) ? tz_tls_api.check_key(ctx) : 0;
    free(cpath); free(kpath);
    if (ok1 != 1 || ok2 != 1 || ok3 != 1) {
        tz_tls_api.ctx_free(ctx);
        tz_web_set_err(err_ptr, err_len, "No se pudo cargar o validar certificado/clave PEM");
        return 0;
    }
#ifdef TZ_HAS_SERVER
    if (srv->tls_ctx) tz_tls_api.ctx_free(srv->tls_ctx);
    srv->tls_ctx = ctx;
#else
    tz_tls_api.ctx_free(ctx);
#endif
    return 1;
}
)c";

} // namespace web_rt_parts

inline std::string turiWebRuntimeC(bool needSrv, bool needWs, bool needClient, bool needSql, bool needTls) {
    std::string out;
    out.reserve(65536);
    if (needSrv || needWs) out += "#define TZ_HAS_SERVER 1\n";
    if (needTls) out += "#define TZ_USE_TLS 1\n";
    if (needClient) out += "#define TZ_USE_CLIENT_TLS 1\n";
    out += web_rt_parts::kCommonHeader;
    out += web_rt_parts::kCommonSimd;
    out += web_rt_parts::kCommonUtil;
    if (needTls || needClient) {
        out += web_rt_parts::kTlsApiLoader;
    }
    if (needSrv || needWs) {
        out += web_rt_parts::kServerCore;
        out += web_rt_parts::kServerHttp;
        out += web_rt_parts::kServerRouter;
        if (needWs) {
            out += web_rt_parts::kServerWsFull;
            out += web_rt_parts::kServerWsApi;
        } else {
            out += web_rt_parts::kServerWsStub;
        }
        out += web_rt_parts::kServerDispatchAndApi;
        out += web_rt_parts::kServerDispatchLoop;
        if (needTls) {
            out += web_rt_parts::kServerTls;
        }
        out += web_rt_parts::kServerPlatformWin;
        out += web_rt_parts::kServerWinEngine;
        out += web_rt_parts::kServerPlatformPosix;
        out += web_rt_parts::kServerPosixPoll;
        out += web_rt_parts::kServerApi;
        out += web_rt_parts::kServerCtxApi;
    }
    if (needClient) {
        out += web_rt_parts::kClientRuntime;
        out += web_rt_parts::kClientExec;
    }
    if (needSql) {
        out += web_rt_parts::kSqliteRuntime;
    }
    if (needTls) {
        out += web_rt_parts::kTlsRuntime;
    }
    return out;
}

} // namespace veloz::native
