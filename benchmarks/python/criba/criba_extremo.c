/* Criba sobre impares empaquetados. Una rueda marca los primeros divisores
   y AVX-512 cuenta y suma los primos sin enumerar cada bit vivo. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TZ_CRIBA_COCIENTES
#include <math.h>
typedef struct { uint64_t size, total; } PrimeTotals;

#ifdef TZ_CRIBA_PARES
typedef uint64_t SmallTotals;
static inline SmallTotals pack_small(PrimeTotals cell) { return (cell.total << 32) | cell.size; }
static inline PrimeTotals unpack_small(SmallTotals cell) { return (PrimeTotals){cell & UINT32_MAX, cell >> 32}; }
#else
typedef PrimeTotals SmallTotals;
static inline SmallTotals pack_small(PrimeTotals cell) { return cell; }
static inline PrimeTotals unpack_small(SmallTotals cell) { return cell; }
#endif

#ifdef TZ_CRIBA_RUEDA_COCIENTES
static PrimeTotals wheel_totals(uint64_t n, const PrimeTotals *prefix) {
    uint64_t cycles = n / 210;
    PrimeTotals tail = prefix[n % 210];
    unsigned small_count = n >= 7 ? 4 : n >= 5 ? 3 : n >= 3 ? 2 : n >= 2;
    unsigned small_sum = n >= 7 ? 17 : n >= 5 ? 10 : n >= 3 ? 5 : n >= 2 ? 2 : 0;
    return (PrimeTotals){48 * cycles + tail.size - 1 + small_count,
                         5040 * cycles * cycles + 210 * cycles * tail.size + tail.total - 1 + small_sum};
}

#ifdef TZ_CRIBA_RUEDA_VECTORIAL
static void prepare_small(SmallTotals *small, uint64_t root, const PrimeTotals *prefix) {
    static const uint64_t wheel[] = {
        UINT64_C(0), UINT64_C(4294967297), UINT64_C(4294967297), UINT64_C(4294967297), UINT64_C(4294967297), UINT64_C(4294967297),
        UINT64_C(4294967297), UINT64_C(4294967297), UINT64_C(4294967297), UINT64_C(4294967297), UINT64_C(4294967297), UINT64_C(51539607554),
        UINT64_C(51539607554), UINT64_C(107374182403), UINT64_C(107374182403), UINT64_C(107374182403), UINT64_C(107374182403), UINT64_C(180388626436),
        UINT64_C(180388626436), UINT64_C(261993005061), UINT64_C(261993005061), UINT64_C(261993005061), UINT64_C(261993005061), UINT64_C(360777252870),
        UINT64_C(360777252870), UINT64_C(360777252870), UINT64_C(360777252870), UINT64_C(360777252870), UINT64_C(360777252870), UINT64_C(485331304455),
        UINT64_C(485331304455), UINT64_C(618475290632), UINT64_C(618475290632), UINT64_C(618475290632), UINT64_C(618475290632), UINT64_C(618475290632),
        UINT64_C(618475290632), UINT64_C(777389080585), UINT64_C(777389080585), UINT64_C(777389080585), UINT64_C(777389080585), UINT64_C(953482739722),
        UINT64_C(953482739722), UINT64_C(1138166333451), UINT64_C(1138166333451), UINT64_C(1138166333451), UINT64_C(1138166333451), UINT64_C(1340029796364),
        UINT64_C(1340029796364), UINT64_C(1340029796364), UINT64_C(1340029796364), UINT64_C(1340029796364), UINT64_C(1340029796364), UINT64_C(1567663063053),
        UINT64_C(1567663063053), UINT64_C(1567663063053), UINT64_C(1567663063053), UINT64_C(1567663063053), UINT64_C(1567663063053), UINT64_C(1821066133518),
        UINT64_C(1821066133518), UINT64_C(2083059138575), UINT64_C(2083059138575), UINT64_C(2083059138575), UINT64_C(2083059138575), UINT64_C(2083059138575),
        UINT64_C(2083059138575), UINT64_C(2370821947408), UINT64_C(2370821947408), UINT64_C(2370821947408), UINT64_C(2370821947408), UINT64_C(2675764625425),
        UINT64_C(2675764625425), UINT64_C(2989297238034), UINT64_C(2989297238034), UINT64_C(2989297238034), UINT64_C(2989297238034), UINT64_C(2989297238034),
        UINT64_C(2989297238034), UINT64_C(3328599654419), UINT64_C(3328599654419), UINT64_C(3328599654419), UINT64_C(3328599654419), UINT64_C(3685081939988),
        UINT64_C(3685081939988), UINT64_C(3685081939988), UINT64_C(3685081939988), UINT64_C(3685081939988), UINT64_C(3685081939988), UINT64_C(4067334029333),
        UINT64_C(4067334029333), UINT64_C(4067334029333), UINT64_C(4067334029333), UINT64_C(4067334029333), UINT64_C(4067334029333), UINT64_C(4067334029333),
        UINT64_C(4067334029333), UINT64_C(4483945857046), UINT64_C(4483945857046), UINT64_C(4483945857046), UINT64_C(4483945857046), UINT64_C(4917737553943),
        UINT64_C(4917737553943), UINT64_C(5360119185432), UINT64_C(5360119185432), UINT64_C(5360119185432), UINT64_C(5360119185432), UINT64_C(5819680686105),
        UINT64_C(5819680686105), UINT64_C(6287832121370), UINT64_C(6287832121370), UINT64_C(6287832121370), UINT64_C(6287832121370), UINT64_C(6773163425819),
        UINT64_C(6773163425819), UINT64_C(6773163425819), UINT64_C(6773163425819), UINT64_C(6773163425819), UINT64_C(6773163425819), UINT64_C(6773163425819),
        UINT64_C(6773163425819), UINT64_C(7292854468636), UINT64_C(7292854468636), UINT64_C(7292854468636), UINT64_C(7292854468636), UINT64_C(7292854468636),
        UINT64_C(7292854468636), UINT64_C(7838315315229), UINT64_C(7838315315229), UINT64_C(7838315315229), UINT64_C(7838315315229), UINT64_C(8400956031006),
        UINT64_C(8400956031006), UINT64_C(8400956031006), UINT64_C(8400956031006), UINT64_C(8400956031006), UINT64_C(8400956031006), UINT64_C(8989366550559),
        UINT64_C(8989366550559), UINT64_C(9586367004704), UINT64_C(9586367004704), UINT64_C(9586367004704), UINT64_C(9586367004704), UINT64_C(10200547328033),
        UINT64_C(10200547328033), UINT64_C(10200547328033), UINT64_C(10200547328033), UINT64_C(10200547328033), UINT64_C(10200547328033), UINT64_C(10840497455138),
        UINT64_C(10840497455138), UINT64_C(11489037516835), UINT64_C(11489037516835), UINT64_C(11489037516835), UINT64_C(11489037516835), UINT64_C(11489037516835),
        UINT64_C(11489037516835), UINT64_C(12163347382308), UINT64_C(12163347382308), UINT64_C(12163347382308), UINT64_C(12163347382308), UINT64_C(12163347382308),
        UINT64_C(12163347382308), UINT64_C(12863427051557), UINT64_C(12863427051557), UINT64_C(12863427051557), UINT64_C(12863427051557), UINT64_C(13580686589990),
        UINT64_C(13580686589990), UINT64_C(14306536063015), UINT64_C(14306536063015), UINT64_C(14306536063015), UINT64_C(14306536063015), UINT64_C(15049565405224),
        UINT64_C(15049565405224), UINT64_C(15049565405224), UINT64_C(15049565405224), UINT64_C(15049565405224), UINT64_C(15049565405224), UINT64_C(15818364551209),
        UINT64_C(15818364551209), UINT64_C(16595753631786), UINT64_C(16595753631786), UINT64_C(16595753631786), UINT64_C(16595753631786), UINT64_C(16595753631786),
        UINT64_C(16595753631786), UINT64_C(17398912516139), UINT64_C(17398912516139), UINT64_C(17398912516139), UINT64_C(17398912516139), UINT64_C(18219251269676),
        UINT64_C(18219251269676), UINT64_C(19048179957805), UINT64_C(19048179957805), UINT64_C(19048179957805), UINT64_C(19048179957805), UINT64_C(19894288515118),
        UINT64_C(19894288515118), UINT64_C(20748987007023), UINT64_C(20748987007023), UINT64_C(20748987007023), UINT64_C(20748987007023), UINT64_C(20748987007023),
        UINT64_C(20748987007023), UINT64_C(20748987007023), UINT64_C(20748987007023), UINT64_C(20748987007023), UINT64_C(20748987007023), UINT64_C(21646635171888),
    };
    for (uint64_t value = 1; value <= root && value < 210; value++)
        small[value] = pack_small(wheel_totals(value, prefix));
    for (uint64_t cycle = 1; cycle <= root / 210; cycle++) {
        uint64_t base = cycle * 210, width = root + 1 - base;
        if (width > 210) width = 210;
        uint64_t bias = ((5040 * cycle * cycle + 16) << 32) | (48 * cycle + 3);
        uint64_t shift = base << 32, i = 0;
        __m512i biases = _mm512_set1_epi64(bias), shifts = _mm512_set1_epi64(shift);
        __m512i keep_count = _mm512_set1_epi64(UINT32_MAX);
        for (; i + 8 <= width; i += 8) {
            __m512i packed = _mm512_loadu_si512(wheel + i);
            __m512i low = _mm512_and_si512(packed, keep_count);
            __m512i corrected = _mm512_add_epi64(packed, biases);
            _mm512_storeu_si512(small + base + i, _mm512_add_epi64(corrected, _mm512_mullo_epi64(low, shifts)));
        }
        for (; i < width; i++)
            small[base + i] = wheel[i] + bias + (wheel[i] & UINT32_MAX) * shift;
    }
}
#endif

#ifdef TZ_CRIBA_DIVISORES
static unsigned divisor_rank(uint64_t n, const PrimeTotals *prefix) {
    return (unsigned)(48 * (n / 210) + prefix[n % 210].size);
}
#endif
#endif

static PrimeTotals exclude(PrimeTotals out, PrimeTotals src, PrimeTotals before, uint64_t p) {
    return (PrimeTotals){out.size - (src.size - before.size), out.total - p * (src.total - before.total)};
}

int main(int argc, char **argv) {
    uint64_t requested = argc > 1 ? strtoull(argv[1], NULL, 10) : 1000000;
    if (requested < 2 || requested > (UINT64_C(1) << 30)) return EXIT_FAILURE;
    double start = c_extremo_inicio();
    uint64_t n = requested - 1;
    uint64_t root = (uint64_t)sqrt((double)n), high = n / (root + 1);
    SmallTotals *small = calloc(1, (root + 1) * sizeof(*small) + (high + 1) * sizeof(PrimeTotals));
    if (!small) return EXIT_FAILURE;
    PrimeTotals *big = (PrimeTotals *)(small + root + 1);
#ifdef TZ_CRIBA_RECIPROCOS
    double *inverse = malloc((root + 8) * sizeof(*inverse));
    if (!inverse) {
        free(small);
        return EXIT_FAILURE;
    }
#endif
#ifdef TZ_CRIBA_RUEDA_COCIENTES
    PrimeTotals prefix[210] = {{0, 0}};
#ifdef TZ_CRIBA_DIVISORES
    uint16_t residues[48];
#endif
    uint64_t count = 0, total = 0;
    for (unsigned j = 1; j < 210; j++) {
        if (j % 2 && j % 3 && j % 5 && j % 7) {
#ifdef TZ_CRIBA_DIVISORES
            residues[count] = (uint16_t)j;
#endif
            count++;
            total += j;
        }
        prefix[j] = (PrimeTotals){count, total};
    }
#ifdef TZ_CRIBA_DIVISORES
    const unsigned prime_end = divisor_rank(root, prefix), big_end = divisor_rank(high, prefix);
    const unsigned first_prime = 2;
    uint16_t *positions = malloc((prime_end + 1) * sizeof(*positions));
    if (!positions) {
        free(inverse);
        free(small);
        return EXIT_FAILURE;
    }
    for (unsigned rank = 1; rank <= prime_end; rank++) {
        unsigned index = rank - 1;
        positions[rank] = (uint16_t)(210 * (index / 48) + residues[index % 48]);
    }
#else
    const uint64_t first_prime = 11, prime_end = root, big_end = high;
#endif
#ifdef TZ_CRIBA_RUEDA_VECTORIAL
    prepare_small(small, root, prefix);
#else
    for (uint64_t value = 1; value <= root; value++)
        small[value] = pack_small(wheel_totals(value, prefix));
#endif
#else
    const uint64_t first_prime = 2, prime_end = root, big_end = high;
    for (uint64_t value = 1; value <= root; value++)
        small[value] = pack_small((PrimeTotals){value - 1, value * (value + 1) / 2 - 1});
#endif
#ifdef TZ_CRIBA_RECIPROCOS_SIMD
    const __m512d lanes = _mm512_setr_pd(0,1,2,3,4,5,6,7), one = _mm512_set1_pd(1.0);
    for (uint64_t i = 1; i <= root; i += 8) {
        __m512d number = _mm512_add_pd(lanes, _mm512_set1_pd((double)i));
        __m512i bits = _mm512_add_epi64(_mm512_castpd_si512(_mm512_div_pd(one, number)), _mm512_set1_epi64(1));
        _mm512_storeu_pd(inverse + i, _mm512_castsi512_pd(bits));
    }
#endif
    for (uint64_t rank = 1; rank <= big_end; rank++) {
#ifdef TZ_CRIBA_DIVISORES
        uint64_t i = positions[rank];
#else
        uint64_t i = rank;
#endif
#if defined(TZ_CRIBA_RECIPROCOS_SIMD)
        uint64_t value = (uint64_t)((double)n * inverse[i]);
#elif defined(TZ_CRIBA_RECIPROCOS)
        /* Subir el recíproco un ulp alcanza los cocientes enteros. Para
           un cociente fraccionario, el error escalado por i es menor que 1. */
        union { double f; uint64_t bits; } up = {1.0 / (double)i};
        up.bits++;
        inverse[i] = up.f;
        uint64_t value = (uint64_t)((double)n * up.f);
