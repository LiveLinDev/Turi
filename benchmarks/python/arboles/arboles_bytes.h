/* Enlaces de un byte para árboles completos. */
typedef uint8_t NodeWord;

static void build16(uint8_t *nodes, int depth) {
    size_t leaves = (size_t)1 << depth;
    __m128i values = _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31);
    const __m128i zero = _mm_setzero_si128();
    const __m128i advance = _mm_set1_epi8(32);
    _mm_storeu_si128((__m128i*)nodes, _mm_insert_epi8(values, 0, 0));
    _mm_storeu_si128((__m128i*)(nodes + leaves), zero);
    for (size_t at = 16; at < leaves; at += 16) {
        values = _mm_add_epi8(values, advance);
        _mm_storeu_si128((__m128i*)(nodes + at), values);
        _mm_storeu_si128((__m128i*)(nodes + leaves + at), zero);
    }
}

static uint64_t count16(const uint8_t *nodes, size_t end) {
    const __m128i zero = _mm_setzero_si128();
    uint64_t internal = 0;
    for (size_t at = 0; at < end; at += 16) {
        __m128i values = _mm_loadu_si128((const __m128i*)(nodes + at));
        uint32_t missing = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(values, zero));
        internal += 16 - (unsigned)__builtin_popcount(missing);
    }
    return 2 * internal + 1;
}

static void build32(uint8_t *nodes, int depth) {
    size_t leaves = (size_t)1 << depth;
    __m256i values = _mm256_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61, 63);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i advance = _mm256_set1_epi8(64);
    _mm256_storeu_si256((__m256i*)nodes, _mm256_insert_epi8(values, 0, 0));
    _mm256_storeu_si256((__m256i*)(nodes + leaves), zero);
    for (size_t at = 32; at < leaves; at += 32) {
        values = _mm256_add_epi8(values, advance);
        _mm256_storeu_si256((__m256i*)(nodes + at), values);
        _mm256_storeu_si256((__m256i*)(nodes + leaves + at), zero);
    }
}

#ifdef TZ_ARBOLES_ACUMULADOS
static uint64_t count32(const uint8_t *nodes, size_t end) {
    const __m256i zero = _mm256_setzero_si256(), bit = _mm256_set1_epi8(1);
    uint64_t internal = 0;
    for (size_t begin = 0; begin < end; ) {
        size_t stop = end - begin < 4096 ? end : begin + 4096;
        __m256i lanes = zero;
        for (size_t at = begin; at < stop; at += 32) {
            __m256i values = _mm256_loadu_si256((const __m256i*)(nodes + at));
            lanes = _mm256_add_epi8(lanes, _mm256_and_si256(values, bit));
        }
        __m256i packed = _mm256_sad_epu8(lanes, zero);
        __m128i sums = _mm_add_epi64(_mm256_castsi256_si128(packed), _mm256_extracti128_si256(packed, 1));
        internal += (uint64_t)_mm_extract_epi64(sums, 0) + (uint64_t)_mm_extract_epi64(sums, 1);
        begin = stop;
    }
    return 2 * internal + 1;
}
#else
static uint64_t count32(const uint8_t *nodes, size_t end) {
    const __m256i zero = _mm256_setzero_si256();
    uint64_t internal = 0;
    for (size_t at = 0; at < end; at += 32) {
        __m256i values = _mm256_loadu_si256((const __m256i*)(nodes + at));
        uint32_t missing = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(values, zero));
        internal += 32 - (unsigned)__builtin_popcount(missing);
    }
    return 2 * internal + 1;
}
#endif

static void build(uint8_t *nodes, int depth) {
    if (depth == 4) build16(nodes, depth);
    else build32(nodes, depth);
}

static uint64_t count_tree(const uint8_t *nodes, int depth) {
    size_t size = (size_t)1 << (depth + 1);
    return depth == 4 ? count16(nodes, size) : count32(nodes, size);
}
