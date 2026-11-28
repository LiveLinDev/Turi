#ifndef TZ_PALABRAS_ADELANTO
#define TZ_PALABRAS_ADELANTO 0
#endif

static inline unsigned count_word_pair(uint64_t *counts, const uint32_t *pairs, unsigned at) {
    uint32_t pair = pairs[at];
    counts[pair & 2047]++;
    counts[(pair >> 11) & 2047]++;
#if TZ_PALABRAS_RELATIVOS
    return at + (pair >> 22);
#else
    return pair >> 22;
#endif
}

/* Los residuos y las transiciones de una palabra caben en carriles de 16 bits. */
static void count_words(uint64_t *counts, long long n) {
    enum { BLOCK = 256, WIDE = 16, NARROW = 32,
           CAPACITY = BLOCK + (TZ_PALABRAS_ADELANTO ? WIDE : NARROW),
           SINGLE = BLOCK + (TZ_PALABRAS_ADELANTO ? 8 : WIDE) };
    __m512i scales[CAPACITY / WIDE], offsets[CAPACITY / WIDE];
    uint32_t power = 1, add = 0;
    for (unsigned k = 0; k < CAPACITY / WIDE; ++k) {
        uint32_t scale[WIDE], bias[WIDE];
        for (unsigned lane = 0; lane < WIDE; ++lane) {
            power *= MULTIPLIER;
            add = add * MULTIPLIER + INCREMENT;
            scale[lane] = power;
            bias[lane] = add;
        }
        scales[k] = _mm512_loadu_si512(scale);
        offsets[k] = _mm512_loadu_si512(bias);
    }
    const __m512i keep = _mm512_set1_epi32(32767), magic = _mm512_set1_epi16(21846);
    const __m512i three = _mm512_set1_epi16(3), one = _mm512_set1_epi16(1);
    const __m512i zero = _mm512_setzero_si512(), radix = _mm512_set1_epi16(12);
    const __m512i mask = _mm512_set1_epi16(2047);
    uint32_t state = 42;
    unsigned at = 0;
    while (n > 0) {
        uint16_t symbols[CAPACITY], lengths[CAPACITY], single[SINGLE];
        uint32_t pairs[BLOCK];
        __m512i seed = _mm512_set1_epi32((int32_t)state);
        for (unsigned k = 0; k + 1 < CAPACITY / WIDE; k += 2) {
            __m512i first = _mm512_add_epi32(_mm512_mullo_epi32(seed, scales[k]), offsets[k]);
            __m512i second = _mm512_add_epi32(_mm512_mullo_epi32(seed, scales[k + 1]), offsets[k + 1]);
            __m256i low = _mm512_cvtepi32_epi16(_mm512_and_si512(_mm512_srli_epi32(first, 16), keep));
            __m256i high = _mm512_cvtepi32_epi16(_mm512_and_si512(_mm512_srli_epi32(second, 16), keep));
            __m512i samples = _mm512_inserti64x4(_mm512_castsi256_si512(low), high, 1);
            __m512i q = _mm512_mulhi_epu16(samples, magic);
            __m512i length = _mm512_sub_epi16(samples, _mm512_mullo_epi16(q, three));
            __m512i symbol = _mm512_add_epi16(length, _mm512_mullo_epi16(_mm512_and_si512(q, three), three));
            _mm512_storeu_si512(symbols + k * WIDE, symbol);
            _mm512_storeu_si512(lengths + k * WIDE, length);
            if (k == BLOCK / WIDE - 2)
                state = (uint32_t)_mm256_extract_epi32(_mm512_extracti64x4_epi64(second, 1), 7);
        }
#if TZ_PALABRAS_ADELANTO
        __m512i next_state = _mm512_add_epi32(_mm512_mullo_epi32(seed, scales[BLOCK / WIDE]), offsets[BLOCK / WIDE]);
        __m256i tail = _mm512_cvtepi32_epi16(_mm512_and_si512(_mm512_srli_epi32(next_state, 16), keep));
        __m256i m = _mm512_castsi512_si256(magic), t = _mm512_castsi512_si256(three);
        __m256i q = _mm256_mulhi_epu16(tail, m);
        __m256i len = _mm256_sub_epi16(tail, _mm256_mullo_epi16(q, t));
        __m256i symbol = _mm256_add_epi16(len, _mm256_mullo_epi16(_mm256_and_si256(q, t), t));
        _mm256_storeu_si256((__m256i *)(symbols + BLOCK), symbol);
        _mm256_storeu_si256((__m256i *)(lengths + BLOCK), len);
#endif
#if TZ_PALABRAS_ADELANTO && defined(TZ_PALABRAS_TRANSICIONES32)
        for (unsigned k = 0; k < BLOCK; k += NARROW) {
            __m512i length = _mm512_loadu_si512(lengths + k);
            __m512i a = _mm512_loadu_si512(symbols + k + 1);
            __m512i b = _mm512_add_epi16(_mm512_mullo_epi16(_mm512_add_epi16(a, one), radix),
                                        _mm512_loadu_si512(symbols + k + 2));
            __m512i c = _mm512_add_epi16(_mm512_mullo_epi16(_mm512_add_epi16(b, one), radix),
                                        _mm512_loadu_si512(symbols + k + 3));
            __m512i longer = _mm512_mask_blend_epi16(_mm512_cmpeq_epi16_mask(length, one), c, b);
            __m512i code = _mm512_mask_blend_epi16(_mm512_cmpeq_epi16_mask(length, zero), longer, a);
            _mm512_storeu_si512(single + k, _mm512_or_si512(_mm512_slli_epi16(length, 11), code));
        }
#else
        for (unsigned k = 0; k + WIDE <= SINGLE; k += WIDE) {
            const __m256i r = _mm512_castsi512_si256(radix), one16 = _mm512_castsi512_si256(one);
            const __m256i nada = _mm512_castsi512_si256(zero);
            __m256i length = _mm256_loadu_si256((const __m256i *)(lengths + k));
            __m256i a = _mm256_loadu_si256((const __m256i *)(symbols + k + 1));
            __m256i b = _mm256_add_epi16(_mm256_mullo_epi16(_mm256_add_epi16(a, one16), r),
                                           _mm256_loadu_si256((const __m256i *)(symbols + k + 2)));
            __m256i c = _mm256_add_epi16(_mm256_mullo_epi16(_mm256_add_epi16(b, one16), r),
                                           _mm256_loadu_si256((const __m256i *)(symbols + k + 3)));
            __m256i longer = _mm256_mask_blend_epi16(_mm256_cmpeq_epi16_mask(length, one16), c, b);
            __m256i code = _mm256_mask_blend_epi16(_mm256_cmpeq_epi16_mask(length, nada), longer, a);
            _mm256_storeu_si256((__m256i *)(single + k), _mm256_or_si256(_mm256_slli_epi16(length, 11), code));
        }
#endif
#if TZ_PALABRAS_ADELANTO
        {
            const __m128i r = _mm_set1_epi16(12), o = _mm_set1_epi16(1), z = _mm_setzero_si128();
            __m128i length = _mm_loadu_si128((const __m128i *)(lengths + BLOCK));
            __m128i a = _mm_loadu_si128((const __m128i *)(symbols + BLOCK + 1));
            __m128i b = _mm_add_epi16(_mm_mullo_epi16(_mm_add_epi16(a, o), r),
                                     _mm_loadu_si128((const __m128i *)(symbols + BLOCK + 2)));
            __m128i c = _mm_add_epi16(_mm_mullo_epi16(_mm_add_epi16(b, o), r),
                                     _mm_loadu_si128((const __m128i *)(symbols + BLOCK + 3)));
            __m128i longer = _mm_mask_blend_epi16(_mm_cmpeq_epi16_mask(length, o), c, b);
            __m128i code = _mm_mask_blend_epi16(_mm_cmpeq_epi16_mask(length, z), longer, a);
            _mm_storeu_si128((__m128i *)(single + BLOCK), _mm_or_si128(_mm_slli_epi16(length, 11), code));
        }
#endif
        if (n >= BLOCK / 2) {
            for (unsigned k = 0; k < BLOCK; k += WIDE) {
                __m256i first = _mm256_loadu_si256((const __m256i *)(single + k));
                __m256i length = _mm256_srli_epi16(first, 11);
                __m256i a = _mm256_loadu_si256((const __m256i *)(single + k + 2));
                __m256i b = _mm256_loadu_si256((const __m256i *)(single + k + 3));
                __m256i c = _mm256_loadu_si256((const __m256i *)(single + k + 4));
                __m256i next = _mm256_mask_blend_epi16(_mm256_cmpeq_epi16_mask(length, _mm512_castsi512_si256(one)), c, b);
                next = _mm256_mask_blend_epi16(_mm256_cmpeq_epi16_mask(length, _mm512_castsi512_si256(zero)), next, a);
                __m512i first_code = _mm512_cvtepu16_epi32(_mm256_and_si256(first, _mm512_castsi512_si256(mask)));
#if TZ_PALABRAS_ADELANTO
                __m512i next_code = _mm512_cvtepu16_epi32(next);
                __m512i jump = _mm512_cvtepu16_epi32(length);
#else
                __m512i next_code = _mm512_cvtepu16_epi32(_mm256_and_si256(next, _mm512_castsi512_si256(mask)));
                __m512i jump = _mm512_cvtepu16_epi32(_mm256_add_epi16(length, _mm256_srli_epi16(next, 11)));
#endif
#if TZ_PALABRAS_RELATIVOS
                jump = _mm512_add_epi32(jump, _mm512_set1_epi32(4));
#else
                const __m512i lanes = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
                jump = _mm512_add_epi32(jump, _mm512_add_epi32(lanes, _mm512_set1_epi32(k + 4)));
#endif
#if TZ_PALABRAS_ADELANTO
                /* El salto de la segunda transición ya está en los bits altos. */
                __m512i high = _mm512_add_epi32(_mm512_slli_epi32(jump, 22), _mm512_slli_epi32(next_code, 11));
                _mm512_storeu_si512(pairs + k, _mm512_or_si512(first_code, high));
#else
                __m512i pair = _mm512_or_si512(first_code, _mm512_slli_epi32(next_code, 11));
                _mm512_storeu_si512(pairs + k, _mm512_or_si512(pair, _mm512_slli_epi32(jump, 22)));
#endif
            }
#if TZ_PALABRAS_ADELANTO
            while (at < BLOCK - 8) {
                at = count_word_pair(counts, pairs, at);
                at = count_word_pair(counts, pairs, at);
                n -= 4;
            }
#endif
            while (at < BLOCK) {
                at = count_word_pair(counts, pairs, at);
                n -= 2;
            }
        } else {
            while (at < BLOCK && n > 0) {
                uint16_t word = single[at];
                at += (word >> 11) + 2;
                counts[word & 2047]++;
                --n;
            }
        }
        at -= BLOCK;
    }
}
