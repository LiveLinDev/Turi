/* Procesa los 24 órdenes de las primeras cuatro fichas y reduce al terminar. */
#ifdef TZ_FANNKUCH_CODIFICADO
enum { CARD_BITS = 6, CARD_MASK = 63, CARD_END = 58, PACK_LIMIT = 10 };
#else
enum { CARD_BITS = 4, CARD_MASK = 15, CARD_END = 0, PACK_LIMIT = 12 };
#endif

static inline unsigned flip_shift(unsigned card) {
#ifdef TZ_FANNKUCH_CODIFICADO
    return card;
#else
    return 60 - 4 * card;
#endif
}

static inline __m512i flip_shifts(__m512i cards) {
#ifdef TZ_FANNKUCH_CODIFICADO
    return cards;
#else
    return _mm512_sub_epi64(_mm512_set1_epi64(60), _mm512_slli_epi64(cards, 2));
#endif
}

static inline __mmask8 active_cards(__m512i cards) {
#ifdef TZ_FANNKUCH_CODIFICADO
    return _mm512_cmplt_epu64_mask(cards, _mm512_set1_epi64(CARD_END));
#else
    return _mm512_test_epi64_mask(cards, cards);
#endif
}

#ifdef TZ_FANNKUCH_OCHO_CARRILES
static inline void count_orders(uint64_t base, uint64_t inverse,
                                __m512i sa, __m512i sb, __m512i sc, __m512i sd,
                                __m512i *sum, __m512i *record) {
    __m512i data = _mm512_set1_epi64(base), mask = _mm512_set1_epi64(CARD_MASK);
    __m512i a = _mm512_and_si512(_mm512_srlv_epi64(data, sa), mask);
    __m512i b = _mm512_and_si512(_mm512_srlv_epi64(data, sb), mask);
    __m512i c = _mm512_and_si512(_mm512_srlv_epi64(data, sc), mask);
    __m512i d = _mm512_and_si512(_mm512_srlv_epi64(data, sd), mask);
    uint64_t keep = UINT64_MAX >> 24;
    __m512i value = _mm512_or_si512(_mm512_set1_epi64(base & (keep << 24)), a);
    value = _mm512_or_si512(value, _mm512_slli_epi64(b, 6));
    value = _mm512_or_si512(value, _mm512_slli_epi64(c, 12));
    value = _mm512_or_si512(value, _mm512_slli_epi64(d, 18));
    __m512i reverse = _mm512_or_si512(_mm512_set1_epi64(inverse & keep), _mm512_slli_epi64(a, 58));
    reverse = _mm512_or_si512(reverse, _mm512_slli_epi64(b, 52));
    reverse = _mm512_or_si512(reverse, _mm512_slli_epi64(c, 46));
    reverse = _mm512_or_si512(reverse, _mm512_slli_epi64(d, 40));
    __m512i one = _mm512_set1_epi64(1), all = _mm512_set1_epi64(-1);
    __m512i flips = _mm512_setzero_si512();
    __m512i first = a;
    __mmask8 active = active_cards(first);
    while (active) {
        __m512i next = _mm512_or_si512(_mm512_andnot_si512(_mm512_srlv_epi64(all, first), value),
                                      _mm512_srlv_epi64(reverse, first));
        reverse = _mm512_or_si512(_mm512_andnot_si512(_mm512_sllv_epi64(all, first), reverse),
                                 _mm512_sllv_epi64(value, first));
        value = next;
        flips = _mm512_mask_add_epi64(flips, active, flips, one);
        first = _mm512_and_si512(value, mask);
        active = active_cards(first);
    }
    *sum = _mm512_add_epi64(*sum, flips);
    *record = _mm512_max_epu64(*record, flips);
}
#else
typedef struct {
    __m512i value, reverse;
} Prefix;

