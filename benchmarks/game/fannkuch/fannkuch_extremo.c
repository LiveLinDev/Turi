/* Una permutación entera cabe en un registro. Cada inversión o rotación de
   prefijo es un pshufb. Se mantiene el orden de enumeración y su paridad. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <stdint.h>

#ifndef __SSSE3__
#error Este banco requiere SSSE3
#endif

static void fannkuch_shuffle(int n, int64_t *checksum, int *maximum) {
    __m128i reverse[16], rotate[16];
    int left[16];
    for (int k = 0; k < n; k++) {
        uint8_t src_reverse[16], src_rotate[16];
        for (int i = 0; i < 16; i++) {
            src_reverse[i] = (uint8_t)(i <= k ? k - i : i);
            src_rotate[i] = (uint8_t)(i < k ? i + 1 : (i == k ? 0 : i));
        }
        reverse[k] = _mm_loadu_si128((const __m128i *)src_reverse);
        rotate[k] = _mm_loadu_si128((const __m128i *)src_rotate);
        left[k] = k + 1;
    }
    __m128i permutation = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    int64_t sum = 0;
    int max_flips = 0, sign = 1;
    for (;;) {
        __m128i current = permutation;
        int flips = 0;
        for (unsigned first = (uint8_t)_mm_cvtsi128_si32(current); first;
             first = (uint8_t)_mm_cvtsi128_si32(current)) {
            current = _mm_shuffle_epi8(current, reverse[first]);
            flips++;
        }
        if (flips > max_flips) max_flips = flips;
        sum += sign * flips;
        sign = -sign;
        int k = 1;
        for (; k < n; k++) {
            permutation = _mm_shuffle_epi8(permutation, rotate[k]);
            if (--left[k] > 0) break;
            left[k] = k + 1;
        }
        if (k == n) break;
    }
    *checksum = sum;
    *maximum = max_flips;
}

#ifdef TZ_FANNKUCH_PAREJAS
/* Cada registro contiene cuatro permutaciones. Las dos de cada pareja
   se obtienen intercambiando sus primeras posiciones. */
static inline int next_higher(uint64_t *p, uint64_t *digits, int n) {
    uint64_t value = *p, counts = *digits;
#ifdef TZ_FANNKUCH_CUATRO
    const int first = 4;
#elif defined(TZ_FANNKUCH_SEIS)
    const int first = 3;
#else
    const int first = 2;
#endif
    for (int k = first; k < n; k++) {
        unsigned shift = (unsigned)k * 4u;
        uint64_t low = UINT64_MAX >> (64u - shift);
        value = (value & ~((low << 4) | 15u)) | ((value >> 4) & low) | ((value & 15u) << shift);
        counts -= UINT64_C(1) << shift;
        if ((counts >> shift) & 15u) {
            *p = value;
            *digits = counts;
            return 1;
        }
        counts |= (uint64_t)(k + 1) << shift;
    }
    return 0;
}

static inline uint64_t swap_first(uint64_t p) {
    return (p & ~UINT64_C(255)) | ((p & 15u) << 4) | ((p >> 4) & 15u);
}
#ifdef TZ_FANNKUCH_CUATRO
#include "fannkuch_prefijo.h"
#elif defined(TZ_FANNKUCH_SEIS)
static uint64_t rotate_three(uint64_t x) {
    return (x & ~UINT64_C(4095)) | ((x >> 4) & 255) | ((x & 15) << 8);
}

static inline __m512i reverse_nibbles(__m512i value, __m512i order, __m512i nibble) {
    __m512i bytes = _mm512_shuffle_epi8(value, order);
    return _mm512_or_si512(_mm512_slli_epi64(_mm512_and_si512(bytes, nibble), 4),
                           _mm512_and_si512(_mm512_srli_epi64(bytes, 4), nibble));
}