#else
        uint64_t value = n / i;
#endif
#ifdef TZ_CRIBA_RUEDA_COCIENTES
#if defined(TZ_CRIBA_DIVISORES) && !defined(TZ_CRIBA_DIVISORES_DENSOS)
        big[rank] = wheel_totals(value, prefix);
#else
        big[i] = wheel_totals(value, prefix);
#endif
#else
        big[i] = (PrimeTotals){value - 1, value * (value + 1) / 2 - 1};
#endif
    }
    for (uint64_t rank = first_prime; rank <= prime_end; rank++) {
#ifdef TZ_CRIBA_DIVISORES
        uint64_t p = positions[rank];
#else
        uint64_t p = rank;
#endif
        PrimeTotals before = unpack_small(small[p - 1]);
        if (unpack_small(small[p]).size == before.size) continue;
        uint64_t square = p * p;
#ifdef TZ_CRIBA_RECIPROCOS_SIMD
        double recip = inverse[p];
        double scaled = (double)(uint64_t)((double)n * recip);
        uint64_t last = (uint64_t)(scaled * recip);
        uint64_t split = (uint64_t)((double)high * recip);
#else
        uint64_t last = n / square;
        uint64_t split = high / p;
#endif
        if (last > high) last = high;
        if (split > last) split = last;
#ifdef TZ_CRIBA_DIVISORES
        unsigned first_end = divisor_rank(split, prefix), last_end = divisor_rank(last, prefix);
        for (unsigned index = 1; index <= first_end; index++) {
            uint64_t i = positions[index];
#ifdef TZ_CRIBA_DIVISORES_DENSOS
            big[i] = exclude(big[i], big[i * p], before, p);
#else
            big[index] = exclude(big[index], big[divisor_rank(i * p, prefix)], before, p);
#endif
        }
        for (unsigned index = first_end + 1; index <= last_end; index++) {
            uint64_t i = positions[index];
            PrimeTotals previous = unpack_small(small[(uint64_t)(scaled * inverse[i])]);
#ifdef TZ_CRIBA_DIVISORES_DENSOS
            big[i] = exclude(big[i], previous, before, p);
#else
            big[index] = exclude(big[index], previous, before, p);
#endif
        }
#else
        for (uint64_t i = 1; i <= split; i++) big[i] = exclude(big[i], big[i * p], before, p);
#ifdef TZ_CRIBA_RECIPROCOS
#ifndef TZ_CRIBA_RECIPROCOS_SIMD
        double scaled = (double)(n / p);
#endif
        for (uint64_t i = split + 1; i <= last; i++)
            big[i] = exclude(big[i], unpack_small(small[(uint64_t)(scaled * inverse[i])]), before, p);
#else
        for (uint64_t i = split + 1; i <= last; i++)
            big[i] = exclude(big[i], unpack_small(small[n / (i * p)]), before, p);
#endif
#endif
        if (root >= square) {
#ifdef TZ_CRIBA_RECIPROCOS_SIMD
            uint64_t quotient = (uint64_t)((double)root * recip);
#else
            uint64_t quotient = root / p;
#endif
            uint64_t boundary = quotient * p;
#ifdef TZ_CRIBA_GRUPOS
            uint64_t value = root;
            while (value >= square) {
                uint64_t first = boundary > square ? boundary : square;
                PrimeTotals source = unpack_small(small[quotient]);
                uint64_t count = source.size - before.size, total = p * (source.total - before.total);
#ifdef TZ_CRIBA_PARES
                uint64_t delta = (total << 32) | count;
                uint64_t at = first;
#ifdef TZ_CRIBA_PARES_SIMD
                __m512i amount = _mm512_set1_epi64((int64_t)delta);
                for (; at + 7 <= value; at += 8) {
                    __m512i cells = _mm512_loadu_si512(small + at);
                    _mm512_storeu_si512(small + at, _mm512_sub_epi64(cells, amount));
                }
#endif
                for (; at <= value; at++) small[at] -= delta;
#else
                for (uint64_t at = first; at <= value; at++) {
                    small[at].size -= count;
                    small[at].total -= total;
                }
#endif
                value = first - 1;
                quotient--;
                boundary -= p;
            }
#else
            for (uint64_t value = root; value >= square; value--) {
                small[value] = pack_small(exclude(unpack_small(small[value]), unpack_small(small[quotient]), before, p));
                if (value == boundary) {
                    quotient--;
                    boundary -= p;
                }
            }
#endif
        }
    }
    PrimeTotals result = high ? big[1] : unpack_small(small[n]);
    printf("%llu %llu\n", (unsigned long long)result.size, (unsigned long long)result.total);
