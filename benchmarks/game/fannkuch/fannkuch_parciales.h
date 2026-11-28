/* Cada ficha guarda el desplazamiento de su giro en seis bits. */
static const int partial_factorial[] = {1, 1, 2, 6, 24, 120, 720, 5040};

static int64_t partial_leaf(uint64_t assigned, uint32_t unused, int depth) {
    unsigned a = assigned & 63, b = (assigned >> 6) & 63, c = (assigned >> 12) & 63;
    unsigned missing = (a == 0) + (b == 0) + (c == 0);
    if (missing >= 2) return 0;
    int left = __builtin_popcount(unused);
    int sign = ((a > b) ^ (b > c) ^ (a > c)) ? 1 : -1;
    if (!missing) return (int64_t)sign * depth * partial_factorial[left];
    unsigned high = a > b ? a : b;
    if (c > high) high = c;
    unsigned low = a + b + c - high;
    unsigned largest = (64 - low) / 6, smallest = (64 - high) / 6;
    uint32_t interior = (1u << (largest - 1)) - 1, lower = (1u << smallest) - 1;
    int middle = __builtin_popcount(unused & interior & ~lower);
    return (int64_t)sign * depth * (left - 2 * middle) * partial_factorial[left - 1];
}

#ifdef TZ_FANNKUCH_FICHAS
/* Los desplazamientos de giro son pares. Las posiciones sin valor son impares:
   1, 3 y 5 guardan el signo, y 7 representa las demás posiciones. */
static inline unsigned resolved_shift(uint64_t direct, uint64_t assigned) {
    (void)assigned;
    unsigned field = direct & 63;
    return field & 1 ? 0 : field;
}

static inline unsigned sign_slot(uint64_t direct) { return 3 * (direct & 6); }

static inline void insert_after_turn(uint64_t direct, unsigned shift, uint64_t *next, uint64_t *previous) {
    uint64_t changed = (direct & 63) ^ shift;
    *next ^= changed << (58 - shift);
    *previous ^= changed << shift;
}
#else
static inline unsigned resolved_shift(uint64_t direct, uint64_t assigned) { return (assigned >> (direct & 63)) & 63; }
static inline unsigned sign_slot(uint64_t direct) { return direct & 63; }
#endif

#ifdef TZ_FANNKUCH_SIGNO_FIJO
/* Con las tres primeras fichas fijadas, cada término comparte el signo. */
static int64_t partial_unsigned(uint64_t direct, uint64_t reverse, uint64_t assigned,
                                uint32_t unused, int depth, int *maximum) {
    int64_t sum = 0;
    while (1) {
        unsigned shift = resolved_shift(direct, assigned);
        if (shift) {
            uint64_t next = (direct & ~(UINT64_MAX >> shift)) | (reverse >> shift);
            reverse = (reverse & ~(UINT64_MAX << shift)) | (direct << shift);
            direct = next;
            ++depth;
            continue;
        }
        uint32_t pending = unused & ~1u;
        sum += (int64_t)depth * partial_factorial[__builtin_popcount(pending)];
        if (depth > *maximum) *maximum = depth;
        if (!pending) return sum;
        unsigned slot = sign_slot(direct);
        ++depth;
        while (pending) {
            shift = 58 - 6 * (unsigned)__builtin_ctz(pending);
            uint64_t next = (direct & ~(UINT64_MAX >> shift)) | (reverse >> shift);
            uint64_t previous = (reverse & ~(UINT64_MAX << shift)) | (direct << shift);
#ifdef TZ_FANNKUCH_FICHAS
            insert_after_turn(direct, shift, &next, &previous);
#endif
            uint32_t used = pending & -pending;
            pending ^= used;
            uint64_t values = assigned | ((uint64_t)shift << slot);
            uint32_t left = unused ^ used;
            if (!pending) {
                direct = next;
                reverse = previous;
                assigned = values;
                unused = left;
                break;
            }
            sum += partial_unsigned(next, previous, values, left, depth, maximum);
        }
    }
}
#endif

static int64_t partial(uint64_t direct, uint64_t reverse, uint64_t assigned, uint32_t unused, int depth,
                    int *maximum) {
    int64_t sum = 0;
    while (1) {
        unsigned shift = resolved_shift(direct, assigned);
        if (shift) {
            uint64_t next = (direct & ~(UINT64_MAX >> shift)) | (reverse >> shift);
            reverse = (reverse & ~(UINT64_MAX << shift)) | (direct << shift);
            direct = next;
            ++depth;
            continue;
        }
#ifdef TZ_FANNKUCH_SIGNO_FIJO
        /* Una resta por campo detecta si alguna de las tres fichas aún es cero. */
        if (((assigned - UINT64_C(0x1041)) & ~assigned & UINT64_C(0x20820)) == 0) {
            unsigned a = assigned & 63, b = (assigned >> 6) & 63, c = (assigned >> 12) & 63;
            int64_t total = partial_unsigned(direct, reverse, assigned, unused, depth, maximum);
            return ((a > b) ^ (b > c) ^ (a > c)) ? sum + total : sum - total;
        }
#endif
        unsigned slot = sign_slot(direct);
        uint32_t pending = unused & ~1u;
        sum += partial_leaf(assigned | (UINT64_C(58) << slot), pending, depth);
        if (depth > *maximum) *maximum = depth;
        if (!pending) return sum;
        ++depth;
        while (pending) {
            shift = 58 - 6 * (unsigned)__builtin_ctz(pending);
            uint64_t next = (direct & ~(UINT64_MAX >> shift)) | (reverse >> shift);
            uint64_t previous = (reverse & ~(UINT64_MAX << shift)) | (direct << shift);
#ifdef TZ_FANNKUCH_FICHAS
            insert_after_turn(direct, shift, &next, &previous);
#endif
            uint32_t used = pending & -pending;
            pending ^= used;
            uint64_t values = assigned | ((uint64_t)shift << slot);
            uint32_t left = unused ^ used;
            if (!pending) {
                direct = next;
                reverse = previous;
                assigned = values;
                unused = left;
                break;
            }
            sum += partial(next, previous, values, left, depth, maximum);
        }
    }
}

