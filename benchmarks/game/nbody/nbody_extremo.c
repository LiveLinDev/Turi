/* Las posiciones y velocidades permanecen en registros. Cada ronda reúne
   pares disjuntos y conserva el orden de actualización de cada cuerpo. */
#include "../../c_extremo_tiempo.h"
#include "nbody_model.h"
#include <immintrin.h>
#include <stdint.h>

#ifndef __AVX512DQ__
#error Este banco requiere AVX-512DQ
#endif

#pragma STDC FP_CONTRACT OFF

typedef struct {
    __m512i left, right;
    __m512d mass_left, mass_right;
    __mmask8 valid;
} Pairs;

typedef struct {
    int64_t index[8];
    __mmask8 active;
} Round;

/* Cada índice selecciona un delta del extremo izquierdo (0 a 7)
   o derecho (8 a 15) del par. La máscara indica los cuerpos afectados. */
static const Round first_rounds[] = {
    {{0, 8,  0,  0,  0}, 0x03},
    {{1, 0,  9,  0,  0}, 0x05},
    {{2, 4, 12, 10,  0}, 0x0f},
    {{3, 5,  0, 13, 11}, 0x1b},
    {{0, 6,  7, 15, 14}, 0x1e}
};
static const Round last_rounds[] = {
    {{0, 0, 0, 0, 8}, 0x14},
    {{0, 0, 0, 1, 9}, 0x18}
};

static Pairs pair_data(__m512i left, __m512i right, __m512d mass, __mmask8 valid) {
    return (Pairs){left, right, _mm512_permutexvar_pd(left, mass),
                   _mm512_permutexvar_pd(right, mass), valid};
}

static inline __m512d differences(const __m512d p[3], const Pairs *pairs, __m512d dt, __m512d d[3]) {
    for (int axis = 0; axis < 3; axis++) {
        d[axis] = _mm512_sub_pd(_mm512_permutexvar_pd(pairs->left, p[axis]),
                               _mm512_permutexvar_pd(pairs->right, p[axis]));
    }
    __m512d d2 = _mm512_add_pd(_mm512_add_pd(_mm512_mul_pd(d[0], d[0]), _mm512_mul_pd(d[1], d[1])),
                              _mm512_mul_pd(d[2], d[2]));
    return _mm512_maskz_div_pd(pairs->valid, dt, _mm512_mul_pd(d2, _mm512_sqrt_pd(d2)));
}

static inline __m512d apply_rounds(__m512d v, __m512d d, __m512d magnitude, const Pairs *pairs,
                                  const Round *rounds, size_t count) {
    __m512d a = _mm512_xor_pd(_mm512_set1_pd(-0.0),
                             _mm512_mul_pd(_mm512_mul_pd(d, pairs->mass_right), magnitude));
    __m512d b_delta = _mm512_mul_pd(_mm512_mul_pd(d, pairs->mass_left), magnitude);
    for (size_t round = 0; round < count; round++) {
        __m512i index = _mm512_loadu_si512(rounds[round].index);
        __m512d delta = _mm512_permutex2var_pd(a, index, b_delta);
        v = _mm512_mask_add_pd(v, rounds[round].active, v, delta);
    }
    return v;
}