#ifdef TZ_CRIBA_DIVISORES
    free(positions);
#endif
#ifdef TZ_CRIBA_RECIPROCOS
    free(inverse);
#endif
    free(small);
    c_extremo_fin(start);
    return EXIT_SUCCESS;
}
#else

#if !defined(__AVX512VPOPCNTDQ__) || !defined(__AVX512DQ__)
#error Este banco requiere AVX-512DQ y AVX-512VPOPCNTDQ
#endif

static const uint64_t small_primes[] = {3, 5, 7, 11, 13};
enum { WHEEL_WORDS = 3 * 5 * 7 * 11 * 13 };

static void fill_wheel(uint64_t *composite, size_t words) {
    uint64_t wheel[WHEEL_WORDS] = {0};
    for (size_t i = 0; i < sizeof(small_primes) / sizeof(*small_primes); i++) {
        uint64_t p = small_primes[i];
        for (uint64_t k = p / 2; k < 64 * WHEEL_WORDS; k += p) {
            wheel[k >> 6] |= UINT64_C(1) << (k & 63);
        }
    }
    for (size_t i = 0; i < words; i += WHEEL_WORDS) {
        size_t remaining = words - i;
        size_t count = remaining < WHEEL_WORDS ? remaining : WHEEL_WORDS;
        memcpy(composite + i, wheel, count * sizeof(*wheel));
    }
    composite[0] |= 1;
}

