/* Dieciséis filas de palabras de 32 bits por vector. */
static inline __m512i from_left32(const uint32_t *word, __m512i center, size_t stride) {
    __m512i before = _mm512_loadu_si512(word - stride);
    return _mm512_or_si512(_mm512_slli_epi32(center, 1), _mm512_srli_epi32(before, 31));
}

static inline __m512i from_right32(const uint32_t *word, __m512i center, size_t stride) {
    __m512i after = _mm512_loadu_si512(word + stride);
    return _mm512_or_si512(_mm512_srli_epi32(center, 1), _mm512_slli_epi32(after, 31));
}

static inline Sides sides_at32(const uint32_t *word, size_t stride) {
    __m512i center = _mm512_loadu_si512(word);
    __m512i left = from_left32(word, center, stride), right = from_right32(word, center, stride);
    __m512i odd = _mm512_xor_si512(left, right), carry = _mm512_and_si512(left, right);
    return (Sides){center, odd, carry, {_mm512_xor_si512(odd, center),
                   _mm512_or_si512(carry, _mm512_and_si512(odd, center))}};
}

static uint64_t step_columns32(const uint32_t *restrict before, uint32_t *restrict after,
                             size_t stride, size_t columns, Active *active, int count_population) {
    unsigned begin = active->first, end = active->last;
    size_t first = active->first_block, last = active->last_block;
    __m512i population = _mm512_setzero_si512();
    const __m512i top = _mm512_setr_epi32(15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30);
    const __m512i bottom = _mm512_setr_epi32(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    for (size_t column = first; column <= last; column++) {
        size_t offset = column * stride;
        Sides up = sides_at32(before + offset + (begin - 1) * 16, stride);
        Sides middle = sides_at32(before + offset + begin * 16, stride);
        __m512i any = _mm512_setzero_si512();
        for (unsigned row = begin; row <= end; row++) {
            size_t at = offset + row * 16;
            Sides south = sides_at32(before + at + 16, stride);
            BitSum upper = {_mm512_permutex2var_epi32(up.triad.ones, top, middle.triad.ones),
                            _mm512_permutex2var_epi32(up.triad.twos, top, middle.triad.twos)};
            BitSum lower = {_mm512_permutex2var_epi32(middle.triad.ones, bottom, south.triad.ones),
                            _mm512_permutex2var_epi32(middle.triad.twos, bottom, south.triad.twos)};
            __m512i cells = next_cells(upper, middle, lower);
            _mm512_storeu_si512(after + at, cells);
            any = _mm512_or_si512(any, cells);
            if (count_population) population = _mm512_add_epi32(population, _mm512_popcnt_epi32(cells));
            up = middle;
            middle = south;
        }
        if (begin > 1 && after[offset + begin * 16]) active->first = begin - 1;
        if (end + 2 < stride / 16 && after[offset + end * 16 + 15]) active->last = end + 1;
        if (column == first && column > 1 && _mm512_test_epi32_mask(any, _mm512_set1_epi32(1)))
            active->first_block = column - 1;
        if (column == last && column < columns && _mm512_movepi32_mask(any)) active->last_block = column + 1;
    }
    return (uint64_t)_mm512_reduce_add_epi32(population);
}
