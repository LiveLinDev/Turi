#pragma once

namespace veloz::native {

inline const char* turiCollectionsRuntimeC() {
    return R"c(
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifdef _WIN32
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
#include <intrin.h>
#endif

typedef struct tz_gstr {
    uint32_t hdr;
    uint32_t pref;
    uintptr_t tail;
} tz_gstr;

typedef struct tz_table {
    uint8_t* data;
    uint32_t len;
    uint32_t buckets;
    uint32_t growth_left;
    uint32_t flags;
    uint64_t seed;
} tz_table;

typedef void (*tz_drop_fn)(void* slot);
typedef void (*tz_clone_fn)(void* dst, const void* src, const void* diag);

enum {
    TZ_KEY_I64 = 0,
    TZ_KEY_U8 = 1,
    TZ_KEY_U32 = 2,
    TZ_KEY_STR = 3,
    TZ_KEY_U16 = 4
};

/* El hash, la comparación de claves y la búsqueda por grupo se integran siempre en quien las llama:
   con el tipo de clave fijo en cada punto de llamada, el compilador elimina el reparto por tipo y,
   si la longitud de la cadena se conoce, también el reparto por longitud. */
#if defined(__GNUC__) || defined(__clang__)
#define TZ_HOT_INLINE inline __attribute__((always_inline))
#else
#define TZ_HOT_INLINE inline
#endif

#define TZ_CTRL_EMPTY 0xFFu
#define TZ_CTRL_DELETED 0x80u
#define TZ_MAX_BUCKETS (1u << 24)
#define TZ_FOLDHASH_SEED UINT64_C(0x243F6A8885A308D3)
#define TZ_FOLDHASH_MULT UINT64_C(0x2D358DCCAA6C78A5)
#define TZ_FOLDHASH_MULT2 UINT64_C(0x8BB84B93962EACC9)

static TZ_HOT_INLINE uint32_t tz_gstr_len(const tz_gstr* s) {
    return s->hdr & 0x7FFFFFFFu;
}

static TZ_HOT_INLINE const uint8_t* tz_gstr_bytes(const tz_gstr* s) {
    return tz_gstr_len(s) <= 12u ? ((const uint8_t*)s + 4) : (const uint8_t*)s->tail;
}

static inline uint32_t tz_bswap32(uint32_t x) {
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) |
           ((x & 0xFF000000u) >> 24);
}

static TZ_HOT_INLINE uint64_t tz_folded_mul(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    __uint128_t prod = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)prod ^ (uint64_t)(prod >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    uint64_t hi = 0;
    uint64_t lo = _umul128(a, b, &hi);
    return lo ^ hi;
#else
    uint64_t a_lo = (uint32_t)a;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = (uint32_t)b;
    uint64_t b_hi = b >> 32;
    uint64_t p00 = a_lo * b_lo;
    uint64_t p01 = a_lo * b_hi;
    uint64_t p10 = a_hi * b_lo;
    uint64_t p11 = a_hi * b_hi;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    uint64_t lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return lo ^ hi;
#endif
}

static TZ_HOT_INLINE uint64_t tz_hash_u64(uint64_t x, uint64_t seed) {
    return tz_folded_mul(x ^ seed, TZ_FOLDHASH_MULT);
}

static TZ_HOT_INLINE uint64_t tz_hash_bytes(const uint8_t* p, size_t n, uint64_t seed) {
    uint64_t h = tz_folded_mul(seed ^ (uint64_t)n, TZ_FOLDHASH_MULT2);
    if (n == 0) return h;
    while (n >= 16u) {
        uint64_t a = 0, b = 0;
        memcpy(&a, p, 8);
        memcpy(&b, p + 8, 8);
        h = tz_folded_mul(a ^ h, b ^ TZ_FOLDHASH_MULT);
        p += 16;
        n -= 16;
    }
    if (n >= 8u) {
        uint64_t a = 0, b = 0;
        memcpy(&a, p, 8);
        memcpy(&b, p + (n - 8u), 8);
        h = tz_folded_mul(a ^ h, b ^ TZ_FOLDHASH_MULT2);
    } else if (n >= 4u) {
        uint32_t a = 0, b = 0;
        memcpy(&a, p, 4);
        memcpy(&b, p + (n - 4u), 4);
        h = tz_folded_mul(((uint64_t)a) ^ h, ((uint64_t)b) ^ TZ_FOLDHASH_MULT2);
    } else if (n > 0u) {
        uint64_t tail = ((uint64_t)p[0]) |
                        ((uint64_t)p[n >> 1] << 8) |
                        ((uint64_t)p[n - 1u] << 16);
        h = tz_folded_mul(tail ^ h, TZ_FOLDHASH_MULT);
    }
    return h;
}

static inline uint64_t tz_rotl64(uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}

#define TZ_SIPROUND(v0, v1, v2, v3) \
    do {                            \
        (v0) += (v1);               \
        (v1) = tz_rotl64((v1), 13); \
        (v1) ^= (v0);               \
        (v0) = tz_rotl64((v0), 32); \
        (v2) += (v3);               \
        (v3) = tz_rotl64((v3), 16); \
        (v3) ^= (v2);               \
        (v0) += (v3);               \
        (v3) = tz_rotl64((v3), 21); \
        (v3) ^= (v0);               \
        (v2) += (v1);               \
        (v1) = tz_rotl64((v1), 17); \
        (v1) ^= (v2);               \
        (v2) = tz_rotl64((v2), 32); \
    } while (0)

static inline uint64_t tz_siphash13(const uint8_t* p, size_t len, uint64_t k0, uint64_t k1) {
    uint64_t v0 = UINT64_C(0x736f6d6570736575) ^ k0;
    uint64_t v1 = UINT64_C(0x646f72616e646f6d) ^ k1;
    uint64_t v2 = UINT64_C(0x6c7967656e657261) ^ k0;
    uint64_t v3 = UINT64_C(0x7465646279746573) ^ k1;
    size_t blocks = len / 8u;
    for (size_t i = 0; i < blocks; ++i) {
        uint64_t m = 0;
        memcpy(&m, p + i * 8u, 8);
        v3 ^= m;
        TZ_SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
    }
    const uint8_t* tail = p + blocks * 8u;
    uint64_t b = ((uint64_t)len) << 56;
    for (size_t k = 0u; k < (len & 7u); ++k) {
        b |= ((uint64_t)tail[k]) << (k * 8u);
    }
    v3 ^= b;
    TZ_SIPROUND(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xFFu;
    TZ_SIPROUND(v0, v1, v2, v3);
    TZ_SIPROUND(v0, v1, v2, v3);
    TZ_SIPROUND(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

static uint64_t tz_random_seed(void) {
    uint64_t s = 0;
#ifdef _WIN32
    unsigned int r0 = 0, r1 = 0;
    if (rand_s(&r0) == 0 && rand_s(&r1) == 0) {
        s = ((uint64_t)r1 << 32) | (uint64_t)r0;
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t got = read(fd, &s, sizeof(s));
        close(fd);
        if (got != (ssize_t)sizeof(s)) s = 0;
    }
#endif
    if (s == 0) {
        uint64_t x = ((uint64_t)(uintptr_t)&s) ^ ((uint64_t)time(NULL) * UINT64_C(0x9E3779B97F4A7C15));
        x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
        x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
        s = x ^ (x >> 31);
        if (s == 0) s = TZ_FOLDHASH_SEED;
    }
    return s;
}

void tz_col_table_init_secure(tz_table* t) {
    memset(t, 0, sizeof(*t));
    t->flags = 1u;
    t->seed = tz_random_seed();
}

/* Mismo valor que tz_hash_bytes para una cadena en línea (hasta 12 bytes), calculado con los tres
   campos de la cadena (longitud, prefijo de 4 bytes y cola de 8) en vez de leer sus bytes de memoria.
   La clave llega recién escrita en tres trozos de 4, 4 y 8 bytes; una lectura de 8 bytes a caballo
   entre dos de ellos no se puede reenviar desde las escrituras pendientes y obliga al procesador a
   esperar a que se retiren, lo que encadena cada búsqueda con la anterior. Leyendo por campos no hay
   lectura de memoria y las búsquedas de vueltas distintas se solapan. */
static TZ_HOT_INLINE uint64_t tz_hash_inline_str(uint32_t n, uint32_t pref, uint64_t tail, uint64_t seed) {
    uint64_t h = tz_folded_mul(seed ^ (uint64_t)n, TZ_FOLDHASH_MULT2);
    if (n == 0u) return h;
    if (n >= 8u) {
        const uint32_t sh = 8u * (n - 8u); /* 0..32 */
        const uint64_t a = (uint64_t)pref | (tail << 32);
        const uint64_t b = ((uint64_t)pref >> sh) | (tail << (32u - sh));
        return tz_folded_mul(a ^ h, b ^ TZ_FOLDHASH_MULT2);
    }
    if (n >= 4u) {
        const uint32_t sh = 8u * (n - 4u); /* 0..24 */
        const uint32_t b = (uint32_t)(((uint64_t)pref >> sh) | (tail << (32u - sh)));
        return tz_folded_mul(((uint64_t)pref) ^ h, ((uint64_t)b) ^ TZ_FOLDHASH_MULT2);
    }
    {
        const uint64_t bytes = ((uint64_t)(pref & 0xFFu)) |
                               ((uint64_t)((pref >> (8u * (n >> 1))) & 0xFFu) << 8) |
                               ((uint64_t)((pref >> (8u * (n - 1u))) & 0xFFu) << 16);
        return tz_folded_mul(bytes ^ h, TZ_FOLDHASH_MULT);
    }
}

static TZ_HOT_INLINE uint64_t tz_hash_key(const tz_table* t, const void* key_ptr, int32_t kk) {
    const uint32_t secure = t->flags & 1u;
    const uint64_t seed = secure ? t->seed : TZ_FOLDHASH_SEED;
    if (kk == TZ_KEY_STR) {
        const tz_gstr* s = (const tz_gstr*)key_ptr;
        uint32_t len = tz_gstr_len(s);
        const uint8_t* p = tz_gstr_bytes(s);
        if (secure) return tz_siphash13(p, (size_t)len, seed, tz_folded_mul(seed, TZ_FOLDHASH_MULT));
        if (len <= 12u) return tz_hash_inline_str(len, s->pref, (uint64_t)s->tail, seed);
        return tz_hash_bytes(p, (size_t)len, seed);
    }
    uint64_t k = 0;
    if (kk == TZ_KEY_I64) {
        memcpy(&k, key_ptr, 8);
    } else if (kk == TZ_KEY_U32) {
        uint32_t u = 0;
        memcpy(&u, key_ptr, 4);
        k = (uint64_t)u;
    } else if (kk == TZ_KEY_U16) {
        uint16_t u = 0;
        memcpy(&u, key_ptr, 2);
        k = (uint64_t)u;
    } else {
        k = (uint64_t)(*(const uint8_t*)key_ptr);
    }
    if (secure) {
        uint8_t buf[8];
        memcpy(buf, &k, 8);
        return tz_siphash13(buf, 8u, seed, tz_folded_mul(seed, TZ_FOLDHASH_MULT));
    }
    return tz_hash_u64(k, seed);
}

static TZ_HOT_INLINE uint8_t tz_h2(uint64_t h) {
    return (uint8_t)((h >> 57) & 0x7Fu);
}

static TZ_HOT_INLINE uint8_t tz_gstr_eq(const tz_gstr* a, const tz_gstr* b) {
    uint32_t la = tz_gstr_len(a);
    uint32_t lb = tz_gstr_len(b);
    if (la != lb) return 0u;
    if (la == 0u) return 1u;
    if (a->pref != b->pref) return 0u;
    if (la <= 12u) return a->tail == b->tail;
    if (a->tail == b->tail) return 1u;
    return memcmp(((const uint8_t*)a->tail) + 4, ((const uint8_t*)b->tail) + 4, (size_t)(la - 4u)) == 0;
}

static inline int tz_gstr_cmp(const tz_gstr* a, const tz_gstr* b) {
    uint32_t la = tz_gstr_len(a);
    uint32_t lb = tz_gstr_len(b);
    if (la >= 4u && lb >= 4u) {
        uint32_t pa = tz_bswap32(a->pref);
        uint32_t pb = tz_bswap32(b->pref);
        if (pa != pb) return pa < pb ? -1 : 1;
    }
    uint32_t min_l = la < lb ? la : lb;
    if (min_l > 0u) {
        int c = memcmp(tz_gstr_bytes(a), tz_gstr_bytes(b), (size_t)min_l);
        if (c != 0) return c < 0 ? -1 : 1;
    }
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

static inline uint8_t tz_gstr_clone_into(tz_gstr* dst, const tz_gstr* src) {
    *dst = *src;
    if (src->hdr & 0x80000000u) {
        uint32_t len = src->hdr & 0x7FFFFFFFu;
        uint8_t* p = (uint8_t*)malloc(len > 0u ? (size_t)len : 1u);
        if (!p) return 0u;
        if (len > 0u) memcpy(p, (const void*)src->tail, (size_t)len);
        dst->tail = (uintptr_t)p;
    }
    return 1u;
}

static inline void tz_gstr_drop(tz_gstr* s) {
    if (s->hdr & 0x80000000u) {
        free((void*)s->tail);
    }
    memset(s, 0, sizeof(*s));
}

static TZ_HOT_INLINE uint8_t tz_key_eq(const void* a, const void* b, int32_t kk) {
    if (kk == TZ_KEY_STR) return tz_gstr_eq((const tz_gstr*)a, (const tz_gstr*)b);
    if (kk == TZ_KEY_I64) {
        int64_t va = 0, vb = 0;
        memcpy(&va, a, 8);
        memcpy(&vb, b, 8);
        return va == vb;
    }
    if (kk == TZ_KEY_U32) {
        uint32_t va = 0, vb = 0;
        memcpy(&va, a, 4);
        memcpy(&vb, b, 4);
        return va == vb;
    }
    if (kk == TZ_KEY_U16) {
        uint16_t va = 0, vb = 0;
        memcpy(&va, a, 2);
        memcpy(&vb, b, 2);
        return va == vb;
    }
    return *(const uint8_t*)a == *(const uint8_t*)b;
}

static TZ_HOT_INLINE uint16_t tz_match_byte(const uint8_t* ctrl_group, uint8_t target) {
#if defined(__SSE2__)
    __m128i g = _mm_loadu_si128((const __m128i*)ctrl_group);
    return (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(g, _mm_set1_epi8((char)target)));
#elif defined(__aarch64__)
    static const uint8_t bits[16] = {1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128};
    uint8x16_t g = vld1q_u8(ctrl_group);
    uint8x16_t eq = vceqq_u8(g, vdupq_n_u8(target));
    uint8x16_t masked = vandq_u8(eq, vld1q_u8(bits));
    return (uint16_t)vaddv_u8(vget_low_u8(masked)) |
           ((uint16_t)vaddv_u8(vget_high_u8(masked)) << 8);
#else
    uint16_t m = 0;
    for (uint32_t k = 0; k < 16u; ++k) {
        if (ctrl_group[k] == target) m |= (uint16_t)(1u << k);
    }
    return m;
#endif
}

static TZ_HOT_INLINE uint32_t tz_ctz16(uint16_t m) {
#if defined(__clang__) || defined(__GNUC__)
    return (uint32_t)__builtin_ctz((unsigned)m);
#else
    uint32_t c = 0;
    while ((m & 1u) == 0u) { m >>= 1; ++c; }
    return c;
#endif
}

static inline void tz_set_ctrl(uint8_t* ctrl, uint32_t buckets, uint32_t idx, uint8_t c) {
    ctrl[idx] = c;
    if (idx < 16u) ctrl[buckets + idx] = c;
}

static inline uint32_t tz_max_load(uint32_t buckets) {
    return (uint32_t)(((uint64_t)buckets * 7ULL) >> 3);
}

static inline uint32_t tz_min_buckets_for(uint64_t cap) {
    uint64_t b = 16;
    while (((b * 7ULL) >> 3) < cap) {
        if (b >= TZ_MAX_BUCKETS) return 0u;
        b <<= 1;
    }
    return (uint32_t)b;
}

static uint8_t* tz_alloc_table_buf(uint32_t buckets, int64_t ks, int64_t vs) {
    if (buckets < 16u || buckets > TZ_MAX_BUCKETS || ks <= 0 || vs < 0) return NULL;
    size_t ctrl_bytes = (size_t)buckets + 16u;
    size_t u_b = (size_t)buckets;
    size_t u_ks = (size_t)ks;
    size_t u_vs = (size_t)vs;
    if (u_ks > (SIZE_MAX - ctrl_bytes) / u_b) return NULL;
    size_t key_bytes = u_b * u_ks;
    if (u_vs > 0 && u_vs > (SIZE_MAX - ctrl_bytes - key_bytes) / u_b) return NULL;
    size_t val_bytes = u_b * u_vs;
    uint8_t* p = (uint8_t*)malloc(ctrl_bytes + key_bytes + val_bytes);
    if (!p) return NULL;
    memset(p, (int)TZ_CTRL_EMPTY, ctrl_bytes);
    return p;
}

static uint8_t tz_rehash(tz_table* t, uint32_t new_buckets, int64_t ks, int64_t vs, int32_t kk) {
    uint8_t* new_data = tz_alloc_table_buf(new_buckets, ks, vs);
    if (!new_data) return 0u;
    uint8_t* new_ctrl = new_data;
    uint8_t* new_keys = new_data + new_buckets + 16u;
    uint8_t* new_vals = new_keys + (size_t)new_buckets * (size_t)ks;
    uint32_t new_mask = new_buckets - 1u;

    if (t->data && t->buckets > 0u) {
        const uint8_t* old_ctrl = t->data;
        const uint8_t* old_keys = t->data + t->buckets + 16u;
        const uint8_t* old_vals = old_keys + (size_t)t->buckets * (size_t)ks;
        for (uint32_t old_idx = 0; old_idx < t->buckets; ++old_idx) {
            if ((old_ctrl[old_idx] & 0x80u) != 0u) continue;
            const uint8_t* kp = old_keys + (size_t)old_idx * (size_t)ks;
            uint64_t h = tz_hash_key(t, kp, kk);
            uint8_t h2 = tz_h2(h);
            uint32_t pos = (uint32_t)h & new_mask;
            uint32_t step = 0;
            uint32_t dst_idx = 0;
            for (;;) {
                uint16_t em = tz_match_byte(new_ctrl + pos, TZ_CTRL_EMPTY);
                if (em != 0u) {
                    dst_idx = (pos + tz_ctz16(em)) & new_mask;
                    break;
                }
                step += 16u;
                pos = (pos + step) & new_mask;
            }
            tz_set_ctrl(new_ctrl, new_buckets, dst_idx, h2);
            memcpy(new_keys + (size_t)dst_idx * (size_t)ks, kp, (size_t)ks);
            if (vs > 0) {
                memcpy(new_vals + (size_t)dst_idx * (size_t)vs,
                       old_vals + (size_t)old_idx * (size_t)vs, (size_t)vs);
            }
        }
        free(t->data);
    }
    t->data = new_data;
    t->buckets = new_buckets;
    t->growth_left = tz_max_load(new_buckets) - t->len;
    return 1u;
}

uint8_t tz_col_table_reserve(tz_table* t, int64_t min_cap, int64_t ks, int64_t vs, int32_t kk) {
    if (min_cap < 0) return 0u;
    if (min_cap == 0) return 1u;
    uint32_t need_buckets = tz_min_buckets_for((uint64_t)min_cap);
    if (need_buckets == 0u) return 0u;
    if (t->buckets == 0u || t->buckets < need_buckets) {
        return tz_rehash(t, need_buckets, ks, vs, kk);
    }
    if ((uint64_t)min_cap > (uint64_t)t->len &&
        (uint64_t)t->growth_left < (uint64_t)min_cap - (uint64_t)t->len) {
        return tz_rehash(t, t->buckets, ks, vs, kk);
    }
    return 1u;
}

void tz_col_table_drop(tz_table* t, int32_t kk, int64_t ks, int64_t vs, tz_drop_fn val_drop) {
    if (!t) return;
    if (t->data && t->buckets > 0u) {
        if (kk == TZ_KEY_STR || val_drop) {
            uint8_t* ctrl = t->data;
            uint8_t* keys = t->data + t->buckets + 16u;
            uint8_t* vals = keys + (size_t)t->buckets * (size_t)ks;
            for (uint32_t i = 0; i < t->buckets; ++i) {
                if ((ctrl[i] & 0x80u) != 0u) continue;
                if (kk == TZ_KEY_STR) tz_gstr_drop((tz_gstr*)(keys + (size_t)i * (size_t)ks));
                if (val_drop) val_drop(vals + (size_t)i * (size_t)vs);
            }
        }
        free(t->data);
    }
    memset(t, 0, sizeof(*t));
}

void tz_col_table_clear(tz_table* t, int32_t kk, int64_t ks, int64_t vs, tz_drop_fn val_drop) {
    if (!t || !t->data || t->buckets == 0u) return;
    if (t->len > 0u && (kk == TZ_KEY_STR || val_drop)) {
        uint8_t* ctrl = t->data;
        uint8_t* keys = t->data + t->buckets + 16u;
        uint8_t* vals = keys + (size_t)t->buckets * (size_t)ks;
        for (uint32_t i = 0; i < t->buckets; ++i) {
            if ((ctrl[i] & 0x80u) != 0u) continue;
            if (kk == TZ_KEY_STR) tz_gstr_drop((tz_gstr*)(keys + (size_t)i * (size_t)ks));
            if (val_drop) val_drop(vals + (size_t)i * (size_t)vs);
        }
    }
    memset(t->data, (int)TZ_CTRL_EMPTY, (size_t)t->buckets + 16u);
    t->len = 0u;
    t->growth_left = tz_max_load(t->buckets);
}

uint8_t tz_col_table_clone(tz_table* dst, const tz_table* src, int32_t kk, int64_t ks, int64_t vs,
                           tz_clone_fn val_clone, const void* diag) {
    *dst = *src;
    if (!src->data || src->buckets == 0u) {
        dst->data = NULL;
        return 1u;
    }
    uint32_t b = src->buckets;
    size_t ctrl_bytes = (size_t)b + 16u;
    size_t key_bytes = (size_t)b * (size_t)ks;
    size_t val_bytes = (size_t)b * (size_t)vs;
    uint8_t* new_data = (uint8_t*)malloc(ctrl_bytes + key_bytes + val_bytes);
    if (!new_data) {
        memset(dst, 0, sizeof(*dst));
        return 0u;
    }
    if (kk != TZ_KEY_STR && !val_clone) {
        memcpy(new_data, src->data, ctrl_bytes + key_bytes + val_bytes);
        dst->data = new_data;
        return 1u;
    }
    memcpy(new_data, src->data, ctrl_bytes);
    const uint8_t* src_ctrl = src->data;
    const uint8_t* src_keys = src->data + ctrl_bytes;
    const uint8_t* src_vals = src_keys + key_bytes;
    uint8_t* dst_keys = new_data + ctrl_bytes;
    uint8_t* dst_vals = dst_keys + key_bytes;
    for (uint32_t i = 0; i < b; ++i) {
        if ((src_ctrl[i] & 0x80u) != 0u) continue;
        if (kk == TZ_KEY_STR) {
            if (!tz_gstr_clone_into((tz_gstr*)(dst_keys + (size_t)i * (size_t)ks),
                                    (const tz_gstr*)(src_keys + (size_t)i * (size_t)ks))) {
                for (uint32_t j = 0; j < i; ++j) {
                    if ((src_ctrl[j] & 0x80u) != 0u) continue;
                    tz_gstr_drop((tz_gstr*)(dst_keys + (size_t)j * (size_t)ks));
                }
                free(new_data);
                memset(dst, 0, sizeof(*dst));
                return 0u;
            }
        } else {
            memcpy(dst_keys + (size_t)i * (size_t)ks, src_keys + (size_t)i * (size_t)ks, (size_t)ks);
        }
        if (val_clone) {
            val_clone(dst_vals + (size_t)i * (size_t)vs, src_vals + (size_t)i * (size_t)vs, diag);
        } else if (vs > 0) {
            memcpy(dst_vals + (size_t)i * (size_t)vs, src_vals + (size_t)i * (size_t)vs, (size_t)vs);
        }
    }
    dst->data = new_data;
    return 1u;
}

void* tz_col_map_find(const tz_table* t, const void* key_ptr, int32_t kk, int64_t ks, int64_t vs) {
    if (!t || t->len == 0u || !t->data || t->buckets == 0u) return NULL;
    const uint8_t* ctrl = t->data;
    const uint8_t* keys = t->data + t->buckets + 16u;
    const uint8_t* vals = keys + (size_t)t->buckets * (size_t)ks;
    uint32_t mask = t->buckets - 1u;
    uint64_t h = tz_hash_key(t, key_ptr, kk);
    uint8_t h2 = tz_h2(h);
    uint32_t pos = (uint32_t)h & mask;
    uint32_t step = 0;
    for (;;) {
        uint16_t m = tz_match_byte(ctrl + pos, h2);
        while (m != 0u) {
            uint32_t b = tz_ctz16(m);
            uint32_t idx = (pos + b) & mask;
            if (tz_key_eq(keys + (size_t)idx * (size_t)ks, key_ptr, kk)) {
                return vs > 0 ? (void*)(vals + (size_t)idx * (size_t)vs) : (void*)(ctrl + idx);
            }
            m &= (uint16_t)(m - 1u);
        }
        if (tz_match_byte(ctrl + pos, TZ_CTRL_EMPTY) != 0u) return NULL;
        step += 16u;
        pos = (pos + step) & mask;
    }
}

uint8_t tz_col_map_set(tz_table* t, const void* key_ptr, const void* val_ptr,
                       int32_t kk, int64_t ks, int64_t vs, tz_drop_fn val_drop) {
    if (!t->data || t->buckets == 0u) {
        if (!tz_col_table_reserve(t, 1, ks, vs, kk)) return 0u;
    }
    uint8_t* ctrl = t->data;
    uint8_t* keys = t->data + t->buckets + 16u;
    uint8_t* vals = keys + (size_t)t->buckets * (size_t)ks;
    uint32_t mask = t->buckets - 1u;
    uint64_t h = tz_hash_key(t, key_ptr, kk);
    uint8_t h2 = tz_h2(h);
    uint32_t pos = (uint32_t)h & mask;
    uint32_t step = 0;
    uint32_t first_del = UINT32_MAX;
    uint32_t empty_idx = UINT32_MAX;

    for (;;) {
        uint16_t m = tz_match_byte(ctrl + pos, h2);
        while (m != 0u) {
            uint32_t b = tz_ctz16(m);
            uint32_t idx = (pos + b) & mask;
            if (tz_key_eq(keys + (size_t)idx * (size_t)ks, key_ptr, kk)) {
                if (val_drop) val_drop(vals + (size_t)idx * (size_t)vs);
                if (vs > 0) memcpy(vals + (size_t)idx * (size_t)vs, val_ptr, (size_t)vs);
                return 1u;
            }
            m &= (uint16_t)(m - 1u);
        }
        if (first_del == UINT32_MAX) {
            uint16_t dm = tz_match_byte(ctrl + pos, TZ_CTRL_DELETED);
            if (dm != 0u) first_del = (pos + tz_ctz16(dm)) & mask;
        }
        uint16_t em = tz_match_byte(ctrl + pos, TZ_CTRL_EMPTY);
        if (em != 0u) {
            empty_idx = (pos + tz_ctz16(em)) & mask;
            break;
        }
        step += 16u;
        pos = (pos + step) & mask;
    }

    uint32_t target = first_del;
    if (target == UINT32_MAX) {
        if (t->growth_left == 0u) {
            uint32_t new_b = (t->len * 2u < t->buckets) ? t->buckets : (t->buckets * 2u);
            if (new_b > TZ_MAX_BUCKETS || !tz_rehash(t, new_b, ks, vs, kk)) return 0u;
            ctrl = t->data;
            keys = t->data + t->buckets + 16u;
            vals = keys + (size_t)t->buckets * (size_t)ks;
            mask = t->buckets - 1u;
            pos = (uint32_t)h & mask;
            step = 0;
            for (;;) {
                uint16_t em = tz_match_byte(ctrl + pos, TZ_CTRL_EMPTY);
                if (em != 0u) {
                    empty_idx = (pos + tz_ctz16(em)) & mask;
                    break;
                }
                step += 16u;
                pos = (pos + step) & mask;
            }
        }
        target = empty_idx;
    }

    if (kk == TZ_KEY_STR) {
        if (!tz_gstr_clone_into((tz_gstr*)(keys + (size_t)target * (size_t)ks), (const tz_gstr*)key_ptr))
            return 0u;
    } else {
        memcpy(keys + (size_t)target * (size_t)ks, key_ptr, (size_t)ks);
    }
    if (vs > 0) memcpy(vals + (size_t)target * (size_t)vs, val_ptr, (size_t)vs);
    tz_set_ctrl(ctrl, t->buckets, target, h2);
    t->len += 1u;
    if (first_del == UINT32_MAX) t->growth_left -= 1u;
    return 1u;
}

/* Adelanto de caché para bucles de mapa. El compilador llama aquí con la clave que usará una vuelta
   posterior del bucle; solo se calcula su hash y se piden las líneas de caché donde vivirá. No lee
   ni escribe la tabla, y una clave imposible (corte fuera de rango) se descarta sin fallar: la vuelta
   adelantada puede no llegar a ejecutarse nunca. */
static TZ_HOT_INLINE void tz_prefetch_home(const tz_table* t, uint64_t h, int64_t ks, int64_t vs) {
#if defined(__GNUC__) || defined(__clang__)
    const uint32_t pos = (uint32_t)h & (t->buckets - 1u);
    const uint8_t* ctrl = t->data;
    const uint8_t* keys = ctrl + t->buckets + 16u;
    __builtin_prefetch(ctrl + pos);
    __builtin_prefetch(keys + (size_t)pos * (size_t)ks);
    if (vs > 0) __builtin_prefetch(keys + (size_t)t->buckets * (size_t)ks + (size_t)pos * (size_t)vs, 1);
#else
    (void)t; (void)h; (void)ks; (void)vs;
#endif
}

void tz_col_map_prefetch_str(const tz_table* t, const tz_gstr* s, int64_t lo, int64_t hi, int64_t ks, int64_t vs) {
    if (!t->data || t->buckets == 0u || (t->flags & 1u)) return;
    const int64_t len = (int64_t)tz_gstr_len(s);
    if (lo < 0 || hi < lo || hi > len) return;
    tz_prefetch_home(t, tz_hash_bytes(tz_gstr_bytes(s) + lo, (size_t)(hi - lo), TZ_FOLDHASH_SEED), ks, vs);
}

void tz_col_map_prefetch_int(const tz_table* t, int64_t key, int64_t ks, int64_t vs) {
    if (!t->data || t->buckets == 0u || (t->flags & 1u)) return;
    tz_prefetch_home(t, tz_hash_key(t, &key, TZ_KEY_I64), ks, vs);
}

/* Devuelve la dirección del valor de `key`. Si la clave no existe la inserta con el valor a cero.
   Una sola búsqueda para `m[k] op= v`. NULL solo si no hay memoria. */
void* tz_col_map_slot(tz_table* t, const void* key_ptr, int32_t kk, int64_t ks, int64_t vs) {
    if (!t->data || t->buckets == 0u) {
        if (!tz_col_table_reserve(t, 1, ks, vs, kk)) return NULL;
    }
    uint8_t* ctrl = t->data;
    uint8_t* keys = t->data + t->buckets + 16u;
    uint8_t* vals = keys + (size_t)t->buckets * (size_t)ks;
    uint32_t mask = t->buckets - 1u;
    uint64_t h = tz_hash_key(t, key_ptr, kk);
    uint8_t h2 = tz_h2(h);
    uint32_t pos = (uint32_t)h & mask;
    uint32_t step = 0;
    uint32_t first_del = UINT32_MAX;
    uint32_t empty_idx = UINT32_MAX;

    for (;;) {
        uint16_t m = tz_match_byte(ctrl + pos, h2);
        while (m != 0u) {
            uint32_t idx = (pos + tz_ctz16(m)) & mask;
            if (tz_key_eq(keys + (size_t)idx * (size_t)ks, key_ptr, kk))
                return vals + (size_t)idx * (size_t)vs;
            m &= (uint16_t)(m - 1u);
        }
        if (first_del == UINT32_MAX) {
            uint16_t dm = tz_match_byte(ctrl + pos, TZ_CTRL_DELETED);
            if (dm != 0u) first_del = (pos + tz_ctz16(dm)) & mask;
        }
        uint16_t em = tz_match_byte(ctrl + pos, TZ_CTRL_EMPTY);
        if (em != 0u) {
            empty_idx = (pos + tz_ctz16(em)) & mask;
            break;
        }
        step += 16u;
        pos = (pos + step) & mask;
    }

    uint32_t target = first_del;
    if (target == UINT32_MAX) {
        if (t->growth_left == 0u) {
            uint32_t new_b = (t->len * 2u < t->buckets) ? t->buckets : (t->buckets * 2u);
            if (new_b > TZ_MAX_BUCKETS || !tz_rehash(t, new_b, ks, vs, kk)) return NULL;
            ctrl = t->data;
            keys = t->data + t->buckets + 16u;
            vals = keys + (size_t)t->buckets * (size_t)ks;
            mask = t->buckets - 1u;
            pos = (uint32_t)h & mask;
            step = 0;
            for (;;) {
                uint16_t em = tz_match_byte(ctrl + pos, TZ_CTRL_EMPTY);
                if (em != 0u) {
                    empty_idx = (pos + tz_ctz16(em)) & mask;
                    break;
                }
                step += 16u;
                pos = (pos + step) & mask;
            }
        }
        target = empty_idx;
    }

    if (kk == TZ_KEY_STR) {
        if (!tz_gstr_clone_into((tz_gstr*)(keys + (size_t)target * (size_t)ks), (const tz_gstr*)key_ptr))
            return NULL;
    } else {
        memcpy(keys + (size_t)target * (size_t)ks, key_ptr, (size_t)ks);
    }
    if (vs > 0) memset(vals + (size_t)target * (size_t)vs, 0, (size_t)vs);
    tz_set_ctrl(ctrl, t->buckets, target, h2);
    t->len += 1u;
    if (first_del == UINT32_MAX) t->growth_left -= 1u;
    return vals + (size_t)target * (size_t)vs;
}

uint8_t tz_col_map_del(tz_table* t, const void* key_ptr, int32_t kk, int64_t ks, int64_t vs, tz_drop_fn val_drop) {
    if (!t || t->len == 0u || !t->data || t->buckets == 0u) return 0u;
    uint8_t* ctrl = t->data;
    uint8_t* keys = t->data + t->buckets + 16u;
    uint8_t* vals = keys + (size_t)t->buckets * (size_t)ks;
    uint32_t mask = t->buckets - 1u;
    uint64_t h = tz_hash_key(t, key_ptr, kk);
    uint8_t h2 = tz_h2(h);
    uint32_t pos = (uint32_t)h & mask;
    uint32_t step = 0;
    for (;;) {
        uint16_t m = tz_match_byte(ctrl + pos, h2);
        while (m != 0u) {
            uint32_t b = tz_ctz16(m);
            uint32_t idx = (pos + b) & mask;
            if (tz_key_eq(keys + (size_t)idx * (size_t)ks, key_ptr, kk)) {
                if (kk == TZ_KEY_STR) tz_gstr_drop((tz_gstr*)(keys + (size_t)idx * (size_t)ks));
                else memset(keys + (size_t)idx * (size_t)ks, 0, (size_t)ks);
                if (val_drop) val_drop(vals + (size_t)idx * (size_t)vs);
                else if (vs > 0) memset(vals + (size_t)idx * (size_t)vs, 0, (size_t)vs);
                tz_set_ctrl(ctrl, t->buckets, idx, TZ_CTRL_DELETED);
                t->len -= 1u;
                return 1u;
            }
            m &= (uint16_t)(m - 1u);
        }
        if (tz_match_byte(ctrl + pos, TZ_CTRL_EMPTY) != 0u) return 0u;
        step += 16u;
        pos = (pos + step) & mask;
    }
}

uint8_t tz_col_set_add(tz_table* t, const void* key_ptr, int32_t kk, int64_t ks) {
    if (!t->data || t->buckets == 0u) {
        if (!tz_col_table_reserve(t, 1, ks, 0, kk)) return 0u;
    }
    uint8_t* ctrl = t->data;
    uint8_t* keys = t->data + t->buckets + 16u;
    uint32_t mask = t->buckets - 1u;
    uint64_t h = tz_hash_key(t, key_ptr, kk);
    uint8_t h2 = tz_h2(h);
    uint32_t pos = (uint32_t)h & mask;
    uint32_t step = 0;
    uint32_t first_del = UINT32_MAX;
    uint32_t empty_idx = UINT32_MAX;

    for (;;) {
        uint16_t m = tz_match_byte(ctrl + pos, h2);
        while (m != 0u) {
            uint32_t b = tz_ctz16(m);
            uint32_t idx = (pos + b) & mask;
            if (tz_key_eq(keys + (size_t)idx * (size_t)ks, key_ptr, kk)) return 1u;
            m &= (uint16_t)(m - 1u);
        }
        if (first_del == UINT32_MAX) {
            uint16_t dm = tz_match_byte(ctrl + pos, TZ_CTRL_DELETED);
            if (dm != 0u) first_del = (pos + tz_ctz16(dm)) & mask;
        }
        uint16_t em = tz_match_byte(ctrl + pos, TZ_CTRL_EMPTY);
        if (em != 0u) {
            empty_idx = (pos + tz_ctz16(em)) & mask;
            break;
        }
        step += 16u;
        pos = (pos + step) & mask;
    }

    uint32_t target = first_del;
    if (target == UINT32_MAX) {
        if (t->growth_left == 0u) {
            uint32_t new_b = (t->len * 2u < t->buckets) ? t->buckets : (t->buckets * 2u);
            if (new_b > TZ_MAX_BUCKETS || !tz_rehash(t, new_b, ks, 0, kk)) return 0u;
            ctrl = t->data;
            keys = t->data + t->buckets + 16u;
            mask = t->buckets - 1u;
            pos = (uint32_t)h & mask;
            step = 0;
            for (;;) {
                uint16_t em = tz_match_byte(ctrl + pos, TZ_CTRL_EMPTY);
                if (em != 0u) {
                    empty_idx = (pos + tz_ctz16(em)) & mask;
                    break;
                }
                step += 16u;
                pos = (pos + step) & mask;
            }
        }
        target = empty_idx;
    }

    if (kk == TZ_KEY_STR) {
        if (!tz_gstr_clone_into((tz_gstr*)(keys + (size_t)target * (size_t)ks), (const tz_gstr*)key_ptr))
            return 0u;
    } else {
        memcpy(keys + (size_t)target * (size_t)ks, key_ptr, (size_t)ks);
    }
    tz_set_ctrl(ctrl, t->buckets, target, h2);
    t->len += 1u;
    if (first_del == UINT32_MAX) t->growth_left -= 1u;
    return 2u;
}

static inline uint64_t tz_float_key(double d) {
    uint64_t u = 0;
    memcpy(&u, &d, 8);
    uint64_t m = (uint64_t)(-(int64_t)(u >> 63)) | UINT64_C(0x8000000000000000);
    return u ^ m;
}

static inline int tz_cmp_i64(const int64_t* a, const int64_t* b) {
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

static inline int tz_cmp_i8(const int8_t* a, const int8_t* b) {
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

static inline int tz_cmp_i16(const int16_t* a, const int16_t* b) {
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

static inline int tz_cmp_i32(const int32_t* a, const int32_t* b) {
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

static inline int tz_cmp_u8(const uint8_t* a, const uint8_t* b) {
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

static inline int tz_cmp_u16(const uint16_t* a, const uint16_t* b) {
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

static inline int tz_cmp_u32(const uint32_t* a, const uint32_t* b) {
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

static inline int tz_cmp_u64(const uint64_t* a, const uint64_t* b) {
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

static inline uint32_t tz_float32_key(float f) {
    uint32_t u = 0;
    memcpy(&u, &f, 4);
    uint32_t m = (uint32_t)(-(int32_t)(u >> 31)) | UINT32_C(0x80000000);
    return u ^ m;
}

static inline int tz_cmp_f32(const float* a, const float* b) {
    uint32_t ka = tz_float32_key(*a);
    uint32_t kb = tz_float32_key(*b);
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

static inline int tz_cmp_f64(const double* a, const double* b) {
    uint64_t ka = tz_float_key(*a);
    uint64_t kb = tz_float_key(*b);
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

static inline int tz_cmp_str_ptr(const tz_gstr* a, const tz_gstr* b) {
    return tz_gstr_cmp(a, b);
}

static inline size_t tz_floor_log2(size_t n) {
    size_t k = 0;
    while (n > 1u) { n >>= 1; ++k; }
    return k;
}

#define TZ_DEFINE_SORTS(SFX, T, CMP_PTR)                                                         \
static void tz_ins_##SFX(T* arr, size_t len) {                                                   \
    for (size_t i = 1; i < len; ++i) {                                                           \
        T cur = arr[i];                                                                          \
        size_t j = i;                                                                            \
        while (j > 0u && CMP_PTR(&cur, &arr[j - 1u]) < 0) {                                      \
            arr[j] = arr[j - 1u];                                                                \
            --j;                                                                                 \
        }                                                                                        \
        arr[j] = cur;                                                                            \
    }                                                                                            \
}                                                                                                \
static void tz_sift_##SFX(T* arr, size_t root, size_t end) {                                     \
    for (;;) {                                                                                   \
        size_t child = root * 2u + 1u;                                                           \
        if (child >= end) break;                                                                 \
        if (child + 1u < end && CMP_PTR(&arr[child], &arr[child + 1u]) < 0) ++child;             \
        if (CMP_PTR(&arr[root], &arr[child]) >= 0) break;                                        \
        T tmp = arr[root];                                                                       \
        arr[root] = arr[child];                                                                  \
        arr[child] = tmp;                                                                        \
        root = child;                                                                            \
    }                                                                                            \
}                                                                                                \
static void tz_heap_##SFX(T* arr, size_t len) {                                                  \
    for (size_t i = len / 2u; i > 0u; --i) tz_sift_##SFX(arr, i - 1u, len);                      \
    for (size_t end = len - 1u; end > 0u; --end) {                                               \
        T tmp = arr[0];                                                                          \
        arr[0] = arr[end];                                                                       \
        arr[end] = tmp;                                                                          \
        tz_sift_##SFX(arr, 0u, end);                                                             \
    }                                                                                            \
}                                                                                                \
static inline void tz_swap2_##SFX(T* a, T* b) { T t = *a; *a = *b; *b = t; }                    \
static size_t tz_med3_##SFX(T* arr, size_t a, size_t b, size_t c) {                              \
    if (CMP_PTR(&arr[a], &arr[b]) > 0) tz_swap2_##SFX(&arr[a], &arr[b]);                         \
    if (CMP_PTR(&arr[b], &arr[c]) > 0) tz_swap2_##SFX(&arr[b], &arr[c]);                         \
    if (CMP_PTR(&arr[a], &arr[b]) > 0) tz_swap2_##SFX(&arr[a], &arr[b]);                         \
    return b;                                                                                    \
}                                                                                                \
static size_t tz_pivot_##SFX(T* arr, size_t len) {                                               \
    size_t mid = len >> 1;                                                                       \
    if (len > 128u) {                                                                            \
        size_t step = len >> 3;                                                                  \
        size_t s1 = tz_med3_##SFX(arr, 0u, step, step * 2u);                                     \
        size_t s2 = tz_med3_##SFX(arr, mid - step, mid, mid + step);                             \
        size_t s3 = tz_med3_##SFX(arr, len - 1u - step * 2u, len - 1u - step, len - 1u);         \
        return tz_med3_##SFX(arr, s1, s2, s3);                                                   \
    }                                                                                            \
    return tz_med3_##SFX(arr, 0u, mid, len - 1u);                                                \
}                                                                                                \
static size_t tz_part_block_##SFX(T* arr, size_t len, size_t piv_idx) {                          \
    tz_swap2_##SFX(&arr[0], &arr[piv_idx]);                                                      \
    T piv = arr[0];                                                                              \
    size_t l = 1u;                                                                               \
    size_t r = len - 1u;                                                                         \
    while (l <= r && CMP_PTR(&arr[l], &piv) < 0) ++l;                                            \
    while (r >= l && CMP_PTR(&arr[r], &piv) >= 0) --r;                                           \
    if (l < r) {                                                                                 \
        tz_swap2_##SFX(&arr[l], &arr[r]);                                                        \
        ++l; --r;                                                                                \
    }                                                                                            \
    uint8_t off_l[32], off_r[32];                                                                \
    size_t num_l = 0u, num_r = 0u, idx_l = 0u, idx_r = 0u;                                       \
    for (;;) {                                                                                   \
        size_t span = (r >= l) ? (r - l + 1u) : 0u;                                              \
        if (span < 64u) break;                                                                   \
        if (num_l == 0u) {                                                                       \
            idx_l = 0u;                                                                          \
            for (size_t k = 0u; k < 32u; ++k) {                                                  \
                off_l[num_l] = (uint8_t)k;                                                       \
                num_l += (size_t)(CMP_PTR(&arr[l + k], &piv) >= 0);                              \
            }                                                                                    \
        }                                                                                        \
        if (num_r == 0u) {                                                                       \
            idx_r = 0u;                                                                          \
            for (size_t k = 0u; k < 32u; ++k) {                                                  \
                off_r[num_r] = (uint8_t)k;                                                       \
                num_r += (size_t)(CMP_PTR(&arr[r - k], &piv) < 0);                               \
            }                                                                                    \
        }                                                                                        \
        size_t cnt = num_l < num_r ? num_l : num_r;                                              \
        for (size_t k = 0u; k < cnt; ++k) {                                                      \
            tz_swap2_##SFX(&arr[l + off_l[idx_l + k]], &arr[r - off_r[idx_r + k]]);              \
        }                                                                                        \
        num_l -= cnt; num_r -= cnt; idx_l += cnt; idx_r += cnt;                                  \
        if (num_l == 0u) l += 32u;                                                               \
        if (num_r == 0u) r -= 32u;                                                               \
    }                                                                                            \
    while (l <= r) {                                                                             \
        while (l <= r && CMP_PTR(&arr[l], &piv) < 0) ++l;                                        \
        while (r >= l && CMP_PTR(&arr[r], &piv) >= 0) --r;                                       \
        if (l < r) {                                                                             \
            tz_swap2_##SFX(&arr[l], &arr[r]);                                                    \
            ++l; --r;                                                                            \
        }                                                                                        \
    }                                                                                            \
    size_t mid = l - 1u;                                                                         \
    arr[0] = arr[mid];                                                                           \
    arr[mid] = piv;                                                                              \
    return mid;                                                                                  \
}                                                                                                \
static size_t tz_part_eq_##SFX(T* arr, size_t len, const T* piv) {                               \
    size_t eq = 0u;                                                                              \
    for (size_t i = 0u; i < len; ++i) {                                                          \
        if (CMP_PTR(&arr[i], piv) == 0) {                                                        \
            tz_swap2_##SFX(&arr[eq], &arr[i]);                                                   \
            ++eq;                                                                                \
        }                                                                                        \
    }                                                                                            \
    return eq;                                                                                   \
}                                                                                                \
static void tz_ipn_##SFX(T* arr, size_t len, const T* pred_piv, size_t limit) {                  \
    while (len > 20u) {                                                                          \
        if (limit == 0u) { tz_heap_##SFX(arr, len); return; }                                    \
        size_t piv_idx = tz_pivot_##SFX(arr, len);                                               \
        if (pred_piv && CMP_PTR(&arr[piv_idx], pred_piv) == 0) {                                 \
            size_t eq = tz_part_eq_##SFX(arr, len, pred_piv);                                    \
            arr += eq; len -= eq; pred_piv = NULL;                                               \
            continue;                                                                            \
        }                                                                                        \
        size_t mid = tz_part_block_##SFX(arr, len, piv_idx);                                     \
        size_t left_len = mid;                                                                   \
        size_t right_len = len - mid - 1u;                                                       \
        size_t bad = (left_len < (len >> 3)) || (right_len < (len >> 3));                        \
        if (bad) --limit;                                                                        \
        if (left_len <= right_len) {                                                             \
            tz_ipn_##SFX(arr, left_len, pred_piv, limit);                                        \
            pred_piv = &arr[mid];                                                                \
            arr += mid + 1u;                                                                     \
            len = right_len;                                                                     \
        } else {                                                                                 \
            tz_ipn_##SFX(arr + mid + 1u, right_len, &arr[mid], limit);                           \
            len = left_len;                                                                      \
        }                                                                                        \
    }                                                                                            \
    if (len > 1u) tz_ins_##SFX(arr, len);                                                        \
}                                                                                                \
void tz_col_sort_##SFX(T* arr, int64_t n) {                                                      \
    if (!arr || n <= 1) return;                                                                  \
    tz_ipn_##SFX(arr, (size_t)n, NULL, tz_floor_log2((size_t)n) + 1u);                           \
}                                                                                                \
static void tz_merge_##SFX(T* arr, size_t l_len, size_t r_len, T* scratch) {                     \
    if (l_len == 0u || r_len == 0u) return;                                                      \
    memcpy(scratch, arr, l_len * sizeof(T));                                                     \
    size_t i = 0u, j = l_len, k = 0u, end = l_len + r_len;                                       \
    while (i < l_len && j < end) {                                                               \
        if (CMP_PTR(&scratch[i], &arr[j]) <= 0) arr[k++] = scratch[i++];                         \
        else arr[k++] = arr[j++];                                                                \
    }                                                                                            \
    if (i < l_len) memcpy(arr + k, scratch + i, (l_len - i) * sizeof(T));                        \
}                                                                                                \
static void tz_msort_##SFX(T* arr, size_t len, T* scratch) {                                     \
    if (len <= 20u) { if (len > 1u) tz_ins_##SFX(arr, len); return; }                            \
    size_t mid = len >> 1;                                                                       \
    tz_msort_##SFX(arr, mid, scratch);                                                           \
    tz_msort_##SFX(arr + mid, len - mid, scratch);                                               \
    if (CMP_PTR(&arr[mid - 1u], &arr[mid]) <= 0) return;                                         \
    tz_merge_##SFX(arr, mid, len - mid, scratch);                                                \
}                                                                                                \
static size_t tz_med3_ro_##SFX(const T* arr, size_t a, size_t b, size_t c) {                     \
    int ab = CMP_PTR(&arr[a], &arr[b]) <= 0;                                                     \
    int bc = CMP_PTR(&arr[b], &arr[c]) <= 0;                                                     \
    int ac = CMP_PTR(&arr[a], &arr[c]) <= 0;                                                     \
    if (ab) { if (bc) return b; return ac ? c : a; }                                             \
    if (!bc) return b;                                                                           \
    return ac ? a : c;                                                                           \
}                                                                                                \
static void tz_drift_##SFX(T* arr, size_t len, T* scratch, size_t limit) {                       \
    while (len > 20u) {                                                                          \
        size_t r = 1u;                                                                           \
        if (CMP_PTR(&arr[0], &arr[1]) <= 0) {                                                    \
            while (r < len && CMP_PTR(&arr[r - 1u], &arr[r]) <= 0) ++r;                          \
        } else {                                                                                 \
            while (r < len && CMP_PTR(&arr[r - 1u], &arr[r]) > 0) ++r;                           \
            for (size_t a = 0u, b = r - 1u; a < b; ++a, --b) tz_swap2_##SFX(&arr[a], &arr[b]);   \
        }                                                                                        \
        if (r == len) return;                                                                    \
        if (r * 4u >= len * 3u) {                                                                \
            tz_drift_##SFX(arr + r, len - r, scratch, limit);                                    \
            tz_merge_##SFX(arr, r, len - r, scratch);                                            \
            return;                                                                              \
        }                                                                                        \
        if (limit == 0u) { tz_msort_##SFX(arr, len, scratch); return; }                          \
        --limit;                                                                                 \
        size_t p_idx = tz_med3_ro_##SFX(arr, 0u, len >> 1, len - 1u);                            \
        T piv = arr[p_idx];                                                                      \
        size_t lt = 0u, ge = 0u;                                                                 \
        for (size_t i = 0u; i < len; ++i) {                                                      \
            if (CMP_PTR(&arr[i], &piv) < 0) arr[lt++] = arr[i];                                  \
            else scratch[ge++] = arr[i];                                                         \
        }                                                                                        \
        if (ge > 0u) memcpy(arr + lt, scratch, ge * sizeof(T));                                  \
        if (lt == 0u) {                                                                          \
            size_t eq = 0u, gt = 0u;                                                             \
            for (size_t i = 0u; i < len; ++i) {                                                  \
                if (CMP_PTR(&arr[i], &piv) <= 0) arr[eq++] = arr[i];                             \
                else scratch[gt++] = arr[i];                                                     \
            }                                                                                    \
            if (gt > 0u) memcpy(arr + eq, scratch, gt * sizeof(T));                              \
            arr += eq;                                                                           \
            len = gt;                                                                            \
            continue;                                                                            \
        }                                                                                        \
        if (lt <= ge) {                                                                          \
            tz_drift_##SFX(arr, lt, scratch, limit);                                             \
            arr += lt;                                                                           \
            len = ge;                                                                            \
        } else {                                                                                 \
            tz_drift_##SFX(arr + lt, ge, scratch, limit);                                        \
            len = lt;                                                                            \
        }                                                                                        \
    }                                                                                            \
    if (len > 1u) tz_ins_##SFX(arr, len);                                                        \
}                                                                                                \
uint8_t tz_col_sort_stable_##SFX(T* arr, int64_t n) {                                            \
    if (!arr || n <= 1) return 1u;                                                               \
    size_t len = (size_t)n;                                                                      \
    if (len <= 20u) { tz_ins_##SFX(arr, len); return 1u; }                                       \
    T* scratch = (T*)malloc(len * sizeof(T));                                                    \
    if (!scratch) return 0u;                                                                     \
    tz_drift_##SFX(arr, len, scratch, tz_floor_log2(len) * 2u + 2u);                             \
    free(scratch);                                                                               \
    return 1u;                                                                                   \
}

TZ_DEFINE_SORTS(i64, int64_t, tz_cmp_i64)
TZ_DEFINE_SORTS(i8, int8_t, tz_cmp_i8)
TZ_DEFINE_SORTS(i16, int16_t, tz_cmp_i16)
TZ_DEFINE_SORTS(i32, int32_t, tz_cmp_i32)
TZ_DEFINE_SORTS(u8, uint8_t, tz_cmp_u8)
TZ_DEFINE_SORTS(u16, uint16_t, tz_cmp_u16)
TZ_DEFINE_SORTS(u32, uint32_t, tz_cmp_u32)
TZ_DEFINE_SORTS(u64, uint64_t, tz_cmp_u64)
TZ_DEFINE_SORTS(f32, float, tz_cmp_f32)
TZ_DEFINE_SORTS(f64, double, tz_cmp_f64)
TZ_DEFINE_SORTS(str, tz_gstr, tz_cmp_str_ptr)

void tz_col_sort_bool(uint8_t* arr, int64_t n) {
    if (!arr || n <= 1) return;
    size_t zeros = 0u;
    size_t len = (size_t)n;
    for (size_t i = 0u; i < len; ++i) zeros += (size_t)(arr[i] == 0u);
    if (zeros > 0u) memset(arr, 0, zeros);
    if (zeros < len) memset(arr + zeros, 1, len - zeros);
}

uint8_t tz_col_sort_stable_bool(uint8_t* arr, int64_t n) {
    tz_col_sort_bool(arr, n);
    return 1u;
}

typedef struct tz_pool {
    uint8_t* data;
    uint32_t len;
    uint32_t cap;
    uint32_t used;
    uint32_t free_head_plus1;
} tz_pool;

typedef struct tz_handle {
    uint32_t idx;
    uint32_t gen;
} tz_handle;

static size_t tz_pool_vals_off(size_t cap) {
    return (cap * 8u + 15u) & ~(size_t)15u;
}

uint8_t tz_col_pool_reserve(tz_pool* p, int64_t min_cap, int64_t elem_size) {
    if (!p || min_cap < 0 || min_cap > 14680064LL) return 0u;
    uint64_t need = (uint64_t)min_cap;
    if (need == 0u || need <= (uint64_t)p->cap) return 1u;
    uint64_t new_cap = p->cap == 0u ? 8ULL : (uint64_t)p->cap;
    while (new_cap < need) new_cap <<= 1;
    if (new_cap > 14680064ULL) new_cap = 14680064ULL;
    size_t old_off = tz_pool_vals_off((size_t)p->cap);
    size_t new_off = tz_pool_vals_off((size_t)new_cap);
    size_t es = elem_size > 0 ? (size_t)elem_size : 0u;
    size_t total = new_off + (size_t)new_cap * es;
    uint8_t* buf = (uint8_t*)malloc(total > 0u ? total : 1u);
    if (!buf) return 0u;
    if (p->used > 0u && p->data) {
        memcpy(buf, p->data, (size_t)p->used * 8u);
        if (es > 0u) memcpy(buf + new_off, p->data + old_off, (size_t)p->used * es);
    }
    free(p->data);
    p->data = buf;
    p->cap = (uint32_t)new_cap;
    return 1u;
}

uint8_t tz_col_pool_add(tz_pool* p, const void* val, int64_t elem_size, tz_handle* out_h) {
    if (!p || !out_h) return 0u;
    size_t es = elem_size > 0 ? (size_t)elem_size : 0u;
    if (p->free_head_plus1 != 0u) {
        uint32_t idx = p->free_head_plus1 - 1u;
        uint64_t* meta = (uint64_t*)p->data;
        uint64_t m = meta[idx];
        uint32_t gen = (uint32_t)m;
        uint32_t state = (uint32_t)(m >> 32);
        p->free_head_plus1 = (state >= 2u) ? (state - 1u) : 0u;
        meta[idx] = (uint64_t)gen;
        if (es > 0u && val) {
            memcpy(p->data + tz_pool_vals_off((size_t)p->cap) + (size_t)idx * es, val, es);
        }
        ++p->len;
        out_h->idx = idx;
        out_h->gen = gen;
        return 1u;
    }
    if (p->used >= 14680064u) return 0u;
    if (p->used >= p->cap && !tz_col_pool_reserve(p, (int64_t)p->used + 1, elem_size)) return 0u;
    uint32_t idx = p->used++;
    uint64_t* meta = (uint64_t*)p->data;
    meta[idx] = 1ULL;
    if (es > 0u && val) {
        memcpy(p->data + tz_pool_vals_off((size_t)p->cap) + (size_t)idx * es, val, es);
    }
    ++p->len;
    out_h->idx = idx;
    out_h->gen = 1u;
    return 1u;
}

void* tz_col_pool_get_ptr(const tz_pool* p, uint32_t idx, uint32_t gen, int64_t elem_size) {
    if (!p || !p->data || gen == 0u || idx >= p->used) return NULL;
    const uint64_t* meta = (const uint64_t*)p->data;
    if (meta[idx] != (uint64_t)gen) return NULL;
    if (elem_size <= 0) return p->data;
    return p->data + tz_pool_vals_off((size_t)p->cap) + (size_t)idx * (size_t)elem_size;
}

uint8_t tz_col_pool_remove(tz_pool* p, uint32_t idx, uint32_t gen, void* out_val, int64_t elem_size) {
    if (!p || !p->data || gen == 0u || idx >= p->used) return 0u;
    uint64_t* meta = (uint64_t*)p->data;
    if (meta[idx] != (uint64_t)gen) return 0u;
    size_t es = elem_size > 0 ? (size_t)elem_size : 0u;
    if (es > 0u && out_val) {
        memcpy(out_val, p->data + tz_pool_vals_off((size_t)p->cap) + (size_t)idx * es, es);
    }
    uint32_t ng = gen + 1u;
    if (ng != 0u) {
        meta[idx] = ((uint64_t)(p->free_head_plus1 + 1u) << 32) | (uint64_t)ng;
        p->free_head_plus1 = idx + 1u;
    } else {
        meta[idx] = (1ULL << 32);
    }
    --p->len;
    return 1u;
}

void tz_col_pool_clear(tz_pool* p, int64_t elem_size, tz_drop_fn drop_fn) {
    if (!p || !p->data || p->len == 0u) return;
    uint64_t* meta = (uint64_t*)p->data;
    uint8_t* vals = p->data + tz_pool_vals_off((size_t)p->cap);
    size_t es = elem_size > 0 ? (size_t)elem_size : 0u;
    for (uint32_t i = p->used; i > 0u; --i) {
        uint32_t idx = i - 1u;
        uint64_t m = meta[idx];
        if ((m >> 32) != 0ULL) continue;
        if (drop_fn) drop_fn(vals + (size_t)idx * es);
        uint32_t ng = (uint32_t)m + 1u;
        if (ng != 0u) {
            meta[idx] = ((uint64_t)(p->free_head_plus1 + 1u) << 32) | (uint64_t)ng;
            p->free_head_plus1 = idx + 1u;
        } else {
            meta[idx] = (1ULL << 32);
        }
    }
    p->len = 0u;
}

void tz_col_pool_drop(tz_pool* p, int64_t elem_size, tz_drop_fn drop_fn) {
    if (!p) return;
    if (p->data) {
        if (drop_fn && p->len > 0u) {
            const uint64_t* meta = (const uint64_t*)p->data;
            uint8_t* vals = p->data + tz_pool_vals_off((size_t)p->cap);
            size_t es = elem_size > 0 ? (size_t)elem_size : 0u;
            for (uint32_t idx = 0u; idx < p->used; ++idx) {
                if ((meta[idx] >> 32) == 0ULL) drop_fn(vals + (size_t)idx * es);
            }
        }
        free(p->data);
    }
    memset(p, 0, sizeof(*p));
}

uint8_t tz_col_pool_clone(tz_pool* dst, const tz_pool* src, int64_t elem_size, tz_clone_fn clone_fn, const void* diag) {
    if (!dst) return 0u;
    memset(dst, 0, sizeof(*dst));
    if (!src || src->cap == 0u || !src->data) return 1u;
    size_t off = tz_pool_vals_off((size_t)src->cap);
    size_t es = elem_size > 0 ? (size_t)elem_size : 0u;
    size_t total = off + (size_t)src->cap * es;
    uint8_t* buf = (uint8_t*)malloc(total > 0u ? total : 1u);
    if (!buf) return 0u;
    if (src->used > 0u) memcpy(buf, src->data, (size_t)src->used * 8u);
    if (src->used > 0u && es > 0u) {
        const uint64_t* meta = (const uint64_t*)src->data;
        const uint8_t* svals = src->data + off;
        uint8_t* dvals = buf + off;
        if (!clone_fn) {
            memcpy(dvals, svals, (size_t)src->used * es);
        } else {
            for (uint32_t idx = 0u; idx < src->used; ++idx) {
                if ((meta[idx] >> 32) == 0ULL) clone_fn(dvals + (size_t)idx * es, svals + (size_t)idx * es, diag);
                else memset(dvals + (size_t)idx * es, 0, es);
            }
        }
    }
    dst->data = buf;
    dst->len = src->len;
    dst->cap = src->cap;
    dst->used = src->used;
    dst->free_head_plus1 = src->free_head_plus1;
    return 1u;
}

uint32_t tz_col_pool_handles(const tz_pool* p, tz_handle* out_buf) {
    if (!p || !p->data || !out_buf || p->len == 0u) return 0u;
    const uint64_t* meta = (const uint64_t*)p->data;
    uint32_t written = 0u;
    for (uint32_t idx = 0u; idx < p->used; ++idx) {
        uint64_t m = meta[idx];
        if ((m >> 32) == 0ULL) {
            out_buf[written].idx = idx;
            out_buf[written].gen = (uint32_t)m;
            ++written;
        }
    }
    return written;
}
)c";
}

} // namespace veloz::native
