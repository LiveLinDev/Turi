#pragma once
#include <string>

namespace veloz::native {

namespace crypto_rt_parts {

inline constexpr const char* kCryptoHeaderAndSha = R"c(
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#endif

static inline void tz_c_zero(void* p, size_t n) {
    volatile uint8_t* vp = (volatile uint8_t*)p;
    for (size_t i = 0; i < n; ++i) vp[i] = 0u;
}

static inline uint32_t tz_c_u32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void tz_c_put32be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)(v);
}

static inline uint32_t tz_c_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void tz_c_put32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t tz_c_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t tz_c_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void tz_sha256_transform(uint32_t st[8], const uint8_t blk[64]) {
    static const uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = tz_c_u32be(blk + i * 4);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = tz_c_rotr(w[i - 15], 7) ^ tz_c_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = tz_c_rotr(w[i - 2], 17) ^ tz_c_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = tz_c_rotr(e, 6) ^ tz_c_rotr(e, 11) ^ tz_c_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = tz_c_rotr(a, 2) ^ tz_c_rotr(a, 13) ^ tz_c_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    tz_c_zero(w, sizeof(w));
}

void tz_crypto_sha256(const uint8_t* data, int64_t len, uint8_t* out32) {
    if (!out32) return;
    uint32_t st[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    uint64_t n = (len > 0 && data) ? (uint64_t)len : 0ULL;
    size_t off = 0u;
    while (n - off >= 64ULL) {
        tz_sha256_transform(st, data + off);
        off += 64u;
    }
    uint8_t tail[64];
    memset(tail, 0, sizeof(tail));
    size_t rem = (size_t)(n - off);
    if (rem > 0u && data) memcpy(tail, data + off, rem);
    tail[rem++] = 0x80u;
    if (rem > 56u) {
        tz_sha256_transform(st, tail);
        memset(tail, 0, sizeof(tail));
    }
    uint64_t bits = n * 8ULL;
    for (int i = 0; i < 8; ++i) tail[56 + i] = (uint8_t)(bits >> ((7 - i) * 8));
    tz_sha256_transform(st, tail);
    for (int i = 0; i < 8; ++i) tz_c_put32be(out32 + i * 4, st[i]);
    tz_c_zero(tail, sizeof(tail));
    tz_c_zero(st, sizeof(st));
}

void tz_crypto_hmac_sha256(const uint8_t* key, int64_t klen,
                           const uint8_t* msg, int64_t mlen, uint8_t* out32) {
    if (!out32) return;
    uint8_t kpad[64], ipad[64], opad[64], inner[32];
    memset(kpad, 0, sizeof(kpad));
    if (key && klen > 64) tz_crypto_sha256(key, klen, kpad);
    else if (key && klen > 0) memcpy(kpad, key, (size_t)klen);
    for (int i = 0; i < 64; ++i) {
        ipad[i] = (uint8_t)(kpad[i] ^ 0x36u);
        opad[i] = (uint8_t)(kpad[i] ^ 0x5cu);
    }
    uint64_t mn = (mlen > 0 && msg) ? (uint64_t)mlen : 0ULL;
    uint32_t st[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    tz_sha256_transform(st, ipad);
    size_t off = 0u;
    while (mn - off >= 64ULL) {
        tz_sha256_transform(st, msg + off);
        off += 64u;
    }
    uint8_t tail[64];
    memset(tail, 0, sizeof(tail));
    size_t rem = (size_t)(mn - off);
    if (rem > 0u && msg) memcpy(tail, msg + off, rem);
    tail[rem++] = 0x80u;
    if (rem > 56u) { tz_sha256_transform(st, tail); memset(tail, 0, sizeof(tail)); }
    uint64_t bits = (64ULL + mn) * 8ULL;
    for (int i = 0; i < 8; ++i) tail[56 + i] = (uint8_t)(bits >> ((7 - i) * 8));
    tz_sha256_transform(st, tail);
    for (int i = 0; i < 8; ++i) tz_c_put32be(inner + i * 4, st[i]);

    uint8_t obuf[96];
    memcpy(obuf, opad, 64u);
    memcpy(obuf + 64u, inner, 32u);
    tz_crypto_sha256(obuf, 96, out32);

    tz_c_zero(kpad, sizeof(kpad));
    tz_c_zero(ipad, sizeof(ipad));
    tz_c_zero(opad, sizeof(opad));
    tz_c_zero(inner, sizeof(inner));
    tz_c_zero(tail, sizeof(tail));
    tz_c_zero(obuf, sizeof(obuf));
    tz_c_zero(st, sizeof(st));
}
)c";

inline constexpr const char* kCryptoCtRngAead = R"c(
int32_t tz_crypto_ct_eq(const uint8_t* a, const uint8_t* b, int64_t len) {
    if (len < 0) return 0;
    if (len == 0) return 1;
    if (!a || !b) return 0;
    volatile const uint8_t* va = a;
    volatile const uint8_t* vb = b;
    uint8_t diff = 0u;
    for (int64_t i = 0; i < len; ++i) diff |= (uint8_t)(va[i] ^ vb[i]);
    return diff == 0u ? 1 : 0;
}

int32_t tz_crypto_random_bytes(uint8_t* out, int64_t len) {
    if (len < 0) return 0;
    if (len == 0) return 1;
    if (!out) return 0;
    int64_t off = 0;
#ifdef _WIN32
    while (off < len) {
        int64_t rem = len - off;
        ULONG chunk = rem > 1048576 ? 1048576u : (ULONG)rem;
        if (BCryptGenRandom(NULL, out + off, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return 0;
        off += (int64_t)chunk;
    }
    return 1;
#else
#ifdef __linux__
    while (off < len) {
        ssize_t r = getrandom(out + off, (size_t)(len - off), 0);
        if (r > 0) { off += (int64_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == ENOSYS) break;
        return 0;
    }
    if (off == len) return 1;
#endif
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    while (off < len) {
        ssize_t r = read(fd, out + off, (size_t)(len - off));
        if (r > 0) { off += (int64_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
#endif
}

#define TZ_QR(a, b, c, d) do { \
    a += b; d = tz_c_rotl(d ^ a, 16); \
    c += d; b = tz_c_rotl(b ^ c, 12); \
    a += b; d = tz_c_rotl(d ^ a, 8); \
    c += d; b = tz_c_rotl(b ^ c, 7); \
} while (0)

static void tz_chacha20_block(const uint8_t key32[32], uint32_t counter, const uint8_t nonce12[12], uint8_t out64[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu; s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; ++i) s[4 + i] = tz_c_u32le(key32 + i * 4);
    s[12] = counter;
    s[13] = tz_c_u32le(nonce12 + 0);
    s[14] = tz_c_u32le(nonce12 + 4);
    s[15] = tz_c_u32le(nonce12 + 8);
    memcpy(x, s, sizeof(x));
    for (int r = 0; r < 10; ++r) {
        TZ_QR(x[0], x[4], x[8],  x[12]);
        TZ_QR(x[1], x[5], x[9],  x[13]);
        TZ_QR(x[2], x[6], x[10], x[14]);
        TZ_QR(x[3], x[7], x[11], x[15]);
        TZ_QR(x[0], x[5], x[10], x[15]);
        TZ_QR(x[1], x[6], x[11], x[12]);
        TZ_QR(x[2], x[7], x[8],  x[13]);
        TZ_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; ++i) tz_c_put32le(out64 + i * 4, x[i] + s[i]);
    tz_c_zero(x, sizeof(x));
    tz_c_zero(s, sizeof(s));
}
#undef TZ_QR

static void tz_chacha20_xor(const uint8_t key32[32], uint32_t counter, const uint8_t nonce12[12],
                            const uint8_t* in, uint8_t* out, int64_t len) {
    uint8_t blk[64];
    int64_t off = 0;
    while (off < len) {
        tz_chacha20_block(key32, counter++, nonce12, blk);
        int64_t take = (len - off) > 64 ? 64 : (len - off);
        for (int64_t i = 0; i < take; ++i) out[off + i] = (uint8_t)(in[off + i] ^ blk[i]);
        off += take;
    }
    tz_c_zero(blk, sizeof(blk));
}

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint8_t pad[16];
} tz_poly_st;

static void tz_poly_init(tz_poly_st* st, const uint8_t otk32[32]) {
    uint8_t rk[16];
    memcpy(rk, otk32, 16u);
    rk[3] &= 0x0Fu; rk[7] &= 0x0Fu; rk[11] &= 0x0Fu; rk[15] &= 0x0Fu;
    rk[4] &= 0xFCu; rk[8] &= 0xFCu; rk[12] &= 0xFCu;
    st->r[0] = (tz_c_u32le(rk + 0)) & 0x3ffffffu;
    st->r[1] = (tz_c_u32le(rk + 3) >> 2) & 0x3ffffffu;
    st->r[2] = (tz_c_u32le(rk + 6) >> 4) & 0x3ffffffu;
    st->r[3] = (tz_c_u32le(rk + 9) >> 6) & 0x3ffffffu;
    st->r[4] = (tz_c_u32le(rk + 12) >> 8) & 0x0ffffffu;
    memset(st->h, 0, sizeof(st->h));
    memcpy(st->pad, otk32 + 16, 16u);
    tz_c_zero(rk, sizeof(rk));
}

static void tz_poly_blocks(tz_poly_st* st, const uint8_t* m, size_t len) {
    uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    uint32_t s1 = r1 * 5u, s2 = r2 * 5u, s3 = r3 * 5u, s4 = r4 * 5u;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    size_t off = 0u;
    while (off < len) {
        size_t chunk = (len - off) >= 16u ? 16u : (len - off);
        uint8_t blk[17];
        memset(blk, 0, sizeof(blk));
        memcpy(blk, m + off, chunk);
        blk[chunk] = 1u;
        h0 += (tz_c_u32le(blk + 0)) & 0x3ffffffu;
        h1 += (tz_c_u32le(blk + 3) >> 2) & 0x3ffffffu;
        h2 += (tz_c_u32le(blk + 6) >> 4) & 0x3ffffffu;
        h3 += (tz_c_u32le(blk + 9) >> 6) & 0x3ffffffu;
        h4 += (tz_c_u32le(blk + 12) >> 8) | ((uint32_t)blk[16] << 24);
        uint64_t d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        uint64_t d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        uint64_t d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        uint64_t d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        uint64_t d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;
        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffffu; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffffu; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffffu; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffffu; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffffu; h0 += c * 5u;
        c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;
        off += chunk;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void tz_poly_finish(tz_poly_st* st, uint8_t tag16[16]) {
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    uint32_t c = h1 >> 26; h1 &= 0x3ffffffu;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffffu;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffffu;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffffu;
    h0 += c * 5u; c = h0 >> 26; h0 &= 0x3ffffffu;
    h1 += c;

    uint32_t g0 = h0 + 5u; c = g0 >> 26; g0 &= 0x3ffffffu;
    uint32_t g1 = h1 + c;  c = g1 >> 26; g1 &= 0x3ffffffu;
    uint32_t g2 = h2 + c;  c = g2 >> 26; g2 &= 0x3ffffffu;
    uint32_t g3 = h3 + c;  c = g3 >> 26; g3 &= 0x3ffffffu;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1u;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    uint32_t w0 = ((h0) | (h1 << 26));
    uint32_t w1 = ((h1 >> 6) | (h2 << 20));
    uint32_t w2 = ((h2 >> 12) | (h3 << 14));
    uint32_t w3 = ((h3 >> 18) | (h4 << 8));

    uint64_t f;
    f = (uint64_t)w0 + tz_c_u32le(st->pad + 0); w0 = (uint32_t)f;
    f = (uint64_t)w1 + tz_c_u32le(st->pad + 4) + (f >> 32); w1 = (uint32_t)f;
    f = (uint64_t)w2 + tz_c_u32le(st->pad + 8) + (f >> 32); w2 = (uint32_t)f;
    f = (uint64_t)w3 + tz_c_u32le(st->pad + 12) + (f >> 32); w3 = (uint32_t)f;

    tz_c_put32le(tag16 + 0, w0); tz_c_put32le(tag16 + 4, w1);
    tz_c_put32le(tag16 + 8, w2); tz_c_put32le(tag16 + 12, w3);
    tz_c_zero(st, sizeof(*st));
}

static void tz_aead_poly_tag(const uint8_t key32[32], const uint8_t nonce12[12],
                             const uint8_t* aad, int64_t aad_len,
                             const uint8_t* ct, int64_t ct_len, uint8_t tag16[16]) {
    uint8_t otk[64];
    tz_chacha20_block(key32, 0u, nonce12, otk);
    tz_poly_st st;
    tz_poly_init(&st, otk);
    uint8_t zeros[16];
    memset(zeros, 0, sizeof(zeros));
    uint64_t al = aad_len > 0 ? (uint64_t)aad_len : 0ULL;
    uint64_t cl = ct_len > 0 ? (uint64_t)ct_len : 0ULL;
    if (al > 0ULL && aad) {
        size_t full = (size_t)(al & ~15ULL);
        size_t rem = (size_t)(al & 15ULL);
        if (full > 0u) tz_poly_blocks(&st, aad, full);
        if (rem > 0u) {
            uint8_t blk[16];
            memset(blk, 0, sizeof(blk));
            memcpy(blk, aad + full, rem);
            tz_poly_blocks(&st, blk, 16u);
        }
    }
    if (cl > 0ULL && ct) {
        size_t full = (size_t)(cl & ~15ULL);
        size_t rem = (size_t)(cl & 15ULL);
        if (full > 0u) tz_poly_blocks(&st, ct, full);
        if (rem > 0u) {
            uint8_t blk[16];
            memset(blk, 0, sizeof(blk));
            memcpy(blk, ct + full, rem);
            tz_poly_blocks(&st, blk, 16u);
        }
    }
    uint8_t lens[16];
    for (int i = 0; i < 8; ++i) {
        lens[i] = (uint8_t)(al >> (i * 8));
        lens[8 + i] = (uint8_t)(cl >> (i * 8));
    }
    tz_poly_blocks(&st, lens, 16u);
    tz_poly_finish(&st, tag16);
    tz_c_zero(otk, sizeof(otk));
}

int32_t tz_crypto_chacha20_poly1305_seal(const uint8_t* key32, const uint8_t* nonce12,
                                         const uint8_t* aad, int64_t aad_len,
                                         const uint8_t* pt, int64_t pt_len,
                                         uint8_t* ct_out, uint8_t* tag16_out) {
    if (!key32 || !nonce12 || !tag16_out || pt_len < 0 || aad_len < 0) return 0;
    if (pt_len > 0 && (!pt || !ct_out)) return 0;
    if (pt_len > 0) tz_chacha20_xor(key32, 1u, nonce12, pt, ct_out, pt_len);
    tz_aead_poly_tag(key32, nonce12, aad, aad_len, ct_out, pt_len, tag16_out);
    return 1;
}

int32_t tz_crypto_chacha20_poly1305_open(const uint8_t* key32, const uint8_t* nonce12,
                                         const uint8_t* aad, int64_t aad_len,
                                         const uint8_t* ct, int64_t ct_len,
                                         const uint8_t* tag16, uint8_t* pt_out) {
    if (!key32 || !nonce12 || !tag16 || ct_len < 0 || aad_len < 0) return 0;
    if (ct_len > 0 && (!ct || !pt_out)) return 0;
    uint8_t expected[16];
    tz_aead_poly_tag(key32, nonce12, aad, aad_len, ct, ct_len, expected);
    int32_t ok = tz_crypto_ct_eq(expected, tag16, 16);
    tz_c_zero(expected, sizeof(expected));
    if (!ok) {
        if (ct_len > 0 && pt_out) tz_c_zero(pt_out, (size_t)ct_len);
        return 0;
    }
    if (ct_len > 0) tz_chacha20_xor(key32, 1u, nonce12, ct, pt_out, ct_len);
    return 1;
}

typedef struct tz_c_turi_str {
    uint32_t hdr;
    uint32_t word1;
    const char* ptr;
} tz_c_turi_str;

_Static_assert(sizeof(tz_c_turi_str) == 16, "Turi %text size mismatch");
_Static_assert(offsetof(tz_c_turi_str, word1) == 4, "Turi %text word1 offset mismatch");
_Static_assert(offsetof(tz_c_turi_str, ptr) == 8, "Turi %text ptr offset mismatch");

static inline const uint8_t* tz_c_tstr_view(const void* p, int64_t* out_len) {
    if (!p) { *out_len = 0; return NULL; }
    const tz_c_turi_str* ts = (const tz_c_turi_str*)p;
    size_t n = (size_t)(ts->hdr & 0x7FFFFFFFu);
    *out_len = (int64_t)n;
    if (n == 0u) return NULL;
    if (n <= 12u) return (const uint8_t*)&ts->word1;
    return (const uint8_t*)(ts->ptr ? ts->ptr : "");
}

void tz_crypto_sha256_str(const void* tstr, uint8_t* out32) {
    int64_t n = 0;
    const uint8_t* p = tz_c_tstr_view(tstr, &n);
    tz_crypto_sha256(p, n, out32);
}

void tz_crypto_hmac_sha256_str(const void* key_tstr, const void* msg_tstr, uint8_t* out32) {
    int64_t kn = 0, mn = 0;
    const uint8_t* kp = tz_c_tstr_view(key_tstr, &kn);
    const uint8_t* mp = tz_c_tstr_view(msg_tstr, &mn);
    tz_crypto_hmac_sha256(kp, kn, mp, mn, out32);
}

int32_t tz_crypto_ct_eq_str(const void* a_tstr, const void* b_tstr) {
    int64_t an = 0, bn = 0;
    const uint8_t* ap = tz_c_tstr_view(a_tstr, &an);
    const uint8_t* bp = tz_c_tstr_view(b_tstr, &bn);
    if (an != bn) return 0;
    return tz_crypto_ct_eq(ap, bp, an);
}
)c";

} // namespace crypto_rt_parts

inline std::string turiCryptoRuntimeC() {
    std::string out;
    out.reserve(16384);
    out += crypto_rt_parts::kCryptoHeaderAndSha;
    out += crypto_rt_parts::kCryptoCtRngAead;
    return out;
}

} // namespace veloz::native
