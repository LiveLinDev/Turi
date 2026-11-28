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
#ifdef TZ_MATMUL_PREFIJO
enum { CHECKPOINT_PERIOD = 97, CHECKPOINT_STRIDE = 104, CHECKPOINT_PHASE = 2 * CHECKPOINT_PERIOD + 8 };
static const uint64_t checkpoint[] = {
#include "matmul_prefijo.inc"
};

static inline __m512d checkpoint_value(int row, int col) {
    unsigned phase = (unsigned)checkpoint[CHECKPOINT_PHASE + row];
    __m512i reference = _mm512_loadu_si512(checkpoint + phase + col);
    uint64_t word = checkpoint[CHECKPOINT_PHASE + CHECKPOINT_PERIOD + (row * CHECKPOINT_STRIDE + col) / 8];
    __m512i offsets = _mm512_cvtepu8_epi64(_mm_cvtsi64_si128((long long)word));
    return _mm512_castsi512_pd(_mm512_sub_epi64(_mm512_add_epi64(reference, offsets), _mm512_set1_epi64(128)));
}
#endif

#ifdef TZ_MATMUL_FRANJAS
static inline void transpose_columns(double *out, __m256d a, __m256d b, __m256d c, __m256d d) {
    __m256d even_ab = _mm256_unpacklo_pd(a, b), odd_ab = _mm256_unpackhi_pd(a, b);
    __m256d even_cd = _mm256_unpacklo_pd(c, d), odd_cd = _mm256_unpackhi_pd(c, d);
    _mm256_storeu_pd(out, _mm256_permute2f128_pd(even_ab, even_cd, 0x20));
    _mm256_storeu_pd(out + 4, _mm256_permute2f128_pd(odd_ab, odd_cd, 0x20));
    _mm256_storeu_pd(out + 8, _mm256_permute2f128_pd(even_ab, even_cd, 0x31));
    _mm256_storeu_pd(out + 12, _mm256_permute2f128_pd(odd_ab, odd_cd, 0x31));
}
#endif

static inline __attribute__((always_inline))
void tile(const double *restrict a, const double *restrict b, double *restrict c,
          int n, int period, int stride, int left_stride, int row, int col, int rows, int vectors) {
    __m512d accumulators[4][4];
#ifdef TZ_MATMUL_PREFIJO
    int prefix = period == CHECKPOINT_PERIOD && n >= 4 * CHECKPOINT_PERIOD ? 4 : 0;
    for (int r = 0; r < rows; r++)
        for (int v = 0; v < vectors; v++)
            accumulators[r][v] = prefix ? checkpoint_value(row + r, col + 8 * v) : _mm512_setzero_pd();
#else
    int prefix = 0;
    for (int r = 0; r < rows; r++)
        for (int v = 0; v < vectors; v++) accumulators[r][v] = _mm512_setzero_pd();
#endif
#ifdef TZ_MATMUL_CICLOS
    int complete = n / period, rest = n % period;
    for (int segment = prefix; segment <= complete; segment++) {
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
            __m512d left = _mm512_set1_pd(a[(row + r) * left_stride + k]);
            for (int v = 0; v < vectors; v++)
                accumulators[r][v] = _mm512_add_pd(accumulators[r][v], _mm512_mul_pd(left, right[v]));
        }
#ifdef TZ_MATMUL_CICLOS
      }
#else
        if (++k == period) k = 0;
#endif
    }
#ifdef TZ_MATMUL_FRANJAS
    for (int v = 0; v < vectors; v++) {
        if (rows == 1) {
            _mm512_storeu_pd(c + col + 8 * v, accumulators[0][v]);
        } else {
            __m512d a0 = accumulators[0][v], a1 = accumulators[1][v];
            __m512d a2 = accumulators[2][v], a3 = accumulators[3][v];
            double *out = c + 4 * (col + 8 * v);
            transpose_columns(out, _mm512_castpd512_pd256(a0), _mm512_castpd512_pd256(a1),
                             _mm512_castpd512_pd256(a2), _mm512_castpd512_pd256(a3));
            transpose_columns(out + 16, _mm512_extractf64x4_pd(a0, 1), _mm512_extractf64x4_pd(a1, 1),
                             _mm512_extractf64x4_pd(a2, 1), _mm512_extractf64x4_pd(a3, 1));
        }
    }
#else
    for (int r = 0; r < rows; r++)
        for (int v = 0; v < vectors; v++)
            _mm512_storeu_pd(c + (row + r) * stride + col + 8 * v, accumulators[r][v]);
#endif
}
#endif