static inline Prefix start_group(uint64_t base, uint64_t inverse, unsigned lead,
                                 __m512i sb, __m512i sc, __m512i sd) {
    uint64_t a = (base >> lead) & CARD_MASK;
    if (a == CARD_END) {
        return (Prefix){_mm512_set1_epi64(CARD_END), _mm512_set1_epi64((uint64_t)CARD_END << (64 - CARD_BITS))};
    }
    const __m512i all = _mm512_set1_epi64(-1), mask = _mm512_set1_epi64(CARD_MASK);
    __m512i data = _mm512_set1_epi64(base);
    __m512i b = _mm512_and_si512(_mm512_srlv_epi64(data, sb), mask);
    __m512i c = _mm512_and_si512(_mm512_srlv_epi64(data, sc), mask);
    __m512i d = _mm512_and_si512(_mm512_srlv_epi64(data, sd), mask);
    const uint64_t keep = UINT64_MAX >> (4 * CARD_BITS);
    __m512i value = _mm512_set1_epi64((base & (keep << (4 * CARD_BITS))) | a);
    value = _mm512_or_si512(value, _mm512_or_si512(_mm512_slli_epi64(b, CARD_BITS),
                            _mm512_or_si512(_mm512_slli_epi64(c, 2 * CARD_BITS), _mm512_slli_epi64(d, 3 * CARD_BITS))));
    __m512i reversed = _mm512_set1_epi64((inverse & keep) | (a << (64 - CARD_BITS)));
    reversed = _mm512_or_si512(reversed, _mm512_or_si512(_mm512_slli_epi64(b, 64 - 2 * CARD_BITS),
                               _mm512_or_si512(_mm512_slli_epi64(c, 64 - 3 * CARD_BITS), _mm512_slli_epi64(d, 64 - 4 * CARD_BITS))));
    __m512i shift = _mm512_set1_epi64(flip_shift((unsigned)a));
    __m512i next = _mm512_or_si512(_mm512_andnot_si512(_mm512_srlv_epi64(all, shift), value),
                                   _mm512_srlv_epi64(reversed, shift));
    reversed = _mm512_or_si512(_mm512_andnot_si512(_mm512_sllv_epi64(all, shift), reversed),
                              _mm512_sllv_epi64(value, shift));
    return (Prefix){next, reversed};
}