static void count_and_sum(const uint64_t *composite, size_t words,
                          uint64_t *count, uint64_t *total) {
    static const uint64_t position_bits[] = {
        UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0xcccccccccccccccc),
        UINT64_C(0xf0f0f0f0f0f0f0f0), UINT64_C(0xff00ff00ff00ff00),
        UINT64_C(0xffff0000ffff0000), UINT64_C(0xffffffff00000000)
    };
    __m512i count_vec = _mm512_setzero_si512();
    __m512i sum_vec = _mm512_setzero_si512();
    __m512i first = _mm512_setr_epi64(1, 129, 257, 385, 513, 641, 769, 897);
    size_t i = 0;
    for (; i + 8 <= words; i += 8) {
        __m512i primes = _mm512_xor_si512(_mm512_loadu_si512(composite + i), _mm512_set1_epi64(-1));
        __m512i sizes = _mm512_popcnt_epi64(primes);
        __m512i positions = _mm512_setzero_si512();
        for (int bit = 0; bit < 6; bit++) {
            __m512i selected = _mm512_and_si512(primes, _mm512_set1_epi64((int64_t)position_bits[bit]));
            __m512i set = _mm512_popcnt_epi64(selected);
            positions = _mm512_add_epi64(positions, _mm512_sllv_epi64(set, _mm512_set1_epi64(bit + 1)));
        }
        count_vec = _mm512_add_epi64(count_vec, sizes);
        sum_vec = _mm512_add_epi64(sum_vec, _mm512_add_epi64(_mm512_mullo_epi64(first, sizes), positions));
        first = _mm512_add_epi64(first, _mm512_set1_epi64(1024));
    }
    *count += (uint64_t)_mm512_reduce_add_epi64(count_vec);
    *total += (uint64_t)_mm512_reduce_add_epi64(sum_vec);
    for (; i < words; i++) {
        uint64_t bits = ~composite[i];
        while (bits) {
            *total += 2 * (64 * i + (unsigned)__builtin_ctzll(bits)) + 1;
            (*count)++;
            bits &= bits - 1;
        }
    }
}

