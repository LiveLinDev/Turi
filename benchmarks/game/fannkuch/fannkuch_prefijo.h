/* Agrupa los 24 órdenes por su primer valor y reduce al terminar. */
typedef struct {
    __m512i value, reverse;
} Prefix;

static inline Prefix start_group(uint64_t base, uint64_t inverse, unsigned lead,
                                 __m512i sb, __m512i sc, __m512i sd) {
    uint64_t a = (base >> lead) & 15u;
    if (!a) return (Prefix){_mm512_setzero_si512(), _mm512_setzero_si512()};
    const __m512i all = _mm512_set1_epi64(-1), mask = _mm512_set1_epi64(15);
    __m512i data = _mm512_set1_epi64(base);
    __m512i b = _mm512_and_si512(_mm512_srlv_epi64(data, sb), mask);
    __m512i c = _mm512_and_si512(_mm512_srlv_epi64(data, sc), mask);
    __m512i d = _mm512_and_si512(_mm512_srlv_epi64(data, sd), mask);
    __m512i value = _mm512_set1_epi64((base & ~UINT64_C(65535)) | a);
    value = _mm512_or_si512(value, _mm512_or_si512(_mm512_slli_epi64(b, 4),
                            _mm512_or_si512(_mm512_slli_epi64(c, 8), _mm512_slli_epi64(d, 12))));
    __m512i reversed = _mm512_set1_epi64((inverse & UINT64_C(0xffffffffffff)) | (a << 60));
    reversed = _mm512_or_si512(reversed, _mm512_or_si512(_mm512_slli_epi64(b, 56),
                               _mm512_or_si512(_mm512_slli_epi64(c, 52), _mm512_slli_epi64(d, 48))));
    __m512i shift = _mm512_set1_epi64(60 - 4 * a);
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
    const __m512i all = _mm512_set1_epi64(-1), mask = _mm512_set1_epi64(15);
    const __m512i one = _mm512_set1_epi64(1), sixty = _mm512_set1_epi64(60);
    __m512i fa = _mm512_set1_epi64(((base >> left) & 15) != 0);
    __m512i fb = _mm512_set1_epi64(((base >> right) & 15) != 0);
    for (;;) {
        __m512i first_a = _mm512_and_si512(a.value, mask), first_b = _mm512_and_si512(b.value, mask);
        __mmask8 active_a = _mm512_test_epi64_mask(first_a, first_a);
        __mmask8 active_b = _mm512_test_epi64_mask(first_b, first_b);
        if (!(active_a | active_b)) break;
        __m512i shift_a = _mm512_sub_epi64(sixty, _mm512_slli_epi64(first_a, 2));
        __m512i shift_b = _mm512_sub_epi64(sixty, _mm512_slli_epi64(first_b, 2));
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
    if (((base >> lead) & 15u) == 0) return;
    Prefix prefix = start_group(base, inverse, lead, sb, sc, sd);
    __m512i value = prefix.value, reversed = prefix.reverse;
    const __m512i all = _mm512_set1_epi64(-1), mask = _mm512_set1_epi64(15);
    const __m512i one = _mm512_set1_epi64(1), sixty = _mm512_set1_epi64(60);
    __m512i flips = _mm512_setzero_si512();
    for (;;) {
        __m512i first = _mm512_and_si512(value, mask);
        __mmask8 active = _mm512_test_epi64_mask(first, first);
        if (!active) break;
        __m512i shift = _mm512_sub_epi64(sixty, _mm512_slli_epi64(first, 2));
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

static void fannkuch(int n, int64_t *checksum, int *maximum) {
    if (n < 4 || n > 12) {
        fannkuch_shuffle(n, checksum, maximum);
        return;
    }
    uint64_t a = UINT64_C(0xfedcba9876543210), digits = UINT64_C(0x0fedcba987654321);
    __m512i sum = _mm512_setzero_si512(), record = sum;
    do {
        uint64_t bytes = __builtin_bswap64(a), nibble = UINT64_C(0x0f0f0f0f0f0f0f0f);
        uint64_t inverse = ((bytes & nibble) << 4) | ((bytes >> 4) & nibble);
#ifdef TZ_FANNKUCH_DOS_GRUPOS
        first_pair(a, inverse, 0, 4,
                   _mm512_setr_epi64(4, 8, 8, 12, 4, 12, 4, 8),
                   _mm512_setr_epi64(8, 4, 12, 8, 12, 4, 8, 4),
                   _mm512_setr_epi64(12, 12, 4, 4, 8, 8, 12, 12),
                   _mm512_setr_epi64(8, 0, 8, 12, 12, 0, 8, 0),
                   _mm512_setr_epi64(0, 8, 12, 8, 0, 12, 0, 8),
                   _mm512_setr_epi64(12, 12, 0, 0, 8, 8, 12, 12), &sum, &record);
        first_pair(a, inverse, 8, 12,
                   _mm512_setr_epi64(0, 4, 12, 4, 12, 0, 0, 4),
                   _mm512_setr_epi64(4, 0, 4, 12, 0, 12, 4, 0),
                   _mm512_setr_epi64(12, 12, 0, 0, 4, 4, 12, 12),
                   _mm512_setr_epi64(4, 8, 0, 8, 0, 4, 4, 8),
                   _mm512_setr_epi64(8, 4, 8, 0, 4, 0, 8, 4),
                   _mm512_setr_epi64(0, 0, 4, 4, 8, 8, 0, 0), &sum, &record);
#else
        first_group(a, inverse, 0,
                    _mm512_setr_epi64(4, 8, 8, 12, 4, 12, 4, 8),
                    _mm512_setr_epi64(8, 4, 12, 8, 12, 4, 8, 4),
                    _mm512_setr_epi64(12, 12, 4, 4, 8, 8, 12, 12), &sum, &record);
        first_group(a, inverse, 4,
                    _mm512_setr_epi64(8, 0, 8, 12, 12, 0, 8, 0),
                    _mm512_setr_epi64(0, 8, 12, 8, 0, 12, 0, 8),
                    _mm512_setr_epi64(12, 12, 0, 0, 8, 8, 12, 12), &sum, &record);
        first_group(a, inverse, 8,
                    _mm512_setr_epi64(0, 4, 12, 4, 12, 0, 0, 4),
                    _mm512_setr_epi64(4, 0, 4, 12, 0, 12, 4, 0),
                    _mm512_setr_epi64(12, 12, 0, 0, 4, 4, 12, 12), &sum, &record);
        first_group(a, inverse, 12,
                    _mm512_setr_epi64(4, 8, 0, 8, 0, 4, 4, 8),
                    _mm512_setr_epi64(8, 4, 8, 0, 4, 0, 8, 4),
                    _mm512_setr_epi64(0, 0, 4, 4, 8, 8, 0, 0), &sum, &record);
#endif
    } while (next_higher(&a, &digits, n));
    const __m512i signs = _mm512_setr_epi64(1, -1, 1, -1, 1, -1, 0, 0);
    *checksum = _mm512_reduce_add_epi64(_mm512_mullo_epi64(sum, signs));
    *maximum = (int)_mm512_reduce_max_epu64(record);
}
