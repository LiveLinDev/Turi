/* Ocho filas por vector y una columna de palabras por recorrido. */
static uint64_t step_columns(const uint64_t *restrict before, uint64_t *restrict after,
                             size_t stride, size_t columns, Active *active, int count_population) {
    unsigned begin = active->first, end = active->last;
    size_t first = active->first_block, last = active->last_block;
    __m512i population = _mm512_setzero_si512();
    const __m512i top = _mm512_setr_epi64(7, 8, 9, 10, 11, 12, 13, 14);
    const __m512i bottom = _mm512_setr_epi64(1, 2, 3, 4, 5, 6, 7, 8);
    for (size_t column = first; column <= last; column++) {
        size_t offset = column * stride;
        Sides up = sides_at(before + offset + (begin - 1) * 8, stride);
        Sides middle = sides_at(before + offset + begin * 8, stride);
        __m512i any = _mm512_setzero_si512();
        for (unsigned row = begin; row <= end; row++) {
            size_t at = offset + row * 8;
            Sides south = sides_at(before + at + 8, stride);
            BitSum upper = {_mm512_permutex2var_epi64(up.triad.ones, top, middle.triad.ones),
                            _mm512_permutex2var_epi64(up.triad.twos, top, middle.triad.twos)};
            BitSum lower = {_mm512_permutex2var_epi64(middle.triad.ones, bottom, south.triad.ones),
                            _mm512_permutex2var_epi64(middle.triad.twos, bottom, south.triad.twos)};
            __m512i cells = next_cells(upper, middle, lower);
            _mm512_storeu_si512(after + at, cells);
            any = _mm512_or_si512(any, cells);
            if (count_population) population = _mm512_add_epi64(population, _mm512_popcnt_epi64(cells));
            if (row == begin && begin > 1 && (_mm512_test_epi64_mask(cells, cells) & 1)) active->first = begin - 1;
            if (row == end && end + 2 < stride / 8 && (_mm512_test_epi64_mask(cells, cells) & 128)) active->last = end + 1;
            up = middle;
            middle = south;
        }
        if (column == first && column > 1 && _mm512_test_epi64_mask(any, _mm512_set1_epi64(1)))
            active->first_block = column - 1;
        if (column == last && column < columns && _mm512_movepi64_mask(any)) active->last_block = column + 1;
    }
    return (uint64_t)_mm512_reduce_add_epi64(population);
}
