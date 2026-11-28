/* El generador produce como máximo tres sílabas. Se cuentan sus índices
   directamente y se forman las cadenas al ordenar los resultados. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <stdint.h>
#include <string.h>

#ifndef __SSE4_1__
#error Este banco requiere SSE4.1
#endif

#define MULTIPLIER UINT32_C(1103515245)
#define INCREMENT UINT32_C(12345)

enum { SYLLABLES = 12, WORDS = 12 + 12 * 12 + 12 * 12 * 12 };
static const char syllables[SYLLABLES][3] = {
    "ka", "lo", "mi", "ne", "ru", "sa", "to", "vi", "ze", "po", "an", "el"
};

typedef struct {
    char word[7];
    uint64_t count;
} Entry;

static unsigned sample(__m128i states, int lane) {
    uint32_t values[4];
    _mm_storeu_si128((__m128i *)values, states);
    return (values[lane] >> 16) & 32767;
}

#ifdef TZ_PALABRAS_BLOQUE
/* Se generan ocho muestras adicionales para las transiciones del borde. */
static unsigned count_transition(uint64_t *counts, const uint32_t *codes,
                                 const uint32_t *jumps, unsigned at) {
#ifdef TZ_PALABRAS_SALTOS
    counts[codes[at]]++;
    return jumps[at];
#elif defined(TZ_PALABRAS_ABSOLUTO)
    uint32_t transition = codes[at];
    counts[transition >> 16]++;
    return transition & 65535;
#else
    uint32_t transition = codes[at];
    counts[transition & 65535]++;
    return at + (transition >> 16);
#endif
}

static void count_words(uint64_t *counts, long long n) {
    enum { BLOCK = TZ_PALABRAS_BLOQUE, LANES = 8 };
#ifdef TZ_PALABRAS_DOBLE
    enum { TRANSITIONS = BLOCK + LANES };
#else
    enum { TRANSITIONS = BLOCK };
#endif
    __m256i scales[(TRANSITIONS + LANES) / LANES], offsets[(TRANSITIONS + LANES) / LANES];
    uint32_t power = 1, add = 0;
    for (int k = 0; k <= TRANSITIONS / LANES; ++k) {
        uint32_t scale[LANES], bias[LANES];
        for (int lane = 0; lane < LANES; ++lane) {
            power *= MULTIPLIER;
            add = add * MULTIPLIER + INCREMENT;
            scale[lane] = power;
            bias[lane] = add;
        }
        scales[k] = _mm256_loadu_si256((const __m256i *)scale);
        offsets[k] = _mm256_loadu_si256((const __m256i *)bias);
    }
    uint32_t state = 42;
    const __m256i keep = _mm256_set1_epi32(32767), magic = _mm256_set1_epi32(10923);
    const __m256i radix = _mm256_set1_epi32(12), three = _mm256_set1_epi32(3);
    const __m256i two = _mm256_set1_epi32(2), one = _mm256_set1_epi32(1), zero = _mm256_setzero_si256();
    unsigned at = 0;
    while (n > 0) {
        uint32_t symbols[TRANSITIONS + LANES], lengths[TRANSITIONS];
#ifndef TZ_PALABRAS_PAR_32
        uint32_t transitions[TRANSITIONS], jumps[TRANSITIONS];
#endif
#ifdef TZ_PALABRAS_DOBLE
        uint32_t single[TRANSITIONS], pairs[BLOCK];
#ifndef TZ_PALABRAS_PAR_32
        uint32_t twice[BLOCK];
#endif
#endif
        __m256i seed = _mm256_set1_epi32((int32_t)state);
        for (int k = 0; k <= TRANSITIONS / LANES; ++k) {
            __m256i states = _mm256_add_epi32(_mm256_mullo_epi32(seed, scales[k]), offsets[k]);
            __m256i samples = _mm256_and_si256(_mm256_srli_epi32(states, 16), keep);
            __m256i product = _mm256_mullo_epi32(samples, magic);
            __m256i symbol = _mm256_sub_epi32(samples, _mm256_mullo_epi32(_mm256_srli_epi32(product, 17), radix));
            _mm256_storeu_si256((__m256i *)(symbols + k * LANES), symbol);
            if (k < TRANSITIONS / LANES) {
                __m256i length = _mm256_sub_epi32(samples, _mm256_mullo_epi32(_mm256_srli_epi32(product, 15), three));
                _mm256_storeu_si256((__m256i *)(lengths + k * LANES), length);
            }
            if (k == BLOCK / LANES - 1) state = (uint32_t)_mm256_extract_epi32(states, 7);
        }
        for (int k = 0; k < TRANSITIONS; k += LANES) {
            __m256i length = _mm256_loadu_si256((const __m256i *)(lengths + k));
            __m256i a = _mm256_loadu_si256((const __m256i *)(symbols + k + 1));
            __m256i b = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_add_epi32(a, one), radix),
                                         _mm256_loadu_si256((const __m256i *)(symbols + k + 2)));
            __m256i c = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_add_epi32(b, one), radix),
                                         _mm256_loadu_si256((const __m256i *)(symbols + k + 3)));
            __m256i code = _mm256_blendv_epi8(c, b, _mm256_cmpeq_epi32(length, one));
            code = _mm256_blendv_epi8(code, a, _mm256_cmpeq_epi32(length, zero));