int main(int argc, char **argv) {
    uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : 1000000;
    if (n < 2 || n > (UINT64_C(1) << 30)) return EXIT_FAILURE;
    double start = c_extremo_inicio();
    uint64_t odds = n / 2;
    size_t words = (size_t)((odds + 63) / 64);
    uint64_t *composite = malloc(words * sizeof(*composite));
    if (!composite) return EXIT_FAILURE;
    fill_wheel(composite, words);
    for (uint64_t p = 17; p * p < n; p += 2) {
        uint64_t at = p / 2;
        if (composite[at >> 6] & (UINT64_C(1) << (at & 63))) continue;
        for (uint64_t k = p * p / 2; k < odds; k += p) {
            composite[k >> 6] |= UINT64_C(1) << (k & 63);
        }
    }
    if (odds & 63) composite[words - 1] |= UINT64_MAX << (odds & 63);
    uint64_t count = n > 2 ? 1 : 0;
    uint64_t total = n > 2 ? 2 : 0;
    for (size_t i = 0; i < sizeof(small_primes) / sizeof(*small_primes); i++) {
        uint64_t p = small_primes[i];
        if (p < n) {
            count++;
            total += p;
        }
    }
    count_and_sum(composite, words, &count, &total);
    printf("%llu %llu\n", (unsigned long long)count, (unsigned long long)total);
    free(composite);
    c_extremo_fin(start);
    return EXIT_SUCCESS;
}
#endif
