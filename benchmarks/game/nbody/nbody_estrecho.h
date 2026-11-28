/* Cuatro cuerpos por eje. El quinto se conserva como un vector xyz. */
static inline __m256d first_phase(__m256d middle) {
    return _mm256_permute4x64_pd(middle, _MM_SHUFFLE(3, 1, 0, 0));
}

static inline __m256d second_phase(__m256d middle, __m128d end) {
    __m256d same = _mm256_permute4x64_pd(middle, _MM_SHUFFLE(0, 2, 2, 1));
    return _mm256_blend_pd(same, _mm256_broadcastsd_pd(end), 8);
}

static inline __m256d third_phase(__m256d middle, __m128d end) {
    __m256d spread = _mm256_permute4x64_pd(_mm256_castpd128_pd256(end), _MM_SHUFFLE(1, 1, 0, 0));
    return _mm256_blend_pd(spread, _mm256_permute4x64_pd(middle, _MM_SHUFFLE(3, 3, 3, 3)), 1);
}

static inline __m256d magnitude_four(const __m256d d[3], __m256d dt) {
    __m256d square = _mm256_add_pd(_mm256_add_pd(_mm256_mul_pd(d[0], d[0]), _mm256_mul_pd(d[1], d[1])),
                                  _mm256_mul_pd(d[2], d[2]));
    return _mm256_div_pd(dt, _mm256_mul_pd(square, _mm256_sqrt_pd(square)));
}

static inline __m128d magnitude_two(const __m128d d[3], __m128d dt) {
    __m128d square = _mm_add_pd(_mm_add_pd(_mm_mul_pd(d[0], d[0]), _mm_mul_pd(d[1], d[1])), _mm_mul_pd(d[2], d[2]));
    return _mm_div_pd(dt, _mm_mul_pd(square, _mm_sqrt_pd(square)));
}

static inline __m256d impulse(__m256d speed, __m256d delta, __m256d mass, __m256d magnitude) {
    return _mm256_add_pd(speed, _mm256_mul_pd(_mm256_mul_pd(delta, mass), magnitude));
}