#if defined(TZ_PALABRAS_SALTOS) || defined(TZ_PALABRAS_ABSOLUTO)
            const __m256i lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
            __m256i next = _mm256_add_epi32(length, _mm256_add_epi32(lanes, _mm256_set1_epi32(k + 2)));
#ifdef TZ_PALABRAS_SALTOS
#ifndef TZ_PALABRAS_PAR_32
            _mm256_storeu_si256((__m256i *)(jumps + k), next);
            _mm256_storeu_si256((__m256i *)(transitions + k), code);
#endif
#ifdef TZ_PALABRAS_DOBLE
            _mm256_storeu_si256((__m256i *)(single + k), _mm256_or_si256(code, _mm256_slli_epi32(next, 16)));
#endif
#else
            __m256i packed = _mm256_or_si256(_mm256_slli_epi32(code, 16), next);
            _mm256_storeu_si256((__m256i *)(transitions + k), packed);
#endif
#else
            __m256i packed = _mm256_or_si256(_mm256_slli_epi32(_mm256_add_epi32(length, two), 16), code);
            _mm256_storeu_si256((__m256i *)(transitions + k), packed);
#endif
        }
        if (n >= BLOCK / 2) {
#ifdef TZ_PALABRAS_DOBLE
            for (int k = 0; k < BLOCK; k += LANES) {
                __m256i length = _mm256_loadu_si256((const __m256i *)(lengths + k));
                __m256i a = _mm256_loadu_si256((const __m256i *)(single + k + 2));
                __m256i b = _mm256_loadu_si256((const __m256i *)(single + k + 3));
                __m256i c = _mm256_loadu_si256((const __m256i *)(single + k + 4));
                __m256i next = _mm256_blendv_epi8(c, b, _mm256_cmpeq_epi32(length, one));
                next = _mm256_blendv_epi8(next, a, _mm256_cmpeq_epi32(length, zero));
#ifdef TZ_PALABRAS_PAR_32
                const __m256i mask = _mm256_set1_epi32(2047);
                __m256i first = _mm256_loadu_si256((const __m256i *)(single + k));
                __m256i pair = _mm256_or_si256(_mm256_and_si256(first, mask),
                                              _mm256_slli_epi32(_mm256_and_si256(next, mask), 11));
                pair = _mm256_or_si256(pair, _mm256_slli_epi32(_mm256_srli_epi32(next, 16), 22));
#else
                __m256i first = _mm256_loadu_si256((const __m256i *)(transitions + k));
                __m256i pair = _mm256_or_si256(first, _mm256_slli_epi32(next, 16));
                _mm256_storeu_si256((__m256i *)(twice + k), _mm256_srli_epi32(next, 16));
#endif
                _mm256_storeu_si256((__m256i *)(pairs + k), pair);
            }
            while (at < BLOCK) {
                uint32_t pair = pairs[at];
#ifdef TZ_PALABRAS_PAR_32
                at = pair >> 22;
                counts[pair & 2047]++;
                counts[(pair >> 11) & 2047]++;
#else
                at = twice[at];
                counts[pair & 65535]++;
                counts[pair >> 16]++;
#endif
                n -= 2;
            }
#else
            while (at < BLOCK) {
                at = count_transition(counts, transitions, jumps, at);
                n--;
            }
#endif
        } else {
            while (at < BLOCK && n > 0) {
#ifdef TZ_PALABRAS_PAR_32
                uint32_t step = single[at];
                counts[step & 2047]++;
                at = step >> 16;
#else
                at = count_transition(counts, transitions, jumps, at);
#endif
                n--;
            }
        }
        at -= BLOCK;
    }
}
#else
static void count_words(uint64_t *counts, long long n) {
    /* Formas afines de uno, dos, tres y cuatro pasos del generador. */
    const uint32_t step2 = MULTIPLIER * MULTIPLIER;
    const uint32_t step3 = step2 * MULTIPLIER;
    const uint32_t step4 = step3 * MULTIPLIER;
    const uint32_t bias2 = MULTIPLIER * INCREMENT + INCREMENT;
    const uint32_t bias3 = MULTIPLIER * bias2 + INCREMENT;
    const uint32_t bias4 = MULTIPLIER * bias3 + INCREMENT;
    __m128i multiply = _mm_setr_epi32((int32_t)MULTIPLIER, (int32_t)step2, (int32_t)step3, (int32_t)step4);
    __m128i add = _mm_setr_epi32((int32_t)INCREMENT, (int32_t)bias2, (int32_t)bias3, (int32_t)bias4);
    int32_t seed = 42;
    for (long long i = 0; i < n; i++) {
        __m128i states = _mm_add_epi32(_mm_mullo_epi32(_mm_set1_epi32(seed), multiply), add);
#ifdef TZ_PALABRAS_SELECCION
        __m128i samples = _mm_and_si128(_mm_srli_epi32(states, 16), _mm_set1_epi32(32767));
        /* En [0, 32767], el producto por 10923 y el desplazamiento dan x / 12. */
        __m128i quotients = _mm_srli_epi32(_mm_mullo_epi32(samples, _mm_set1_epi32(10923)), 17);
        __m128i symbols = _mm_sub_epi32(samples, _mm_mullo_epi32(quotients, _mm_set1_epi32(12)));
        unsigned length = (unsigned)_mm_extract_epi32(samples, 0) % 3;
        unsigned first = (unsigned)_mm_extract_epi32(symbols, 1);
        unsigned second = 12 + first * 12 + (unsigned)_mm_extract_epi32(symbols, 2);
        unsigned third = 12 + second * 12 + (unsigned)_mm_extract_epi32(symbols, 3);
        unsigned index = length == 0 ? first : (length == 1 ? second : third);
        __m128 controls = _mm_castsi128_ps(states);
        __m128 next = _mm_permutevar_ps(controls, _mm_set1_epi32((int)(length + 1)));
        seed = _mm_cvtsi128_si32(_mm_castps_si128(next));
#else
        unsigned length = sample(states, 0) % 3 + 1;
        unsigned index = sample(states, 1) % SYLLABLES;
        if (length == 1) {
            seed = _mm_extract_epi32(states, 1);
        } else {
            index = SYLLABLES + index * SYLLABLES + sample(states, 2) % SYLLABLES;
            if (length == 2) {
                seed = _mm_extract_epi32(states, 2);
            } else {
                index = SYLLABLES + index * SYLLABLES + sample(states, 3) % SYLLABLES;
                seed = _mm_extract_epi32(states, 3);
            }
        }
#endif
        counts[index]++;
    }
}

