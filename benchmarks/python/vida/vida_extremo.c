/* Cada bit representa una celda. Las sumas lógicas calculan ocho vecinos
   a la vez para todas las celdas de un bloque de 512 bits. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <stdint.h>

#ifndef __AVX512VPOPCNTDQ__
#error Este banco requiere AVX-512VPOPCNTDQ
#endif

enum { SEED_SIDE = 64, VECTOR_BITS = 512, VECTOR_WORDS = 8 };

typedef struct {
    __m512i ones, twos;
} BitSum;

static inline BitSum add_three(__m512i a, __m512i b, __m512i c) {
    __m512i different = _mm512_xor_si512(a, b);
    return (BitSum){_mm512_xor_si512(different, c),
                    _mm512_or_si512(_mm512_and_si512(a, b), _mm512_and_si512(different, c))};
}

static inline __m512i from_left(const uint64_t *word, __m512i center, size_t stride) {
    __m512i before = _mm512_loadu_si512(word - stride);
    return _mm512_or_si512(_mm512_slli_epi64(center, 1), _mm512_srli_epi64(before, 63));
}

static inline __m512i from_right(const uint64_t *word, __m512i center, size_t stride) {
    __m512i after = _mm512_loadu_si512(word + stride);
    return _mm512_or_si512(_mm512_srli_epi64(center, 1), _mm512_slli_epi64(after, 63));
}

typedef struct {
#ifdef TZ_VIDA_REUSO
    __m512i center, odd, carry;
    BitSum triad;
#else
    __m512i center, left, right;
#endif
} Sides;

static inline Sides sides_at(const uint64_t *word, size_t stride) {
    __m512i center = _mm512_loadu_si512(word);
#ifdef TZ_VIDA_REUSO
    __m512i left = from_left(word, center, stride), right = from_right(word, center, stride);
    __m512i odd = _mm512_xor_si512(left, right), carry = _mm512_and_si512(left, right);
    return (Sides){center, odd, carry, {_mm512_xor_si512(odd, center),
                   _mm512_or_si512(carry, _mm512_and_si512(odd, center))}};
#else
    return (Sides){center, from_left(word, center, stride), from_right(word, center, stride)};
#endif
}

static inline Sides sides(const uint64_t *word) {
    return sides_at(word, 1);
}

static inline BitSum neighbours(Sides row) {
#ifdef TZ_VIDA_REUSO
    return row.triad;
#else
    return add_three(row.left, row.center, row.right);
#endif
}

static inline __m512i next_cells(BitSum top, Sides middle, BitSum bottom) {
#ifdef TZ_VIDA_REUSO
    BitSum low = add_three(top.ones, middle.odd, bottom.ones);
    __m512i mid_carry = middle.carry;
#else
    __m512i left = middle.left, right = middle.right;
    BitSum low = add_three(top.ones, _mm512_xor_si512(left, right), bottom.ones);
    __m512i mid_carry = _mm512_and_si512(left, right);
#endif
    __m512i first_pair = _mm512_xor_si512(top.twos, bottom.twos);
    __m512i second_pair = _mm512_xor_si512(mid_carry, low.twos);
    __m512i twos = _mm512_xor_si512(first_pair, second_pair);
    __m512i fours = _mm512_xor_si512(_mm512_and_si512(top.twos, bottom.twos),
                                   _mm512_and_si512(mid_carry, low.twos));
    fours = _mm512_xor_si512(fours, _mm512_and_si512(first_pair, second_pair));
    return _mm512_and_si512(_mm512_andnot_si512(fours, twos), _mm512_or_si512(low.ones, middle.center));
}

typedef struct {
    unsigned first, last;
    size_t first_block, last_block;
} Active;

#ifdef TZ_VIDA_TRIADAS
static uint64_t step(const uint64_t *restrict before, uint64_t *restrict after,
                     size_t stride, Active *active, int count_population) {
    const unsigned begin = active->first - 1, end = active->last + 2;
    const size_t columns = (stride - 2) / VECTOR_WORDS;
    const size_t first = active->first_block, last = active->last_block;
    __m512i population = _mm512_setzero_si512();
    for (size_t block = first; block <= last; block++) {
        size_t offset = 1 + block * VECTOR_WORDS;
        Sides middle = sides(before + (size_t)begin * stride + offset);
        BitSum up = neighbours(sides(before + (size_t)(begin - 1) * stride + offset));
        __m512i any = _mm512_setzero_si512();
        for (unsigned row = begin; row < end; row++) {
            size_t at = (size_t)row * stride + offset;
            Sides south = sides(before + at + stride);
            BitSum down = neighbours(south);
            __m512i cells = next_cells(up, middle, down);
            _mm512_storeu_si512(after + at, cells);
            any = _mm512_or_si512(any, cells);
            if (count_population) population = _mm512_add_epi64(population, _mm512_popcnt_epi64(cells));
            if ((row < active->first || row > active->last) && _mm512_test_epi64_mask(cells, cells)) {
                if (row < active->first) active->first = row;
                if (row > active->last) active->last = row;
            }
            up = neighbours(middle);
            middle = south;
        }
        if (block == first && block > 0 && (_mm_cvtsi128_si64(_mm512_castsi512_si128(any)) & 1))
            active->first_block = block - 1;
        if (block == last && block + 1 < columns && (_mm512_movepi64_mask(any) & 128)) active->last_block = block + 1;
    }
    return (uint64_t)_mm512_reduce_add_epi64(population);
}
#else
static uint64_t step(const uint64_t *restrict before, uint64_t *restrict after,
                     size_t stride, unsigned begin, unsigned end, Active *active) {
#ifdef TZ_VIDA_FILAS
    begin = active->first - 1;
    end = active->last + 2;
    size_t first_word = 0, last_word = stride - 2;
#else
    size_t first_word = (begin / VECTOR_BITS) * VECTOR_WORDS;
    size_t last_word = ((end + VECTOR_BITS - 1) / VECTOR_BITS) * VECTOR_WORDS;
#endif
    __m512i population = _mm512_setzero_si512();
    for (unsigned row = begin; row < end; row++) {
        /* La primera y la última palabra de cada fila son ceros de borde. */
        const uint64_t *middle = before + (size_t)row * stride + 1;
        uint64_t *out = after + (size_t)row * stride + 1;
        __m512i nonzero = _mm512_setzero_si512();
        for (size_t word = first_word; word < last_word; word += VECTOR_WORDS) {
            const uint64_t *here = middle + word;
            __m512i cells = next_cells(neighbours(sides(here - stride)), sides(here),
                                       neighbours(sides(here + stride)));
            _mm512_storeu_si512(out + word, cells);
            population = _mm512_add_epi64(population, _mm512_popcnt_epi64(cells));
            nonzero = _mm512_or_si512(nonzero, cells);
        }
