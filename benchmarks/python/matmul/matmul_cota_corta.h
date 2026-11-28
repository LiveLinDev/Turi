/* Cota del total para una salida de seis decimales. Los prefijos parciales
   proceden del mismo generador afín que usa short_periodic. */
#include "matmul_cota_datos.inc"

static inline __m512d short_residues(uint64_t word) {
    __m512i digits = _mm512_cvtepu8_epi64(_mm_cvtsi64_si128((long long)word));
#ifdef __AVX512DQ__
    return _mm512_cvtepu64_pd(digits);
#else
    __m512i encoded = _mm512_or_si512(digits, _mm512_set1_epi64(INT64_C(0x4330000000000000)));
    return _mm512_sub_pd(_mm512_castsi512_pd(encoded), _mm512_set1_pd(0x1p52));
#endif
}

static inline __attribute__((always_inline))
void short_bounds_block(int first, int groups, int terms, const double *base, const double *weights,
                        __m512d error, double below[SHORT_STRIDE], double above[SHORT_STRIDE]) {
    __m512d tail[4];
    for (int lane = 0; lane < groups; ++lane) tail[lane] = _mm512_setzero_pd();
    for (int k = 0; k < terms; ++k) {
        __m512d weight = _mm512_set1_pd(weights[k]);
        for (int lane = 0; lane < groups; ++lane) {
            const uint64_t word = short_cert_digits[(SHORT_STRIDE / 8) * k + first / 8 + lane];
            tail[lane] = _mm512_fmadd_pd(short_residues(word), weight, tail[lane]);
        }
    }
    for (int lane = 0; lane < groups; ++lane) {
        int row = first + 8 * lane;
        __m512d value = _mm512_add_pd(_mm512_loadu_pd(base + row), tail[lane]);
        _mm512_storeu_pd(below + row, _mm512_sub_pd(value, error));
        _mm512_storeu_pd(above + row, _mm512_add_pd(value, error));
    }
}

static int short_certified(int terms, double *trace, double *total) {
    const double *base = short_cert_rows + terms * (2 * SHORT_STRIDE + SHORT_BAND + 1);
    const double *diagonal = base + SHORT_STRIDE;
    const double *weights = diagonal + SHORT_STRIDE;
    const __m512d error = _mm512_set1_pd(weights[SHORT_BAND]);
    double below[SHORT_STRIDE], above[SHORT_STRIDE];
    int first = 0;
#ifdef TZ_MATMUL_COTA_CUATRO
    for (; first + 32 <= SHORT_STRIDE; first += 32)
        short_bounds_block(first, 4, terms, base, weights, error, below, above);
#endif
    for (; first < SHORT_STRIDE; first += 8)
        short_bounds_block(first, 1, terms, base, weights, error, below, above);

    double low = 0.0, high = 0.0, sum = 0.0;
    for (int cycle = 0; cycle < 5; ++cycle) {
        const int rows = cycle < 4 ? SHORT_PERIOD : terms;
        for (int row = 0; row < rows; ++row) {
            low += below[row];
            high += above[row];
            sum += diagonal[row];
        }
    }
    const double scaled = high * 1000000.0;
    const double margin = (scaled + 1.0) * 0x1p-51;
    if ((uint64_t)(low * 1000000.0 + 0.5 - margin) != (uint64_t)(scaled + 0.5 + margin)) return 0;
    *trace = sum;
    *total = low;
    return 1;
}
