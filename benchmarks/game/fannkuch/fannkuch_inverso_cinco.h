/* Recorrido inverso con fichas de cinco bits y máximo en las hojas. */
#ifdef TZ_FANNKUCH_SIGNOS
static const uint64_t known_sign[] = {
#include "../../../tools/pyfront/permutaciones_signos.inc"
};
#endif

static inline uint64_t partial_missing(uint64_t fields) {
    const uint64_t low = UINT64_C(0x1ef7bdef7bdef);
    return ~(fields + low) & UINT64_C(0x2108421084210);
}

static inline int inverse_weight(uint64_t first, uint64_t unused) {
    unsigned a = first & 15, b = (first >> 5) & 15, c = (first >> 10) & 15;
    unsigned missing = (a == 0) + (b == 0) + (c == 0);
    if (missing >= 2) return 0;
    int left = __builtin_popcountll(unused);
    int sign = ((a > b) ^ (b > c) ^ (a > c)) ? 1 : -1;
    if (!missing) return sign * partial_factorial[left];
    unsigned highest = a > b ? a : b;
    if (highest < c) highest = c;
    unsigned lowest = a + b + c - highest;
    unsigned lower_shift = 9 + lowest * 5;
    unsigned upper_shift = 9 + highest * 5;
    uint64_t between = (UINT64_MAX >> (lower_shift + 1)) & ~(UINT64_MAX >> upper_shift);
    int middle = __builtin_popcountll(unused & between);
    return sign * (left - 2 * middle) * partial_factorial[left - 1];
}

/* Una inversión de longitud k fija esa posición hasta una inversión mayor.
   Con diez fichas la profundidad es como máximo 511 y 10! * 511 cabe en int32. */
static inline uint64_t packed_result(int64_t sum, int best) {
    return ((uint64_t)sum << 32) | (unsigned)best;
}

static uint64_t partial_known(uint64_t direct, uint64_t reverse, uint64_t open, int depth) {
    int64_t sum = 0;
    int best = depth;
    while (1) {
#ifdef TZ_FANNKUCH_SIGNOS
        unsigned head = (unsigned)direct & 32767;
        sum += ((known_sign[head / 64] >> (head & 63)) & 1) ? depth : -depth;
#else
        unsigned a = direct & 15, b = (direct >> 5) & 15, c = (direct >> 10) & 15;
        sum += ((a > b) ^ (b > c) ^ (a > c)) ? depth : -depth;
#endif
#ifdef TZ_FANNKUCH_IDENTIDAD_COMPLETA
        uint64_t possible = partial_missing(direct ^ UINT64_C(0x22190a63a12f));
#else
        uint64_t possible = open & partial_missing(direct ^ UINT64_C(0x22190a63a12a));
#endif
        if (!possible) {
            if (depth > best) best = depth;
            return packed_result(sum, best);
        }
        ++depth;
        while (1) {
            unsigned shift = 63 - (unsigned)__builtin_ctzll(possible);
            uint64_t next = (direct & ~(UINT64_MAX >> shift)) | (reverse >> shift);
            uint64_t previous = (reverse & ~(UINT64_MAX << shift)) | (direct << shift);
            possible &= possible - 1;
            if (!possible) {
                direct = next;
                reverse = previous;
                break;
            }
            uint64_t branch = partial_known(next, previous, open, depth);
            sum += (int64_t)branch >> 32;
            if ((int)(uint32_t)branch > best) best = (int)(uint32_t)branch;
        }
    }
}

static uint64_t partial_inverse(uint64_t direct, uint64_t reverse, uint64_t open, uint64_t unused,
                               int depth) {
    int64_t sum = 0;
    int best = depth;
    while (1) {
        if (__builtin_popcountll(unused) == 2) {
            uint64_t missing = open & partial_missing(direct);
            unsigned left = (unsigned)__builtin_ctzll(missing) - 4;
            unsigned right = (unsigned)__builtin_ctzll(missing & (missing - 1)) - 4;
            uint64_t first = (UINT64_C(0x22190a63a12a) >> (__builtin_ctzll(unused) - 4)) & 15;
            uint64_t second = (UINT64_C(0x22190a63a12a) >> (__builtin_ctzll(unused & (unused - 1)) - 4)) & 15;
            uint64_t ab = partial_known(direct | (first << left) | (second << right),
                                        reverse | (first << (59 - left)) | (second << (59 - right)), open, depth);
            uint64_t ba = partial_known(direct | (second << left) | (first << right),
                                        reverse | (second << (59 - left)) | (first << (59 - right)), open, depth);
            sum += ((int64_t)ab >> 32) + ((int64_t)ba >> 32);
            if ((int)(uint32_t)ab > best) best = (int)(uint32_t)ab;
            if ((int)(uint32_t)ba > best) best = (int)(uint32_t)ba;
            return packed_result(sum, best);
        }
        if (!(unused & (unused - 1))) {
            if (unused) {
                unsigned position = (unsigned)__builtin_ctzll(open & partial_missing(direct)) - 4;
                uint64_t value = (UINT64_C(0x22190a63a12a) >> (__builtin_ctzll(unused) - 4)) & 15;
                direct |= value << position;
                reverse |= value << (59 - position);
            }
            uint64_t branch = partial_known(direct, reverse, open, depth);
            if ((int)(uint32_t)branch > best) best = (int)(uint32_t)branch;
            return packed_result(sum + ((int64_t)branch >> 32), best);
        }
        sum += (int64_t)depth * inverse_weight(direct, unused);
#ifdef TZ_FANNKUCH_IDENTIDAD_PARCIAL
        uint64_t positions = partial_missing(direct ^ UINT64_C(0x22190a63a12f)) | (partial_missing(direct) & unused);
#else
        uint64_t positions = open & (partial_missing(direct ^ UINT64_C(0x22190a63a12a)) | (partial_missing(direct) & unused));
#endif
        if (!positions) {
            if (depth > best) best = depth;
            return packed_result(sum, best);
        }
        ++depth;
        while (1) {
            unsigned position = (unsigned)__builtin_ctzll(positions);
            unsigned shift = 63 - position;
            uint64_t value = (UINT64_C(0x22190a63a12a) >> (position - 4)) & 15;
            uint64_t next = (direct & ~(UINT64_MAX >> shift)) | (reverse >> shift) | value;
            uint64_t previous = (reverse & ~(UINT64_MAX << shift)) | (direct << shift) | (value << 59);
            uint64_t tail = positions & (positions - 1);
            uint64_t left = unused & ~(positions ^ tail);
            if (!tail) {
                direct = next;
                reverse = previous;
                unused = left;
                break;
            }
            positions = tail;
            uint64_t branch = partial_inverse(next, previous, open, left, depth);
            sum += (int64_t)branch >> 32;
            if ((int)(uint32_t)branch > best) best = (int)(uint32_t)branch;
        }
    }
}