static void advance_vector(long steps, double seconds) {
    __m512d p[3], v[3];
    for (int axis = 0; axis < 3; axis++) {
        p[axis] = _mm512_setr_pd(b[0].x[axis], b[1].x[axis], b[2].x[axis], b[3].x[axis], b[4].x[axis], 0, 0, 0);
        v[axis] = _mm512_setr_pd(b[0].v[axis], b[1].v[axis], b[2].v[axis], b[3].v[axis], b[4].v[axis], 0, 0, 0);
    }
    __m512d dt = _mm512_set1_pd(seconds);
    __m512d mass = _mm512_setr_pd(b[0].mass, b[1].mass, b[2].mass, b[3].mass, b[4].mass, 0, 0, 0);
    Pairs first = pair_data(_mm512_setr_epi64(0, 0, 0, 0, 1, 1, 1, 2),
                            _mm512_setr_epi64(1, 2, 3, 4, 2, 3, 4, 3), mass, 0xff);
#ifndef TZ_CUERPOS_PAR_CORTO
    Pairs last = pair_data(_mm512_setr_epi64(2, 3, 0, 0, 0, 0, 0, 0),
                           _mm512_setr_epi64(4, 4, 1, 1, 1, 1, 1, 1), mass, 0x03);
#endif
#ifdef TZ_CUERPOS_PESO_TEMPRANO
    const __m512i phases[] = {
        _mm512_setr_epi64(0, 0, 1, 2, 3, 0, 0, 0), _mm512_setr_epi64(1, 4, 4, 5, 6, 0, 0, 0),
        _mm512_setr_epi64(2, 5, 7, 7, 8, 0, 0, 0), _mm512_setr_epi64(3, 6, 8, 9, 9, 0, 0, 0),
    };
    const __m512i other[] = {
        _mm512_setr_epi64(1, 0, 0, 0, 0, 0, 0, 0), _mm512_setr_epi64(2, 2, 1, 1, 1, 0, 0, 0),
        _mm512_setr_epi64(3, 3, 3, 2, 2, 0, 0, 0), _mm512_setr_epi64(4, 4, 4, 4, 3, 0, 0, 0),
    };
    __m512d weights[4];
    for (int round = 0; round < 4; round++) {
        weights[round] = _mm512_permutexvar_pd(other[round], mass);
#ifndef TZ_CUERPOS_DELTA_POSICIONES
        __mmask8 negative = (1u << (round + 1)) - 1;
        weights[round] = _mm512_mask_xor_pd(weights[round], negative, weights[round], _mm512_set1_pd(-0.0));
#endif
    }
#endif
    for (long step = 0; step < steps; step++) {
        __m512d first_delta[3], last_delta[3];
        __m512d first_mag = differences(p, &first, dt, first_delta);
#ifdef TZ_CUERPOS_PAR_CORTO
        __m128d short_delta[3];
        for (int axis = 0; axis < 3; axis++) {
            __m128d left = _mm512_extractf64x2_pd(p[axis], 1);
            __m128d right = _mm512_extractf64x2_pd(p[axis], 2);
            short_delta[axis] = _mm_sub_pd(left, _mm_shuffle_pd(right, right, 0));
            last_delta[axis] = _mm512_castpd128_pd512(short_delta[axis]);
        }
        __m128d x = short_delta[0], y = short_delta[1], z = short_delta[2];
        __m128d square = _mm_add_pd(_mm_add_pd(_mm_mul_pd(x, x), _mm_mul_pd(y, y)), _mm_mul_pd(z, z));
        __m128d denominator = _mm_mul_pd(square, _mm_sqrt_pd(square));
        __m512d last_mag = _mm512_castpd128_pd512(_mm_div_pd(_mm_set1_pd(seconds), denominator));
#else
        __m512d last_mag = differences(p, &last, dt, last_delta);
#endif
#ifdef TZ_CUERPOS_PESO_TEMPRANO
        __m512d round_mag[4];
        for (int round = 0; round < 4; round++) {
            round_mag[round] = _mm512_permutex2var_pd(first_mag, phases[round], last_mag);
        }
#endif
        for (int axis = 0; axis < 3; axis++) {
#ifdef TZ_CUERPOS_PESO_TEMPRANO
            for (int round = 0; round < 4; round++) {
#ifdef TZ_CUERPOS_DELTA_POSICIONES
                __m512d delta = _mm512_sub_pd(_mm512_permutexvar_pd(other[round], p[axis]), p[axis]);
#else
                __m512d delta = _mm512_permutex2var_pd(first_delta[axis], phases[round], last_delta[axis]);
#endif
                __m512d impulse = _mm512_mul_pd(_mm512_mul_pd(delta, weights[round]), round_mag[round]);
                v[axis] = _mm512_add_pd(v[axis], impulse);
            }
#elif defined(TZ_CUERPOS_RONDAS)
            __m512d l0, r0, l1, r1;
            l0 = _mm512_xor_pd(_mm512_set1_pd(-0.0),
                    _mm512_mul_pd(_mm512_mul_pd(first_delta[axis], first.mass_right), first_mag));
            r0 = _mm512_mul_pd(_mm512_mul_pd(first_delta[axis], first.mass_left), first_mag);
            l1 = _mm512_xor_pd(_mm512_set1_pd(-0.0),
                    _mm512_mul_pd(_mm512_mul_pd(last_delta[axis], last.mass_right), last_mag));
            r1 = _mm512_mul_pd(_mm512_mul_pd(last_delta[axis], last.mass_left), last_mag);
            __m512i controls0 = _mm512_setr_epi64(0, 8, 9, 10, 11, 0, 0, 0);
            __m512d delta0 = _mm512_permutex2var_pd(l0, controls0, r0);
            v[axis] = _mm512_add_pd(v[axis], delta0);
            __m512i controls1 = _mm512_setr_epi64(1, 4, 12, 13, 14, 0, 0, 0);
            __m512d delta1 = _mm512_permutex2var_pd(l0, controls1, r0);
            v[axis] = _mm512_add_pd(v[axis], delta1);
            __m512i controls2 = _mm512_setr_epi64(2, 5, 7, 15, 8, 0, 0, 0);
            __m512d delta2 = _mm512_permutex2var_pd(l0, controls2, r0);
            delta2 = _mm512_mask_blend_pd(16, delta2, _mm512_permutex2var_pd(l1, controls2, r1));
            v[axis] = _mm512_add_pd(v[axis], delta2);
            __m512i controls3 = _mm512_setr_epi64(3, 6, 0, 1, 9, 0, 0, 0);
            __m512d delta3 = _mm512_permutex2var_pd(l0, controls3, r0);
            delta3 = _mm512_mask_blend_pd(28, delta3, _mm512_permutex2var_pd(l1, controls3, r1));
            v[axis] = _mm512_add_pd(v[axis], delta3);
#else
            v[axis] = apply_rounds(v[axis], first_delta[axis], first_mag, &first, first_rounds, 5);
            v[axis] = apply_rounds(v[axis], last_delta[axis], last_mag, &last, last_rounds, 2);
#endif
            p[axis] = _mm512_add_pd(p[axis], _mm512_mul_pd(dt, v[axis]));
        }
    }
    for (int axis = 0; axis < 3; axis++) {
        double position[8], speed[8];
        _mm512_storeu_pd(position, p[axis]);
        _mm512_storeu_pd(speed, v[axis]);
        for (int i = 0; i < BODY_COUNT; i++) {
            b[i].x[axis] = position[i];
            b[i].v[axis] = speed[i];
        }
    }
}

int main(int argc, char **argv) {
    long steps = argc > 1 ? strtol(argv[1], NULL, 10) : 1000;
    double start = c_extremo_inicio();
    offset_momentum();
    printf("%.9f\n", energy());
    advance_vector(steps, 0.01);
    printf("%.9f\n", energy());
    c_extremo_fin(start);
    return EXIT_SUCCESS;
}