static int periodic(int n) {
    /* Los dos generadores se repiten cada 97 índices en los dos ejes. */
    enum { PERIOD = 97 };
    int p = n < PERIOD ? n : PERIOD;
    int stride = (p + 7) & ~7;
    int inner = p;
#ifdef TZ_MATMUL_FACTORES
    if (n >= 4 * PERIOD && n < 5 * PERIOD) inner = n % PERIOD;
#endif
    int left_stride = stride, result_rows = p;
#ifdef TZ_MATMUL_FRANJAS
    left_stride = inner;
    result_rows = 4;
#endif
    size_t left_cells = (size_t)p * left_stride;
    size_t right_cells = (size_t)inner * stride;
    double *storage = calloc(left_cells + right_cells + (size_t)result_rows * stride, sizeof(*storage));
    if (!storage) return EXIT_FAILURE;
    double *a = storage, *b = a + left_cells, *c = b + right_cells;
    if (inner == p) {
        for (int i = 0; i < p; i++) {
            for (int j = 0; j < p; j++) {
                int code = (i * 31 + j * 17) % PERIOD;
                a[i * left_stride + j] = ((code + 1) % PERIOD) / 97.0;
                b[i * stride + j] = ((code + 2) % PERIOD) / 97.0;
            }
        }
    }
#ifdef TZ_MATMUL_FACTORES
    else if (inner) {
#ifdef TZ_MATMUL_RAZONES
        double ratios[PERIOD];
        for (int i = 0; i < PERIOD; i++) ratios[i] = i / 97.0;
        for (int i = 0, seed = 1; i < p; i++, seed = (seed + 31) % PERIOD) {
            int key = seed;
            for (int j = 0; j < inner; j++, key = (key + 17) % PERIOD)
                a[i * left_stride + j] = ratios[key];
        }
        for (int i = 0, seed = 2; i < inner; i++, seed = (seed + 31) % PERIOD) {
            int key = seed;
            for (int j = 0; j < p; j++, key = (key + 17) % PERIOD)
                b[i * stride + j] = ratios[key];
        }
#else
        for (int i = 0; i < p; i++)
            for (int j = 0; j < inner; j++)
                a[i * left_stride + j] = ((i * 31 + j * 17 + 1) % PERIOD) / 97.0;
        for (int i = 0; i < inner; i++)
            for (int j = 0; j < p; j++)
                b[i * stride + j] = ((i * 31 + j * 17 + 2) % PERIOD) / 97.0;
#endif
    }
#endif
#ifdef TZ_MATMUL_SUMAS_BLOQUE
    const int cycles = n / p, rest = n % p;
#endif
#ifdef TZ_MATMUL_FRANJAS
    double diagonal[PERIOD], sums[PERIOD];
#endif
#ifdef TZ_MATMUL_BALDOSAS
    int row = 0;
    for (; row + 4 <= p; row += 4) {
        int col = 0;
        for (; col + 32 <= stride; col += 32) tile(a, b, c, n, p, stride, left_stride, row, col, 4, 4);
        for (; col < stride; col += 8) tile(a, b, c, n, p, stride, left_stride, row, col, 4, 1);
#ifdef TZ_MATMUL_FRANJAS
        __m256d sum = _mm256_setzero_pd();
        for (int cycle = 0; cycle <= cycles; cycle++) {
            int terms = cycle == cycles ? rest : p;
            for (int j = 0; j < terms; j++) sum = _mm256_add_pd(sum, _mm256_loadu_pd(c + j * 4));
        }
        _mm256_storeu_pd(sums + row, sum);
        for (int lane = 0; lane < 4; lane++) diagonal[row + lane] = c[(row + lane) * 4 + lane];
#endif
    }
    for (; row < p; row++) {
        for (int col = 0; col < stride; col += 8) tile(a, b, c, n, p, stride, left_stride, row, col, 1, 1);
#ifdef TZ_MATMUL_FRANJAS
        double sum = 0.0;
        for (int cycle = 0; cycle <= cycles; cycle++) {
            int terms = cycle == cycles ? rest : p;
            for (int j = 0; j < terms; j++) sum += c[j];
        }
        sums[row] = sum;
        diagonal[row] = c[row];
#endif
    }
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
#if defined(TZ_MATMUL_FRANJAS)
    const double *trace_values = diagonal;
    const int trace_stride = 1;
#else
    const double *trace_values = c;
    const int trace_stride = stride + 1;
#ifdef TZ_MATMUL_SUMAS_SIMD
    double sums[(PERIOD + 7) & ~7];
    for (int i = 0; i < p; i++)
        for (int j = 0; j < p; j++)
            a[(size_t)j * stride + i] = c[(size_t)i * stride + j];
    for (int first = 0; first < stride; first += 8) {
        __m512d sum = _mm512_setzero_pd();
        for (int cycle = 0; cycle <= cycles; cycle++) {
            int terms = cycle == cycles ? rest : p;
            for (int j = 0; j < terms; j++)
                sum = _mm512_add_pd(sum, _mm512_loadu_pd(a + (size_t)j * stride + first));
        }
        _mm512_storeu_pd(sums + first, sum);
    }
#else
    double sums[PERIOD];
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
#endif
#endif
    double trace = 0.0, total = 0.0;
#ifdef TZ_MATMUL_SUMAS_BLOQUE
    for (int cycle = 0; cycle < cycles; cycle++) {
        for (int j = 0; j < p; j++) {
            trace += trace_values[j * trace_stride];
            total += sums[j];
        }
    }
    for (int j = 0; j < rest; j++) {
        trace += trace_values[j * trace_stride];
        total += sums[j];
    }
#else
    for (int i = 0; i < n; i++) {
        int j = i % PERIOD;
        trace += trace_values[j * trace_stride];
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
