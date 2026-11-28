/* Prefijos de cuatro ciclos de la misma matriz periódica. Se completan
   las columnas restantes y se acumulan en el orden de cada fila. */
enum { SHORT_PERIOD = 97, SHORT_STRIDE = 104, SHORT_BAND = 16,
       SHORT_DIAGONAL = SHORT_STRIDE * SHORT_BAND,
       SHORT_SEED = SHORT_DIAGONAL + SHORT_PERIOD * SHORT_BAND,
       SHORT_LEFT = SHORT_SEED + SHORT_STRIDE * SHORT_BAND,
       SHORT_RIGHT = SHORT_LEFT + SHORT_STRIDE * SHORT_BAND };

static const double short_data[] = {
#include "matmul_cola_corta_datos.inc"
};

#if defined(TZ_MATMUL_SALIDA_FIJA) || defined(TZ_MATMUL_COTA)
#include <string.h>

/* Los resultados de la franja son no negativos y menores que 404^3.
   El entero escalado cabe en u64. Se redondea una vez, con empate al par. */
static char *short_emit_six(char *tail, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    const unsigned exponent = (unsigned)(bits >> 52);
    const unsigned shift = exponent ? 1075 - exponent : 1074;
    uint64_t fixed = 0;
    if (shift < 128) {
        const uint64_t mantissa = (bits & UINT64_C(0xfffffffffffff)) | (UINT64_C(1) << 52);
        const unsigned __int128 product = (unsigned __int128)mantissa * UINT64_C(1000000);
        const unsigned __int128 half = (unsigned __int128)1 << (shift - 1);
        fixed = (uint64_t)((product + half - 1 + ((product >> shift) & 1)) >> shift);
    }
    uint64_t whole = fixed / UINT64_C(1000000);
    uint64_t fraction = fixed - whole * UINT64_C(1000000);
    for (int i = 0; i < 6; ++i) {
        *--tail = (char)('0' + fraction % 10);
        fraction /= 10;
    }
    *--tail = '.';
    do {
        *--tail = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    return tail;
}

static int short_print(double trace, double total) {
    char line[48];
    char *const end = line + sizeof(line);
    char *tail = end;
    *--tail = '\n';
    tail = short_emit_six(tail, total);
    *--tail = ' ';
    tail = short_emit_six(tail, trace);
    const size_t size = (size_t)(end - tail);
    return fwrite(tail, 1, size, stdout) == size ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif

#ifdef TZ_MATMUL_COTA
#include "matmul_cota_corta.h"
#endif

static inline __attribute__((always_inline))
__m512d short_tile(int row, int col, int columns, int rest, __m512d accum) {
    __m512d cells[12];
    for (int j = 0; j < columns; ++j)
        cells[j] = _mm512_loadu_pd(short_data + SHORT_SEED + SHORT_STRIDE * (col + j) + row);
    for (int k = 0; k < rest; ++k) {
        const __m512d left = _mm512_loadu_pd(short_data + SHORT_LEFT + SHORT_STRIDE * k + row);
        for (int j = 0; j < columns; ++j) {
            const __m512d right = _mm512_set1_pd(short_data[SHORT_RIGHT + SHORT_BAND * k + col + j]);
            cells[j] = _mm512_add_pd(cells[j], _mm512_mul_pd(left, right));
        }
    }
    for (int j = 0; j < columns; ++j) accum = _mm512_add_pd(accum, cells[j]);
    return accum;
}

static int short_periodic(int rest) {
    double trace = 0.0, total = 0.0;
#ifdef TZ_MATMUL_COTA
    if (short_certified(rest, &trace, &total)) return short_print(trace, total);
#endif
    double sums[SHORT_STRIDE];
    const double *diagonal = short_data + SHORT_DIAGONAL + SHORT_PERIOD * rest;
    for (int row = 0; row < SHORT_STRIDE; row += 8) {
        __m512d accum = _mm512_loadu_pd(short_data + rest * SHORT_STRIDE + row);
        int col = 0;
#ifdef TZ_MATMUL_DOCE_COLUMNAS
        if (rest >= 12) {
            accum = short_tile(row, 0, 12, rest, accum);
            col = 12;
        }
#endif
        for (; col + 4 <= rest; col += 4) accum = short_tile(row, col, 4, rest, accum);
        for (; col < rest; ++col) accum = short_tile(row, col, 1, rest, accum);
        _mm512_storeu_pd(sums + row, accum);
    }
    for (int cycle = 0; cycle < 4; ++cycle) {
        for (int i = 0; i < SHORT_PERIOD; ++i) {
            trace += diagonal[i];
            total += sums[i];
        }
    }
    for (int i = 0; i < rest; ++i) {
        trace += diagonal[i];
        total += sums[i];
    }
#ifdef TZ_MATMUL_SALIDA_FIJA
    return short_print(trace, total);
#else
    return printf("%.6f %.6f\n", trace, total) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
#endif
}
