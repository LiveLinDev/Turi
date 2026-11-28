/* Norma espectral en un hilo. Cada carril AVX-512 acumula una fila en el
   orden de Python. Los denominadores triangulares avanzan por diferencias. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef __AVX512F__
#error Este banco requiere AVX-512F
#endif

#pragma STDC FP_CONTRACT OFF

#ifdef TZ_ESPECTRAL_SERIE
#include "spectralnorm_series.h"
#endif

/* Para divisores enteros positivos de hasta 2^31, la estimación por producto
   tiene error relativo menor que 4 * 2^-53. El primer FMA da el residuo exacto.
   El error antes de redondear la corrección es el cuadrado de ese residuo,
   menor que la distancia relativa a cualquier frontera de redondeo, al menos 2^-85. */
static inline __m512d correct_reciprocal(__m512d denominator, __m512d estimate, __m512d one) {
    __m512d error = _mm512_fnmadd_pd(denominator, estimate, one);
    return _mm512_fmadd_pd(estimate, error, estimate);
}

static inline void reciprocal_pair(const __m512d *d, __m512d *r, __m512d one) {
    __m512d inverse_product = _mm512_div_pd(one, _mm512_mul_pd(d[0], d[1]));
    r[0] = correct_reciprocal(d[0], _mm512_mul_pd(inverse_product, d[1]), one);
    r[1] = correct_reciprocal(d[1], _mm512_mul_pd(inverse_product, d[0]), one);
}

static inline void times_block(const double *restrict u, double *restrict out,
                               long n, long start, int vectors, int transposed) {
    const __m512d lanes = _mm512_setr_pd(0, 1, 2, 3, 4, 5, 6, 7);
    const __m512d one = _mm512_set1_pd(1);
    const __m512d half = _mm512_set1_pd(0.5);
    __m512d denominator[4], increment[4], sum[4];
    for (int k = 0; k < vectors; k++) {
        __m512d i = _mm512_add_pd(_mm512_set1_pd((double)(start + 8 * k)), lanes);
        __m512d offset = transposed ? one : _mm512_add_pd(i, one);
        denominator[k] = _mm512_add_pd(_mm512_mul_pd(_mm512_mul_pd(i, half), _mm512_add_pd(i, one)), offset);
        increment[k] = _mm512_add_pd(i, _mm512_set1_pd(transposed ? 2 : 1));
        sum[k] = _mm512_setzero_pd();
    }
    /* Con n <= 32768, el denominador triangular más grande es menor que 2^31. */
    int paired = vectors == 4 && n <= 32768;
    for (long j = 0; j < n; j++) {
        __m512d x = _mm512_set1_pd(u[j]);
        __m512d reciprocals[4];
        if (paired) {
            reciprocal_pair(denominator, reciprocals, one);
            reciprocal_pair(denominator + 2, reciprocals + 2, one);
        }
        for (int k = 0; k < vectors; k++) {
            __m512d reciprocal = paired ? reciprocals[k] : _mm512_div_pd(one, denominator[k]);
            sum[k] = _mm512_add_pd(sum[k], _mm512_mul_pd(reciprocal, x));
            denominator[k] = _mm512_add_pd(denominator[k], increment[k]);
            increment[k] = _mm512_add_pd(increment[k], one);
        }
    }
    for (int k = 0; k < vectors; k++) {
        long at = start + 8 * k;
        long remaining = n - at;
        __mmask8 valid = remaining < 8 ? (__mmask8)((1u << remaining) - 1u) : 0xff;
        _mm512_mask_storeu_pd(out + at, valid, sum[k]);
    }
}

static void times(const double *restrict u, double *restrict out, long n, int transposed) {
    long i = 0;
    for (; i + 32 <= n; i += 32) times_block(u, out, n, i, 4, transposed);
    for (; i < n; i += 8) times_block(u, out, n, i, 1, transposed);
}

int main(int argc, char **argv) {
    long n = argc > 1 ? strtol(argv[1], NULL, 10) : 100;
    /* En este intervalo los denominadores enteros caben exactamente en f64. */
    if (n <= 0 || n > (1L << 25)) return EXIT_FAILURE;
    double start = c_extremo_inicio();
    double *storage = malloc(3 * (size_t)n * sizeof(double));
    if (!storage) return EXIT_FAILURE;
    double *u = storage, *v = storage + n, *tmp = storage + 2 * n;
    for (long i = 0; i < n; i++) u[i] = 1.0;
#ifdef TZ_ESPECTRAL_SERIE
    double answer;
    if (serie_solve(u, v, tmp, n, &answer)) {
        free(storage);
        c_extremo_fin(start);
        printf("%.9f\n", answer);
        return EXIT_SUCCESS;
    }
#endif
    for (int step = 0; step < 10; step++) {
        times(u, tmp, n, 0);
        times(tmp, v, n, 1);
        times(v, tmp, n, 0);
        times(tmp, u, n, 1);
#ifdef TZ_ESPECTRAL_INTERVALO
        if (n <= 32768 && step < 9) {
            double lower = u[0] / v[0], upper = lower;
            for (long j = 1; j < n; j++) {
                double ratio = u[j] / v[j];
                lower = fmin(lower, ratio);
                upper = fmax(upper, ratio);
            }
            double margin = (double)((16 * (9 - step) + 16) * (n + 3)) * 0x1p-53;
            double low = sqrt(lower * (1.0 - margin)), high = sqrt(upper * (1.0 + margin));
            long long first = (long long)((low - 0x1p-47) * 1000000000.0 + 0.5);
            long long last = (long long)((high + 0x1p-47) * 1000000000.0 + 0.5);
            if (first == last) {
                free(storage);
                c_extremo_fin(start);
                printf("%.9f\n", (low + high) * 0.5);
                return EXIT_SUCCESS;
            }
        }
#endif
    }
    double vbv = 0.0, vv = 0.0;
    for (long i = 0; i < n; i++) {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    double result = sqrt(vbv / vv);
    free(storage);
    c_extremo_fin(start);
    printf("%.9f\n", result);
    return EXIT_SUCCESS;
}
