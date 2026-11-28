/* Salta las transiciones afines para construir cada fila de la semilla. */
static uint64_t seed_rows(uint64_t *live, size_t stride, unsigned origin) {
    __m256i scales[8], offsets[8];
    uint32_t power = 1, constant = 0;
    for (unsigned i = 0; i < 8; i++) {
        uint32_t a[8], b[8];
        for (unsigned j = 0; j < 8; j++) {
            power *= UINT32_C(1103515245);
            constant = constant * UINT32_C(1103515245) + UINT32_C(12345);
            a[j] = power;
            b[j] = constant;
        }
        scales[i] = _mm256_loadu_si256((const __m256i*)a);
        offsets[i] = _mm256_loadu_si256((const __m256i*)b);
    }
    uint32_t seed = 7;
    uint64_t count = 0;
    unsigned shift = origin & 63;
    for (unsigned row = origin; row < origin + 64; row++) {
        __m256i base = _mm256_set1_epi32((int32_t)seed);
        uint64_t living = 0;
        for (unsigned group = 0; group < 8; group++) {
            __m256i states = _mm256_add_epi32(_mm256_mullo_epi32(base, scales[group]), offsets[group]);
            __m256i sample = _mm256_and_si256(_mm256_srli_epi32(states, 16), _mm256_set1_epi32(32767));
            __m256i quotient = _mm256_srli_epi32(_mm256_mullo_epi32(sample, _mm256_set1_epi32(10923)), 15);
            __m256i remainder = _mm256_sub_epi32(sample, _mm256_mullo_epi32(quotient, _mm256_set1_epi32(3)));
            __m256i masks = _mm256_cmpeq_epi32(remainder, _mm256_setzero_si256());
            living |= (uint64_t)_mm256_movemask_ps(_mm256_castsi256_ps(masks)) << (8 * group);
            if (group == 7) seed = (uint32_t)_mm256_extract_epi32(states, 7);
        }
#ifdef TZ_VIDA_COLUMNAS
        size_t at = (1 + origin / 64) * stride + row + 8;
        size_t next_word = stride;
#else
        size_t at = row * stride + 1 + origin / 64;
        size_t next_word = 1;
#endif
        live[at] = living << shift;
        if (shift) live[at + next_word] = living >> (64 - shift);
        count += (unsigned)__builtin_popcountll(living);
    }
    return count;
}