#ifdef TZ_FANNKUCH_INVERSO_PARCIAL
#ifdef TZ_FANNKUCH_CINCO_BITS
#include "fannkuch_inverso_cinco.h"
#else
static inline uint64_t partial_missing(uint64_t fields) {
    const uint64_t low = UINT64_C(0x7df7df7df7df7df);
    return ~(((fields & low) + low) | fields) & UINT64_C(0x820820820820820);
}

static inline int inverse_weight(uint64_t first, uint64_t unused) {
    unsigned a = first & 63, b = (first >> 6) & 63, c = (first >> 12) & 63;
    unsigned missing = (a == 0) + (b == 0) + (c == 0);
    if (missing >= 2) return 0;
    int left = __builtin_popcountll(unused);
    int sign = ((a > b) ^ (b > c) ^ (a > c)) ? 1 : -1;
    if (!missing) return sign * partial_factorial[left];
    unsigned highest = a > b ? a : b;
    if (highest < c) highest = c;
    unsigned lowest = a + b + c - highest;
    uint64_t between = (UINT64_MAX >> (lowest + 1)) & ~(UINT64_MAX >> highest);
    int middle = __builtin_popcountll(unused & between);
    return sign * (left - 2 * middle) * partial_factorial[left - 1];
}

#ifdef TZ_FANNKUCH_UNA_PENDIENTE
static int64_t partial_known(uint64_t direct, uint64_t reverse, uint64_t open, int depth, int *maximum) {
    int64_t sum = 0;
    while (1) {
        unsigned a = direct & 63, b = (direct >> 6) & 63, c = (direct >> 12) & 63;
        sum += ((a > b) ^ (b > c) ^ (a > c)) ? depth : -depth;
#ifndef TZ_FANNKUCH_DOS_PENDIENTES
        if (depth > *maximum) *maximum = depth;
#endif
        uint64_t possible = open & partial_missing(direct ^ UINT64_C(0x010a416722a2ed3a));
        if (!possible) {
#ifdef TZ_FANNKUCH_DOS_PENDIENTES
            if (depth > *maximum) *maximum = depth;
#endif
            return sum;
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
            sum += partial_known(next, previous, open, depth, maximum);
        }
    }
}
#endif

static int64_t partial_inverse(uint64_t direct, uint64_t reverse, uint64_t open, uint64_t unused,
                               int depth, int *maximum) {
    int64_t sum = 0;
    while (1) {
#ifdef TZ_FANNKUCH_UNA_PENDIENTE
#ifdef TZ_FANNKUCH_DOS_PENDIENTES
        if (__builtin_popcountll(unused) == 2) {
            uint64_t missing = open & partial_missing(direct);
            unsigned left = (unsigned)__builtin_ctzll(missing) - 5;
            unsigned right = (unsigned)__builtin_ctzll(missing & (missing - 1)) - 5;
            uint64_t first = 63 - (unsigned)__builtin_ctzll(unused);
            uint64_t second = 63 - (unsigned)__builtin_ctzll(unused & (unused - 1));
            sum += partial_known(direct | (first << left) | (second << right),
                                 reverse | (first << (58 - left)) | (second << (58 - right)), open, depth, maximum);
            return sum + partial_known(direct | (second << left) | (first << right),
                                       reverse | (second << (58 - left)) | (first << (58 - right)), open, depth, maximum);
        }
#endif
        if (!(unused & (unused - 1))) {
            if (unused) {
                unsigned position = (unsigned)__builtin_ctzll(open & partial_missing(direct)) - 5;
                uint64_t shift = 63 - (unsigned)__builtin_ctzll(unused);
                direct |= shift << position;
                reverse |= shift << (58 - position);
            }
            return sum + partial_known(direct, reverse, open, depth, maximum);
        }
#endif
        sum += (int64_t)depth * inverse_weight(direct, unused);
#ifndef TZ_FANNKUCH_DOS_PENDIENTES
        if (depth > *maximum) *maximum = depth;
#endif
        uint64_t positions = open & (partial_missing(direct ^ UINT64_C(0x10a416722a2ed3a)) | (partial_missing(direct) & unused));
        if (!positions) {
#ifdef TZ_FANNKUCH_DOS_PENDIENTES
            if (depth > *maximum) *maximum = depth;
#endif
            return sum;
        }
        ++depth;
        while (1) {
            unsigned shift = 63 - (unsigned)__builtin_ctzll(positions);
            uint64_t next = (direct & ~(UINT64_MAX >> shift)) | (reverse >> shift) | shift;
            uint64_t previous = (reverse & ~(UINT64_MAX << shift)) | (direct << shift) | ((uint64_t)shift << 58);
            uint64_t tail = positions & (positions - 1);
            uint64_t left = unused & ~(positions ^ tail);
            if (!tail) {
                direct = next;
                reverse = previous;
                unused = left;
                break;
            }
            positions = tail;
            sum += partial_inverse(next, previous, open, left, depth, maximum);
        }
    }
}
#endif
#endif