static void fannkuch(int n, int64_t *checksum, int *maximum) {
    if (n < 4 || n > 12) {
        fannkuch_shuffle(n, checksum, maximum);
        return;
    }
    const __m512i zero = _mm512_setzero_si512(), one = _mm512_set1_epi64(1);
    const __m512i all = _mm512_set1_epi64(-1), sixty = _mm512_set1_epi64(60);
    const __m512i last = _mm512_set1_epi64(15), nibble = _mm512_set1_epi64(INT64_C(0x0f0f0f0f0f0f0f0f));
    static const uint8_t order_bytes[64] = {
        7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8, 7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8,
        7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8, 7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8
    };
    const __m512i order = _mm512_loadu_si512(order_bytes);
    const __m512i signs = _mm512_setr_epi64(1, -1, 1, -1, 1, -1, 0, 0);
    uint64_t digits = UINT64_C(0x0fedcba987654321);
    uint64_t a = UINT64_C(0xfedcba9876543210);
    int64_t sum = 0;
    uint64_t record = 0;
    do {
        uint64_t c = rotate_three(a), e = rotate_three(c);
        __m512i value = _mm512_setr_epi64(a, swap_first(a), c, swap_first(c), e, swap_first(e), 0, 0);
#ifdef TZ_FANNKUCH_INVERSAS
        __m512i reversed = reverse_nibbles(value, order, nibble);
#endif
        __m512i flips = zero;
        for (;;) {
            __m512i first = _mm512_and_si512(value, last);
            __mmask8 live = _mm512_test_epi64_mask(first,first);
            if (!live) break;
            __m512i shift = _mm512_sub_epi64(sixty, _mm512_slli_epi64(first, 2));
            __m512i prefix = _mm512_srlv_epi64(all, shift);
#ifndef TZ_FANNKUCH_INVERSAS
            __m512i reversed = reverse_nibbles(value, order, nibble);
#endif
            __m512i next = _mm512_or_si512(_mm512_andnot_si512(prefix, value), _mm512_srlv_epi64(reversed, shift));
#ifdef TZ_FANNKUCH_INVERSAS
            __m512i suffix = _mm512_sllv_epi64(all, shift);
            reversed = _mm512_or_si512(_mm512_andnot_si512(suffix, reversed), _mm512_sllv_epi64(value, shift));
#endif
            value = next;
            flips = _mm512_mask_add_epi64(flips, live, flips, one);
        }
        sum += _mm512_reduce_add_epi64(_mm512_mullo_epi64(flips, signs));
        uint64_t high = _mm512_reduce_max_epu64(flips);
        if (high > record) record = high;
    } while (next_higher(&a, &digits, n));
    *checksum = sum;
    *maximum = (int)record;
}
#else
static void fannkuch(int n, int64_t *checksum, int *maximum) {
    if (n < 4 || n > 12) {
        fannkuch_shuffle(n, checksum, maximum);
        return;
    }
    const __m256i zero = _mm256_setzero_si256(), one = _mm256_set1_epi64x(1);
    const __m256i all = _mm256_set1_epi64x(-1), sixty = _mm256_set1_epi64x(60);
    const __m256i last = _mm256_set1_epi64x(15), nibble = _mm256_set1_epi64x(INT64_C(0x0f0f0f0f0f0f0f0f));
    const __m256i order = _mm256_setr_epi8(7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8,
                                          7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8);
    uint64_t digits = UINT64_C(0x0fedcba987654321);
    uint64_t a = UINT64_C(0xfedcba9876543210);
    int64_t sum = 0;
    int record = 0;
    for (;;) {
        uint64_t b = swap_first(a), c = a;
        next_higher(&c, &digits, n);
        uint64_t d = swap_first(c);
        __m256i value = _mm256_setr_epi64x((int64_t)a, (int64_t)b, (int64_t)c, (int64_t)d);
        __m256i flips = zero;
        for (;;) {
            __m256i first = _mm256_and_si256(value, last);
            if (_mm256_testz_si256(first, first)) break;
            __m256i shift = _mm256_sub_epi64(sixty, _mm256_slli_epi64(first, 2));
            __m256i prefix = _mm256_srlv_epi64(all, shift);
            __m256i bytes = _mm256_shuffle_epi8(value, order);
            __m256i reversed = _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(bytes, nibble), 4),
                                             _mm256_and_si256(_mm256_srli_epi64(bytes, 4), nibble));
            value = _mm256_or_si256(_mm256_andnot_si256(prefix, value), _mm256_srlv_epi64(reversed, shift));
            flips = _mm256_add_epi64(flips, _mm256_andnot_si256(_mm256_cmpeq_epi64(first, zero), one));
        }
        int64_t f[4];
        _mm256_storeu_si256((__m256i*)f, flips);
        sum += f[0] - f[1] + f[2] - f[3];
        for (int k = 0; k < 4; k++) if (f[k] > record) record = (int)f[k];
        a = c;
        if (!next_higher(&a, &digits, n)) break;
    }
    *maximum = record;
    *checksum = sum;
}
#endif
#else
#define fannkuch fannkuch_shuffle
#endif

int main(int argc, char **argv) {
    long requested = argc > 1 ? strtol(argv[1], NULL, 10) : 7;
    if (requested < 1 || requested > 16) return EXIT_FAILURE;
    double start = c_extremo_inicio();
    int64_t checksum;
    int flips;
    fannkuch((int)requested, &checksum, &flips);
    c_extremo_fin(start);
    printf("%lld\n%d\n", (long long)checksum, flips);
    return EXIT_SUCCESS;
}