#ifdef TZ_VIDA_FILAS
        if (_mm512_test_epi64_mask(nonzero, nonzero)) {
            if (row < active->first) active->first = row;
            if (row > active->last) active->last = row;
        }
#endif
    }
    return (uint64_t)_mm512_reduce_add_epi64(population);
}
#endif

#ifdef TZ_VIDA_SEMILLA
#include "vida_semilla.h"
#endif

#ifdef TZ_VIDA_COLUMNAS
#include "vida_columnas.h"
#endif

int main(int argc, char **argv) {
    long requested = argc > 1 ? strtol(argv[1], NULL, 10) : 50;
    if (requested > 8192) return EXIT_FAILURE;
    unsigned generations = requested > 0 ? (unsigned)requested : 0;
    unsigned origin = generations + 1;
    unsigned side = SEED_SIDE + 2 * origin;
#ifdef TZ_VIDA_COLUMNAS
    size_t stride = ((side + 7) / 8 + 2) * 8;
    size_t columns = (side + 63) / 64;
    size_t words = (columns + 2) * stride;
#else
    size_t stride = ((side + VECTOR_BITS - 1) / VECTOR_BITS) * VECTOR_WORDS + 2;
    size_t words = side * stride;
#endif
    double start = c_extremo_inicio();
    uint64_t *storage = calloc(2 * words, sizeof(*storage));
    if (!storage) return EXIT_FAILURE;
    uint64_t *live = storage, *buffer = storage + words;
#ifdef TZ_VIDA_COLUMNAS
    Active active = {(origin + 7) / 8, (origin + SEED_SIDE + 8) / 8,
                     1 + (origin - 1) / 64, 1 + (origin + SEED_SIDE) / 64};
#else
    Active active = {origin, origin + SEED_SIDE - 1,
                     (origin - 1) / VECTOR_BITS, (origin + SEED_SIDE) / VECTOR_BITS};
#endif
#ifdef TZ_VIDA_SEMILLA
    uint64_t count = seed_rows(live, stride, origin);
#else
    uint32_t seed = 7;
    uint64_t count = 0;
    for (unsigned x = origin; x < origin + SEED_SIDE; x++) {
        for (unsigned y = origin; y < origin + SEED_SIDE; y++) {
            seed = (seed * UINT32_C(1103515245) + UINT32_C(12345)) & UINT32_C(0x7fffffff);
            if ((seed >> 16) % 3 == 0) {
                live[(size_t)x * stride + 1 + y / 64] |= UINT64_C(1) << (y % 64);
                count++;
            }
        }
    }
#endif
    for (unsigned g = 0; g < generations; g++) {
        if (g % 10 == 0) printf("%u %llu\n", g, (unsigned long long)count);
#if defined(TZ_VIDA_COLUMNAS)
        count = step_columns(live, buffer, stride, columns, &active, (g + 1) % 10 == 0 || g + 1 == generations);
#elif defined(TZ_VIDA_TRIADAS)
        count = step(live, buffer, stride, &active, (g + 1) % 10 == 0 || g + 1 == generations);
#else
        count = step(live, buffer, stride, origin - g - 1, origin + SEED_SIDE + g + 1, &active);
#endif
        uint64_t *swap = live;
        live = buffer;
        buffer = swap;
    }
    printf("%ld %llu\n", requested, (unsigned long long)count);
    free(storage);
    c_extremo_fin(start);
    return EXIT_SUCCESS;
}