static void advance_narrow(long steps, double seconds) {
    __m256d p[3], v[3];
    for (int axis = 0; axis < 3; ++axis) {
        p[axis] = _mm256_setr_pd(b[0].x[axis], b[1].x[axis], b[2].x[axis], b[3].x[axis]);
        v[axis] = _mm256_setr_pd(b[0].v[axis], b[1].v[axis], b[2].v[axis], b[3].v[axis]);
    }
    __m256d fifth = _mm256_setr_pd(b[4].x[0], b[4].x[1], b[4].x[2], 0.0);
    __m256d speed = _mm256_setr_pd(b[4].v[0], b[4].v[1], b[4].v[2], 0.0);
    double m0 = b[0].mass, m1 = b[1].mass, m2 = b[2].mass, m3 = b[3].mass, m4 = b[4].mass;
    const __m256d weights0 = _mm256_setr_pd(-m1, m0, m0, m0);
    const __m256d weights1 = _mm256_setr_pd(-m2, -m2, m1, m1);
    const __m256d weights2 = _mm256_setr_pd(-m3, -m3, -m3, m2);
    const __m256d weights3 = _mm256_set1_pd(-m4);
    const __m256d mass0 = _mm256_set1_pd(m0), mass1 = _mm256_set1_pd(m1);
    const __m256d mass2 = _mm256_set1_pd(m2), mass3 = _mm256_set1_pd(m3);
    const __m256d dt = _mm256_set1_pd(seconds), zero = _mm256_setzero_pd();
    for (long step = 0; step < steps; ++step) {
        __m256d d0[3], d1[3];
        __m128d d2[3];
        d0[0] = _mm256_sub_pd(p[0], _mm256_permute4x64_pd(fifth, _MM_SHUFFLE(0, 0, 0, 0)));
        d0[1] = _mm256_sub_pd(p[1], _mm256_permute4x64_pd(fifth, _MM_SHUFFLE(1, 1, 1, 1)));
        d0[2] = _mm256_sub_pd(p[2], _mm256_permute4x64_pd(fifth, _MM_SHUFFLE(2, 2, 2, 2)));
#pragma clang loop unroll(full)
        for (int axis = 0; axis < 3; ++axis) {
            d1[axis] = _mm256_sub_pd(_mm256_permute4x64_pd(p[axis], _MM_SHUFFLE(0, 1, 0, 0)),
                                    _mm256_permute4x64_pd(p[axis], _MM_SHUFFLE(3, 2, 2, 1)));
            __m128d mid = _mm256_castpd256_pd128(_mm256_permute4x64_pd(p[axis], _MM_SHUFFLE(0, 0, 2, 1)));
            __m128d high = _mm256_extractf128_pd(p[axis], 1);
            d2[axis] = _mm_sub_pd(mid, _mm_shuffle_pd(high, high, 3));
        }
        __m256d mag0 = magnitude_four(d0, dt), mag1 = magnitude_four(d1, dt);
        __m128d mag2 = magnitude_two(d2, _mm256_castpd256_pd128(dt));
        __m256d r0 = first_phase(mag1), r1 = second_phase(mag1, mag2), r2 = third_phase(mag1, mag2);
#pragma clang loop unroll(full)
        for (int axis = 0; axis < 3; ++axis) {
            v[axis] = impulse(v[axis], first_phase(d1[axis]), weights0, r0);
            v[axis] = impulse(v[axis], second_phase(d1[axis], d2[axis]), weights1, r1);
            v[axis] = impulse(v[axis], third_phase(d1[axis], d2[axis]), weights2, r2);
            v[axis] = impulse(v[axis], d0[axis], weights3, mag0);
            p[axis] = _mm256_add_pd(p[axis], _mm256_mul_pd(dt, v[axis]));
        }
        __m256d xy_even = _mm256_unpacklo_pd(d0[0], d0[1]), xy_odd = _mm256_unpackhi_pd(d0[0], d0[1]);
        __m256d z_even = _mm256_unpacklo_pd(d0[2], zero), z_odd = _mm256_unpackhi_pd(d0[2], zero);
        speed = impulse(speed, _mm256_permute2f128_pd(xy_even, z_even, 0x20), mass0,
                        _mm256_permute4x64_pd(mag0, _MM_SHUFFLE(0, 0, 0, 0)));
        speed = impulse(speed, _mm256_permute2f128_pd(xy_odd, z_odd, 0x20), mass1,
                        _mm256_permute4x64_pd(mag0, _MM_SHUFFLE(1, 1, 1, 1)));
        speed = impulse(speed, _mm256_permute2f128_pd(xy_even, z_even, 0x31), mass2,
                        _mm256_permute4x64_pd(mag0, _MM_SHUFFLE(2, 2, 2, 2)));
        speed = impulse(speed, _mm256_permute2f128_pd(xy_odd, z_odd, 0x31), mass3,
                        _mm256_permute4x64_pd(mag0, _MM_SHUFFLE(3, 3, 3, 3)));
        fifth = _mm256_add_pd(fifth, _mm256_mul_pd(dt, speed));
    }
    double x4[4], v4[4];
    _mm256_storeu_pd(x4, fifth);
    _mm256_storeu_pd(v4, speed);
    for (int axis = 0; axis < 3; ++axis) {
        double position[4], velocity[4];
        _mm256_storeu_pd(position, p[axis]);
        _mm256_storeu_pd(velocity, v[axis]);
        for (int i = 0; i < 4; ++i) {
            b[i].x[axis] = position[i];
            b[i].v[axis] = velocity[i];
        }
        b[4].x[axis] = x4[axis];
        b[4].v[axis] = v4[axis];
    }
}