#ifdef TZ_FANNKUCH_DOS_GRUPOS
static inline void first_pair(uint64_t base, uint64_t inverse, unsigned left, unsigned right,
                              __m512i lb, __m512i lc, __m512i ld, __m512i rb, __m512i rc, __m512i rd,
                              __m512i *sum, __m512i *record) {
    Prefix a = start_group(base, inverse, left, lb, lc, ld);
    Prefix b = start_group(base, inverse, right, rb, rc, rd);
    const __m512i all = _mm512_set1_epi64(-1), mask = _mm512_set1_epi64(CARD_MASK);
    const __m512i one = _mm512_set1_epi64(1);
    __m512i fa = _mm512_set1_epi64(((base >> left) & CARD_MASK) != CARD_END);
    __m512i fb = _mm512_set1_epi64(((base >> right) & CARD_MASK) != CARD_END);
    for (;;) {
        __m512i first_a = _mm512_and_si512(a.value, mask), first_b = _mm512_and_si512(b.value, mask);
        __mmask8 active_a = active_cards(first_a);
        __mmask8 active_b = active_cards(first_b);
        if (!(active_a | active_b)) break;
        __m512i shift_a = flip_shifts(first_a);
        __m512i shift_b = flip_shifts(first_b);
        __m512i next_a = _mm512_or_si512(_mm512_andnot_si512(_mm512_srlv_epi64(all, shift_a), a.value),
                                       _mm512_srlv_epi64(a.reverse, shift_a));
        __m512i next_b = _mm512_or_si512(_mm512_andnot_si512(_mm512_srlv_epi64(all, shift_b), b.value),
                                       _mm512_srlv_epi64(b.reverse, shift_b));
        a.reverse = _mm512_or_si512(_mm512_andnot_si512(_mm512_sllv_epi64(all, shift_a), a.reverse),
                                   _mm512_sllv_epi64(a.value, shift_a));
        b.reverse = _mm512_or_si512(_mm512_andnot_si512(_mm512_sllv_epi64(all, shift_b), b.reverse),
                                   _mm512_sllv_epi64(b.value, shift_b));
        a.value = next_a;
        b.value = next_b;
        fa = _mm512_mask_add_epi64(fa, active_a, fa, one);
        fb = _mm512_mask_add_epi64(fb, active_b, fb, one);
    }
    *sum = _mm512_add_epi64(*sum, _mm512_add_epi64(fa, fb));
    *record = _mm512_max_epu64(*record, _mm512_max_epu64(fa, fb));
}
#else
static inline void first_group(uint64_t base, uint64_t inverse, unsigned lead,
                               __m512i sb, __m512i sc, __m512i sd, __m512i *sum, __m512i *record) {
    if (((base >> lead) & CARD_MASK) == CARD_END) return;
    Prefix prefix = start_group(base, inverse, lead, sb, sc, sd);
    __m512i value = prefix.value, reversed = prefix.reverse;
    const __m512i all = _mm512_set1_epi64(-1), mask = _mm512_set1_epi64(CARD_MASK);
    const __m512i one = _mm512_set1_epi64(1);
    __m512i flips = _mm512_setzero_si512();
    for (;;) {
        __m512i first = _mm512_and_si512(value, mask);
        __mmask8 active = active_cards(first);
        if (!active) break;
        __m512i shift = flip_shifts(first);
        __m512i next = _mm512_or_si512(_mm512_andnot_si512(_mm512_srlv_epi64(all, shift), value),
                                      _mm512_srlv_epi64(reversed, shift));
        reversed = _mm512_or_si512(_mm512_andnot_si512(_mm512_sllv_epi64(all, shift), reversed),
                                 _mm512_sllv_epi64(value, shift));
        value = next;
        flips = _mm512_mask_add_epi64(flips, active, flips, one);
    }
    flips = _mm512_add_epi64(flips, one);
    *sum = _mm512_add_epi64(*sum, flips);
    *record = _mm512_max_epu64(*record, flips);
}
#endif
#endif

#ifdef TZ_FANNKUCH_CODIFICADO
static inline int next_encoded(uint64_t *permutation, uint64_t *inverse, uint64_t *digits, int n) {
    uint64_t value = *permutation, reverse = *inverse, counts = *digits;
    for (int k = 4; k < n; ++k) {
        unsigned shift = CARD_BITS * k, reverse_shift = CARD_END - shift, digit = 4 * k;
        uint64_t first = value & CARD_MASK, low = UINT64_MAX >> (64 - shift);
        uint64_t high = UINT64_MAX << (reverse_shift + CARD_BITS);
        value = (value & ~((low << CARD_BITS) | CARD_MASK)) | ((value >> CARD_BITS) & low) | (first << shift);
        reverse = (reverse & ~(high | ((uint64_t)CARD_MASK << reverse_shift))) |
                  ((reverse << CARD_BITS) & high) | (first << reverse_shift);
        counts -= UINT64_C(1) << digit;
        if ((counts >> digit) & 15) {
            *permutation = value;
            *inverse = reverse;
            *digits = counts;
            return 1;
        }
        counts |= (uint64_t)(k + 1) << digit;
    }
    return 0;
}
#endif

#define CARD_ORDER(a, b, c, d, e, f, g, h) _mm512_setr_epi64( \
    CARD_BITS*(a), CARD_BITS*(b), CARD_BITS*(c), CARD_BITS*(d), \
    CARD_BITS*(e), CARD_BITS*(f), CARD_BITS*(g), CARD_BITS*(h))

#ifdef TZ_FANNKUCH_PARCIALES
#include "fannkuch_parciales.h"
#endif

