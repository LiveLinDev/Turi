/* Producto f64 con cuatro filas y 32 columnas por bloque de registros.
   Cada elemento se acumula en el mismo orden k que en Python. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <stdint.h>

#ifndef __AVX512F__
#error Este banco requiere AVX-512F
#endif

#pragma STDC FP_CONTRACT OFF

static inline __attribute__((always_inline))
void multiply_block(const double *restrict a, const double *restrict b,
                    double *restrict c, int n, int col, int rows, int vectors) {
    __m512d sum[4][4];
    for (int row = 0; row < rows; row++) {
        for (int v = 0; v < vectors; v++) sum[row][v] = _mm512_setzero_pd();
    }
    int remaining = n - col;
    __mmask8 active = remaining < 8 ? (__mmask8)((1u << remaining) - 1u) : 0xff;
    for (int k = 0; k < n; k++) {
        const double *bk = b + (size_t)k * n + col;
        __m512d br[4];
        for (int v = 0; v < vectors; v++) br[v] = _mm512_maskz_loadu_pd(active, bk + 8 * v);
        for (int row = 0; row < rows; row++) {
            __m512d ar = _mm512_set1_pd(a[(size_t)row * n + k]);
            for (int v = 0; v < vectors; v++) {
                sum[row][v] = _mm512_add_pd(sum[row][v], _mm512_mul_pd(ar, br[v]));
            }
        }
    }
    for (int row = 0; row < rows; row++) {
        double *cr = c + (size_t)row * n + col;
        for (int v = 0; v < vectors; v++) _mm512_mask_storeu_pd(cr + 8 * v, active, sum[row][v]);
    }
}

static void multiply(const double *restrict a, const double *restrict b, double *restrict c, int n) {
    int row = 0;
    for (; row + 4 <= n; row += 4) {
        const double *ar = a + (size_t)row * n;
        double *cr = c + (size_t)row * n;
        int col = 0;
        for (; col + 32 <= n; col += 32) multiply_block(ar, b, cr, n, col, 4, 4);
        for (; col < n; col += 8) multiply_block(ar, b, cr, n, col, 4, 1);
    }
    for (; row < n; row++) {
        const double *ar = a + (size_t)row * n;
        double *cr = c + (size_t)row * n;
        for (int col = 0; col < n; col += 8) multiply_block(ar, b, cr, n, col, 1, 1);
    }
}

static void make(double *a, int n, int seed) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) a[(size_t)i * n + j] = ((i * 31 + j * 17 + seed) % 97) / 97.0;
    }
}

#ifdef TZ_MATMUL_PERIODOS
#ifdef TZ_MATMUL_BALDOSAS
static inline __attribute__((always_inline))
void tile(const double *restrict a, const double *restrict b, double *restrict c,
          int n, int period, int stride, int row, int col, int rows, int vectors) {
    __m512d accumulators[4][4];
    for (int r = 0; r < rows; r++)
        for (int v = 0; v < vectors; v++) accumulators[r][v] = _mm512_setzero_pd();
#ifdef TZ_MATMUL_CICLOS
    int complete = n / period, rest = n % period;
    for (int segment = 0; segment <= complete; segment++) {
      int terms = segment == complete ? rest : period;
      for (int k = 0; k < terms; k++) {
#else
    int k = 0;
    for (int term = 0; term < n; term++) {
#endif
        __m512d right[4];
        for (int v = 0; v < vectors; v++)
            right[v] = _mm512_loadu_pd(b + k * stride + col + 8 * v);
        for (int r = 0; r < rows; r++) {
            __m512d left = _mm512_set1_pd(a[(row + r) * stride + k]);
            for (int v = 0; v < vectors; v++)
                accumulators[r][v] = _mm512_add_pd(accumulators[r][v], _mm512_mul_pd(left, right[v]));
        }
#ifdef TZ_MATMUL_CICLOS
      }
#else
        if (++k == period) k = 0;
#endif
    }
    for (int r = 0; r < rows; r++)
        for (int v = 0; v < vectors; v++)
            _mm512_storeu_pd(c + (row + r) * stride + col + 8 * v, accumulators[r][v]);
}
#endif

static int periodic(int n) {
    /* Los dos generadores se repiten cada 97 índices en los dos ejes. */
    enum { PERIOD = 97 };
    int p = n < PERIOD ? n : PERIOD;
    int stride = (p + 7) & ~7;
    size_t cells = (size_t)p * stride;
    double *storage = calloc(3 * cells, sizeof(*storage));
    if (!storage) return EXIT_FAILURE;
    double *a = storage, *b = a + cells, *c = b + cells;
    for (int i = 0; i < p; i++) {
        for (int j = 0; j < p; j++) {
            int code = (i * 31 + j * 17) % PERIOD;
            a[i * stride + j] = ((code + 1) % PERIOD) / 97.0;
            b[i * stride + j] = ((code + 2) % PERIOD) / 97.0;
        }
    }
