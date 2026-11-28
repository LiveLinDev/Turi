/* Cuatro grupos independientes de ocho píxeles mantienen ocupadas las
   unidades vectoriales sin cambiar la secuencia f64 de cada píxel. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <stdint.h>

#ifndef __AVX512F__
#error Este banco requiere AVX-512F
#endif

#pragma STDC FP_CONTRACT OFF

static uint64_t pixels(int x, int width, int vectors, double inverse, double imaginary, uint64_t *inside) {
#ifdef TZ_MANDEL_DISCOS
    int size = width - x < 8 * vectors ? width - x : 8 * vectors;
    double left = x * inverse - 1.5, right = (x + size - 1) * inverse - 1.5;
    double vertical = imaginary * imaginary;
    double a = left + 1.0, b = right + 1.0;
    if ((vertical <= 0.06 && left * left + vertical <= 0.06 && right * right + vertical <= 0.06) ||
        (vertical <= 0.009 && a * a + vertical <= 0.009 && b * b + vertical <= 0.009)) {
        *inside += (uint64_t)size;
        return vectors == 1 ? (255u & (255u << (8 - size))) : 255u * (1 + 31 + 961 + 29791);
    }
#endif
    const __m512d inv = _mm512_set1_pd(inverse), ci = _mm512_set1_pd(imaginary);
    __m512d cr[4], zr[4], zi[4];
    const __m512d lanes = _mm512_setr_pd(0, 1, 2, 3, 4, 5, 6, 7);
    const __m512d offset = _mm512_set1_pd(1.5);
    for (int k = 0; k < vectors; k++) {
        __m512d columns = _mm512_add_pd(_mm512_set1_pd(x + 8 * k), lanes);
        cr[k] = _mm512_sub_pd(_mm512_mul_pd(columns, inv), offset);
        zr[k] = zi[k] = _mm512_setzero_pd();
    }
    for (int step = 0; step < 50; step++) {
        for (int k = 0; k < vectors; k++) {
            __m512d r = zr[k], i = zi[k];
            zr[k] = _mm512_add_pd(_mm512_sub_pd(_mm512_mul_pd(r, r), _mm512_mul_pd(i, i)), cr[k]);
            zi[k] = _mm512_add_pd(_mm512_mul_pd(_mm512_add_pd(r, r), i), ci);
        }
#ifdef TZ_MANDEL_ESCAPE
#ifdef TZ_MANDEL_CADENCIA
        if (step == 4 || step % 10 == 9) {
#else
        if (step % 5 == 4) {
#endif
            __mmask8 live = 0;
            for (int k = 0; k < vectors; k++) {
                __m512d magnitude = _mm512_add_pd(_mm512_mul_pd(zr[k], zr[k]), _mm512_mul_pd(zi[k], zi[k]));
                live |= _mm512_cmp_pd_mask(magnitude, _mm512_set1_pd(32.0), _CMP_LE_OQ);
            }
            if (!live) return 0;
        }
#endif
    }
    uint64_t bytes = 0;
    for (int k = 0; k < vectors; k++) {
        __m512d magnitude = _mm512_add_pd(_mm512_mul_pd(zr[k], zr[k]), _mm512_mul_pd(zi[k], zi[k]));
        unsigned mask = _mm512_cmp_pd_mask(magnitude, _mm512_set1_pd(4.0), _CMP_LE_OQ);
        int remaining = width - x - 8 * k;
        if (remaining < 8) mask &= (1u << remaining) - 1u;
        *inside += (unsigned)__builtin_popcount(mask);
        bytes = 31 * bytes + __builtin_bitreverse8((uint8_t)mask);
    }
    return bytes;
}

#ifdef TZ_MANDEL_SIMETRIA
#ifdef TZ_MANDEL_TRAMOS
#include "mandelbrot_tramos.h"
#elif defined(TZ_MANDEL_REGIONES)
#include "mandelbrot_regiones.h"
#else
static uint64_t row(int n, double inverse, double imaginary, uint64_t *inside) {
    const uint64_t modulus = 1000000007;
    uint64_t checksum = 0;
    int x = 0;
    for (; x + 32 <= n; x += 32)
        checksum = (checksum * 923521 + pixels(x, n, 4, inverse, imaginary, inside)) % modulus;
    for (; x < n; x += 8)
        checksum = (checksum * 31 + pixels(x, n, 1, inverse, imaginary, inside)) % modulus;
    return checksum;
}
#endif

int main(int argc, char **argv) {
    long requested = argc > 1 ? strtol(argv[1], NULL, 10) : 200;
    if (requested < 1 || requested > (1L << 25)) return EXIT_FAILURE;
    int n = (int)requested;
    double start = c_extremo_inicio();
    double inverse = 2.0 / n;
    const uint64_t modulus = 1000000007;
    uint64_t inside = 0, factor = 1;
    for (int block = 0; block < (n + 7) / 8; block++)
        factor = 31 * factor % modulus;
    uint64_t top = row(n, inverse, -1.0, &inside), bottom = 0, shift = 1;
    for (int y = 1; y < (n + 1) / 2; y++) {
        double ci = y * inverse - 1.0;
        double other = (n - y) * inverse - 1.0;
        uint64_t count = 0;
        uint64_t a = row(n, inverse, ci, &count), b = a;
        inside += count;
        if (ci == -other) inside += count;
        else b = row(n, inverse, other, &inside);
        top = (top * factor + a) % modulus;
        bottom = (shift * b + bottom) % modulus;
        shift = shift * factor % modulus;
    }
    if ((n & 1) == 0)
        top = (top * factor + row(n, inverse, (n / 2) * inverse - 1.0, &inside)) % modulus;
    uint64_t checksum = (top * shift + bottom) % modulus;
    c_extremo_fin(start);
    printf("%llu\n%llu\n", (unsigned long long)inside, (unsigned long long)checksum);
    return EXIT_SUCCESS;
}
#else
int main(int argc, char **argv) {
    long requested = argc > 1 ? strtol(argv[1], NULL, 10) : 200;
    if (requested < 1 || requested > (1L << 25)) return EXIT_FAILURE;
    int n = (int)requested;
    double start = c_extremo_inicio();
    double inverse = 2.0 / n;
    const uint64_t base = 31, modulus = 1000000007;
    uint64_t inside = 0, checksum = 0;
    for (int y = 0; y < n; y++) {
        double ci = y * inverse - 1.0;
        int x = 0;
        for (; x + 32 <= n; x += 32) {
            uint64_t bytes = pixels(x, n, 4, inverse, ci, &inside);
            checksum = (checksum * (base * base * base * base) + bytes) % modulus;
        }
        for (; x < n; x += 8) {
            uint64_t byte = pixels(x, n, 1, inverse, ci, &inside);
            checksum = (checksum * base + byte) % modulus;
        }
    }
    c_extremo_fin(start);
    printf("%llu\n%llu\n", (unsigned long long)inside, (unsigned long long)checksum);
    return EXIT_SUCCESS;
}
#endif