static void fannkuch(int n, int64_t *checksum, int *maximum) {
#ifdef TZ_FANNKUCH_PARCIALES
    if (n >= 7 && n <= 10) {
        *maximum = 0;
#ifdef TZ_FANNKUCH_INVERSO_PARCIAL
#ifdef TZ_FANNKUCH_CINCO_BITS
        uint64_t open = UINT64_C(0x2108421084210) & ((UINT64_C(1) << (5 * n)) - 1) & ~UINT64_C(16);
        uint64_t result = partial_inverse(10, UINT64_C(0x5000000000000000), open, open, 0);
        *checksum = (int64_t)result >> 32;
        *maximum = (int)(uint32_t)result;
#else
        uint64_t open = UINT64_C(0x0820820820820820) & ((UINT64_C(1) << (6 * n)) - 1) & ~UINT64_C(32);
        *checksum = partial_inverse(58, UINT64_C(0xe800000000000000), open, open, 0, maximum);
#endif
#else
#ifdef TZ_FANNKUCH_FICHAS
        const uint64_t direct = UINT64_C(0x01c71c71c71c50c1), reverse = UINT64_C(0x0431471c71c71c70);
#else
        const uint64_t direct = UINT64_C(0x0db0aa479848c180), reverse = UINT64_C(0x00631261e92ac360);
#endif
        *checksum = partial(direct, reverse, 0, (UINT32_C(1) << n) - 1, 0, maximum);
#endif
        return;
    }
#endif
    if (n < 4 || n > PACK_LIMIT) {
        fannkuch_shuffle(n, checksum, maximum);
        return;
    }
    uint64_t digits = UINT64_C(0x0fedcba987654321);
#ifdef TZ_FANNKUCH_CODIFICADO
    uint64_t a = UINT64_C(0x10a416722a2ed3a), inverse = UINT64_C(0xeb4ba889c5902840);
#else
    uint64_t a = UINT64_C(0xfedcba9876543210);
#endif
    __m512i sum = _mm512_setzero_si512(), record = sum;
    do {
#ifndef TZ_FANNKUCH_CODIFICADO
        uint64_t bytes = __builtin_bswap64(a), nibble = UINT64_C(0x0f0f0f0f0f0f0f0f);
        uint64_t inverse = ((bytes & nibble) << 4) | ((bytes >> 4) & nibble);
#endif
#if defined(TZ_FANNKUCH_OCHO_CARRILES) && defined(TZ_FANNKUCH_ORDEN_RAICES)
        count_orders(a, inverse, CARD_ORDER(0, 0, 0, 0, 0, 0, 1, 1), CARD_ORDER(1, 2, 2, 3, 1, 3, 2, 0),
                     CARD_ORDER(2, 1, 3, 2, 3, 1, 0, 2), CARD_ORDER(3, 3, 1, 1, 2, 2, 3, 3), &sum, &record);
        count_orders(a, inverse, CARD_ORDER(1, 1, 1, 1, 2, 2, 2, 2), CARD_ORDER(2, 3, 3, 0, 0, 1, 3, 1),
                     CARD_ORDER(3, 2, 0, 3, 1, 0, 1, 3), CARD_ORDER(0, 0, 2, 2, 3, 3, 0, 0), &sum, &record);
        count_orders(a, inverse, CARD_ORDER(2, 2, 3, 3, 3, 3, 3, 3), CARD_ORDER(3, 0, 1, 2, 0, 2, 0, 1),
                     CARD_ORDER(0, 3, 2, 1, 2, 0, 1, 0), CARD_ORDER(1, 1, 0, 0, 1, 1, 2, 2), &sum, &record);
#elif defined(TZ_FANNKUCH_OCHO_CARRILES)
        count_orders(a, inverse, CARD_ORDER(0, 1, 1, 2, 2, 0, 1, 2), CARD_ORDER(1, 0, 2, 1, 0, 2, 2, 1),
                     CARD_ORDER(2, 2, 0, 0, 1, 1, 3, 3), CARD_ORDER(3, 3, 3, 3, 3, 3, 0, 0), &sum, &record);
        count_orders(a, inverse, CARD_ORDER(2, 3, 3, 1, 2, 3, 3, 0), CARD_ORDER(3, 2, 1, 3, 3, 2, 0, 3),
                     CARD_ORDER(1, 1, 2, 2, 0, 0, 2, 2), CARD_ORDER(0, 0, 0, 0, 1, 1, 1, 1), &sum, &record);
        count_orders(a, inverse, CARD_ORDER(0, 2, 3, 0, 0, 1, 1, 3), CARD_ORDER(2, 0, 0, 3, 1, 0, 3, 1),
                     CARD_ORDER(3, 3, 1, 1, 3, 3, 0, 0), CARD_ORDER(1, 1, 2, 2, 2, 2, 2, 2), &sum, &record);
#elif defined(TZ_FANNKUCH_DOS_GRUPOS)
        first_pair(a, inverse, 0, CARD_BITS,
                   CARD_ORDER(1, 2, 2, 3, 1, 3, 1, 2), CARD_ORDER(2, 1, 3, 2, 3, 1, 2, 1),
                   CARD_ORDER(3, 3, 1, 1, 2, 2, 3, 3), CARD_ORDER(2, 0, 2, 3, 3, 0, 2, 0),
                   CARD_ORDER(0, 2, 3, 2, 0, 3, 0, 2), CARD_ORDER(3, 3, 0, 0, 2, 2, 3, 3), &sum, &record);
        first_pair(a, inverse, 2 * CARD_BITS, 3 * CARD_BITS,
                   CARD_ORDER(0, 1, 3, 1, 3, 0, 0, 1), CARD_ORDER(1, 0, 1, 3, 0, 3, 1, 0),
                   CARD_ORDER(3, 3, 0, 0, 1, 1, 3, 3), CARD_ORDER(1, 2, 0, 2, 0, 1, 1, 2),
                   CARD_ORDER(2, 1, 2, 0, 1, 0, 2, 1), CARD_ORDER(0, 0, 1, 1, 2, 2, 0, 0), &sum, &record);
#else
        first_group(a, inverse, 0,
                    CARD_ORDER(1, 2, 2, 3, 1, 3, 1, 2), CARD_ORDER(2, 1, 3, 2, 3, 1, 2, 1),
                    CARD_ORDER(3, 3, 1, 1, 2, 2, 3, 3), &sum, &record);
        first_group(a, inverse, CARD_BITS,
                    CARD_ORDER(2, 0, 2, 3, 3, 0, 2, 0), CARD_ORDER(0, 2, 3, 2, 0, 3, 0, 2),
                    CARD_ORDER(3, 3, 0, 0, 2, 2, 3, 3), &sum, &record);
        first_group(a, inverse, 2 * CARD_BITS,
                    CARD_ORDER(0, 1, 3, 1, 3, 0, 0, 1), CARD_ORDER(1, 0, 1, 3, 0, 3, 1, 0),
                    CARD_ORDER(3, 3, 0, 0, 1, 1, 3, 3), &sum, &record);
        first_group(a, inverse, 3 * CARD_BITS,
                    CARD_ORDER(1, 2, 0, 2, 0, 1, 1, 2), CARD_ORDER(2, 1, 2, 0, 1, 0, 2, 1),
                    CARD_ORDER(0, 0, 1, 1, 2, 2, 0, 0), &sum, &record);
#endif
#ifdef TZ_FANNKUCH_CODIFICADO
    } while (next_encoded(&a, &inverse, &digits, n));
#else
    } while (next_higher(&a, &digits, n));
#endif
#ifdef TZ_FANNKUCH_OCHO_CARRILES
    const __m512i signs = _mm512_setr_epi64(1, -1, 1, -1, 1, -1, 1, -1);
#else
    const __m512i signs = _mm512_setr_epi64(1, -1, 1, -1, 1, -1, 0, 0);
#endif
    *checksum = _mm512_reduce_add_epi64(_mm512_mullo_epi64(sum, signs));
    *maximum = (int)_mm512_reduce_max_epu64(record);
}

#undef CARD_ORDER