#ifdef TZ_MATMUL_BALDOSAS
    int row = 0;
    for (; row + 4 <= p; row += 4) {
        int col = 0;
        for (; col + 32 <= stride; col += 32) tile(a, b, c, n, p, stride, row, col, 4, 4);
        for (; col < stride; col += 8) tile(a, b, c, n, p, stride, row, col, 4, 1);
    }
    for (; row < p; row++)
        for (int col = 0; col < stride; col += 8) tile(a, b, c, n, p, stride, row, col, 1, 1);
#else
    for (int i = 0; i < p; i++) {
        const double* left = a + i * stride;
        for (int j = 0; j < stride; j += 8) {
            __m512d value = _mm512_setzero_pd();
            int k = 0;
            for (int term = 0; term < n; term++) {
                value = _mm512_add_pd(value,
                    _mm512_mul_pd(_mm512_set1_pd(left[k]), _mm512_loadu_pd(b + k * stride + j)));
                if (++k == PERIOD) k = 0;
            }
            _mm512_storeu_pd(c + i * stride + j, value);
        }
    }
#endif
    double sums[PERIOD];
#ifdef TZ_MATMUL_SUMAS_BLOQUE
    const int cycles = n / p, rest = n % p;
#endif
    for (int i = 0; i < p; i++) {
        double sum = 0.0;
#ifdef TZ_MATMUL_SUMAS_BLOQUE
        for (int cycle = 0; cycle < cycles; cycle++)
            for (int j = 0; j < p; j++) sum += c[i * stride + j];
        for (int j = 0; j < rest; j++) sum += c[i * stride + j];
#else
        for (int j = 0; j < n; j++) sum += c[i * stride + j % PERIOD];
#endif
        sums[i] = sum;
    }
    double trace = 0.0, total = 0.0;
#ifdef TZ_MATMUL_SUMAS_BLOQUE
    for (int cycle = 0; cycle < cycles; cycle++) {
        for (int j = 0; j < p; j++) {
            trace += c[j * stride + j];
            total += sums[j];
        }
    }
    for (int j = 0; j < rest; j++) {
        trace += c[j * stride + j];
        total += sums[j];
    }
#else
    for (int i = 0; i < n; i++) {
        int j = i % PERIOD;
        trace += c[j * stride + j];
        total += sums[j];
    }
#endif
    printf("%.6f %.6f\n", trace, total);
    free(storage);
    return EXIT_SUCCESS;
}
#endif

int main(int argc, char **argv) {
    long requested = argc > 1 ? strtol(argv[1], NULL, 10) : 200;
    if (requested <= 0 || requested > 32768) return EXIT_FAILURE;
    int n = (int)requested;
    double start = c_extremo_inicio();
#ifdef TZ_MATMUL_PERIODOS
    int status = periodic(n);
    c_extremo_fin(start);
    return status;
#else
    size_t cells = (size_t)n * n;
    if (cells > SIZE_MAX / (3 * sizeof(double))) return EXIT_FAILURE;
    double *storage = malloc(3 * cells * sizeof(double));
    if (!storage) return EXIT_FAILURE;
    double *a = storage, *b = storage + cells, *c = storage + 2 * cells;
    make(a, n, 1);
    make(b, n, 2);
    multiply(a, b, c, n);
    double trace = 0.0, total = 0.0;
    for (int i = 0; i < n; i++) {
        const double *row = c + (size_t)i * n;
        trace += row[i];
        double sum = 0.0;
        for (int j = 0; j < n; j++) sum += row[j];
        total += sum;
    }
    printf("%.6f %.6f\n", trace, total);
    free(storage);
    c_extremo_fin(start);
    return EXIT_SUCCESS;
#endif
}
