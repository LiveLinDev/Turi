/* Dos filas comparten los tramos certificados y los vectores del eje real. */
static inline __attribute__((always_inline))
void pixels_pair(int x, int n, int vectors, double inverse, double a, double b,
                 uint64_t *inside, uint64_t bytes[2]) {
    const __m512d inv = _mm512_set1_pd(inverse), offset = _mm512_set1_pd(1.5);
    const __m512d lanes = _mm512_setr_pd(0, 1, 2, 3, 4, 5, 6, 7);
    const __m512d cy[2] = {_mm512_set1_pd(a), _mm512_set1_pd(b)};
    const __m512d escape = _mm512_set1_pd(12.5), membership = _mm512_set1_pd(4.0);
    __m512d cx[2], r[2][2], i[2][2];
    bytes[0] = bytes[1] = 0;
    for (int k = 0; k < vectors; ++k) {
        __m512d col = _mm512_add_pd(_mm512_set1_pd(x + 8 * k), lanes);
        cx[k] = _mm512_sub_pd(_mm512_mul_pd(col, inv), offset);
        r[0][k] = i[0][k] = r[1][k] = i[1][k] = _mm512_setzero_pd();
    }
    for (int step = 0; step < 50; ++step) {
        for (int k = 0; k < vectors; ++k) {
            for (int side = 0; side < 2; ++side) {
                __m512d re = r[side][k], im = i[side][k];
                r[side][k] = _mm512_add_pd(_mm512_sub_pd(_mm512_mul_pd(re, re), _mm512_mul_pd(im, im)), cx[k]);
                i[side][k] = _mm512_add_pd(_mm512_mul_pd(_mm512_add_pd(re, re), im), cy[side]);
            }
        }
        if (step == 4 || step % 10 == 9) {
            __mmask8 live = 0;
            for (int k = 0; k < vectors; ++k) {
                for (int side = 0; side < 2; ++side) {
                    __m512d re = r[side][k], im = i[side][k];
                    __m512d norm = _mm512_add_pd(_mm512_mul_pd(re, re), _mm512_mul_pd(im, im));
                    live |= _mm512_cmp_pd_mask(norm, escape, _CMP_LE_OQ);
                }
            }
            if (!live) return;
        }
    }
    for (int k = 0; k < vectors; ++k) {
        for (int side = 0; side < 2; ++side) {
            __m512d re = r[side][k], im = i[side][k];
            __m512d norm = _mm512_add_pd(_mm512_mul_pd(re, re), _mm512_mul_pd(im, im));
            unsigned mask = _mm512_cmp_pd_mask(norm, membership, _CMP_LE_OQ);
            int remaining = n - x - 8 * k;
            if (remaining < 8) mask &= (1u << remaining) - 1u;
            *inside += (unsigned)__builtin_popcount(mask);
            bytes[side] = 31 * bytes[side] + __builtin_bitreverse8((uint8_t)mask);
        }
    }
}

static void paired_gap(int x, int end, int n, double inverse, double a, double b, uint64_t *inside,
                       uint64_t checksum[2]) {
    uint64_t bytes[2];
    for (; x + 16 <= end; x += 16) {
        pixels_pair(x, n, 2, inverse, a, b, inside, bytes);
        for (int side = 0; side < 2; ++side)
            checksum[side] = (checksum[side] * 961 + bytes[side]) % 1000000007;
    }
    for (; x < end; x += 8) {
        pixels_pair(x, n, 1, inverse, a, b, inside, bytes);
        for (int side = 0; side < 2; ++side)
            checksum[side] = (checksum[side] * 31 + bytes[side]) % 1000000007;
    }
}

static void paired_row(int n, double inverse, double a, double b, unsigned band, uint64_t *inside,
                       uint64_t checksum[2]) {
    checksum[0] = checksum[1] = 0;
    int x = 0;
    for (unsigned run = zones[band]; run < zones[band + 1]; ++run) {
        unsigned at = REGION_RESOLUTION + 1 + 2 * run;
        unsigned start = zones[at], last = zones[at + 1];
        int first = (int)(region_bounds[start] & UINT32_MAX);
        int stop = (int)(region_bounds[last >> 1] >> 32);
        if (first < x) first = x;
        if (first >= stop) continue;
        paired_gap(x, first, n, inverse, a, b, inside, checksum);
        unsigned width = (unsigned)(stop - first) / 8;
        uint64_t factor = width < sizeof(powers) / sizeof(powers[0]) ? powers[width] & UINT32_MAX : factor_of(width);
        uint64_t bytes = 0;
        if (last & 1) {
            *inside += (uint64_t)(2 * (stop - first));
            bytes = width < sizeof(powers) / sizeof(powers[0]) ? powers[width] >> 32 :
                UINT64_C(255) * ((factor + 1000000006) * 233333335 % 1000000007);
        }
        for (int side = 0; side < 2; ++side)
            checksum[side] = (checksum[side] * factor + bytes) % 1000000007;
        x = stop;
    }
    paired_gap(x, n, n, inverse, a, b, inside, checksum);
}

static unsigned paired_band(double imaginary) {
    unsigned band = (unsigned)(fabs(imaginary) * REGION_RESOLUTION);
    return band < REGION_RESOLUTION ? band : REGION_RESOLUTION - 1;
}

static void mirrored_rows(int n, double inverse, double a, double b, uint64_t *inside, uint64_t checksum[2]) {
    if (a == -b) {
        uint64_t count = 0;
        checksum[0] = checksum[1] = row(n, inverse, a, &count);
        *inside += 2 * count;
        return;
    }
    unsigned band = paired_band(a);
    if (band == paired_band(b)) {
        paired_row(n, inverse, a, b, band, inside, checksum);
    } else {
        checksum[0] = row(n, inverse, a, inside);
        checksum[1] = row(n, inverse, b, inside);
    }
}