#endif

static void decode(char word[7], unsigned index) {
    unsigned length = 1;
    for (unsigned block = SYLLABLES; index >= block; block *= SYLLABLES) {
        index -= block;
        length++;
    }
    word[2 * length] = '\0';
    while (length) {
        length--;
        const char *text = syllables[index % SYLLABLES];
        word[2 * length] = text[0];
        word[2 * length + 1] = text[1];
        index /= SYLLABLES;
    }
}

static int compare(const void *left, const void *right) {
    const Entry *a = left, *b = right;
    if (a->count != b->count) return a->count > b->count ? -1 : 1;
    return strcmp(a->word, b->word);
}

#ifdef TZ_PALABRAS_PRIMERAS
static inline unsigned keep_word(unsigned index, const uint64_t *counts, unsigned *best, uint64_t *scores) {
    uint64_t score = counts[index];
    if (!score) return 0;
    if (score > scores[9]) {
        size_t place = 9;
        while (place && score > scores[place - 1]) {
            best[place] = best[place - 1];
            scores[place] = scores[place - 1];
            place--;
        }
        best[place] = index;
        scores[place] = score;
    }
    return 1;
}
#endif

int main(int argc, char **argv) {
    long long n = argc > 1 ? strtoll(argv[1], NULL, 10) : 100000;
    double start = c_extremo_inicio();
    uint64_t counts[WORDS] = {0};
    count_words(counts, n);
#ifdef TZ_PALABRAS_PRIMERAS
    static const unsigned order[] = {10, 11, 0, 1, 2, 3, 9, 4, 5, 6, 7, 8};
    unsigned best[10] = {0}, distinct = 0;
    uint64_t scores[10] = {0};
    for (unsigned i = 0; i < SYLLABLES; i++) {
        unsigned first = order[i];
        distinct += keep_word(first, counts, best, scores);
        for (unsigned j = 0; j < SYLLABLES; j++) {
            unsigned duo = SYLLABLES + first * SYLLABLES + order[j];
            distinct += keep_word(duo, counts, best, scores);
            for (unsigned k = 0; k < SYLLABLES; k++) {
                unsigned trio = SYLLABLES + duo * SYLLABLES + order[k];
                distinct += keep_word(trio, counts, best, scores);
            }
        }
    }
    for (unsigned i = 0; i < 10 && scores[i]; i++) {
        char word[7];
        decode(word, best[i]);
        printf("%s %llu\n", word, (unsigned long long)scores[i]);
    }
#else
    Entry entries[WORDS];
    unsigned distinct = 0;
    for (unsigned i = 0; i < WORDS; i++) {
        if (!counts[i]) continue;
        Entry *entry = &entries[distinct++];
        entry->count = counts[i];
        decode(entry->word, i);
    }
    qsort(entries, distinct, sizeof(*entries), compare);
    for (unsigned i = 0; i < distinct && i < 10; i++) {
        printf("%s %llu\n", entries[i].word, (unsigned long long)entries[i].count);
    }
#endif
    printf("%u\n", distinct);
    c_extremo_fin(start);
    return EXIT_SUCCESS;
}
