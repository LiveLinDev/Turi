#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

// MODE 1 = scalar, 2 = array, 3 = per-iteration temporary.
// SAFE 1 enables the same arithmetic and index traps for the measured inputs.
static inline int64_t add(int64_t a, int64_t b) {
#if SAFE
    int64_t out;
    if (__builtin_add_overflow(a, b, &out)) abort();
    return out;
#else
    return a + b;
#endif
}
static inline int64_t mul(int64_t a, int64_t b) {
#if SAFE
    int64_t out;
    if (__builtin_mul_overflow(a, b, &out)) abort();
    return out;
#else
    return a * b;
#endif
}
static inline int64_t *at(int64_t *a, int64_t i, int64_t n) {
#if SAFE
    if ((uint64_t)i >= (uint64_t)n) abort();
#endif
    return a + i;
}
static int64_t *repeat(int64_t value, int64_t n) {
    if (n < 0 || n > (1LL << 24)) abort();
    int64_t *a = malloc(sizeof(*a) * (size_t)n);
    if (n && !a) abort();
    for (int64_t i = 0; i < n; ++i) a[i] = value;
    return a;
}
int main(void) {
    int64_t seed = 0;
    for (int c = getchar(); c >= '0' && c <= '9'; c = getchar())
        seed = add(mul(seed, 10), c - '0');
    int64_t total = 0;
#if MODE == 1
    total = seed;
    for (int64_t i = 0; i < 8000000; ++i)
        total = add(mul(total, 48271), 1) % 2147483647;
#elif MODE == 2
    const int64_t n = 262144;
    int64_t *a = repeat(seed, n);
    for (int64_t i = 0; i < n; ++i) *at(a, i, n) = add(i, seed) % 1009;
    for (int64_t p = 0; p < 32; ++p)
        for (int64_t i = 0; i < n; ++i)
            *at(a, i, n) = add(add(*at(a, i, n), p), 1) % 1009;
    for (int64_t i = 0; i < n; ++i) total = add(total, *at(a, i, n));
    free(a);
#elif MODE == 3
    for (int64_t r = 0; r < 20000; ++r) {
        int64_t n = add(32, add(r, seed) % 2017);
        int64_t *a = repeat(r % 251, n);
        for (int64_t i = 0; i < n; ++i)
            *at(a, i, n) = add(add(*at(a, i, n), i), seed) % 251;
        total = add(total, *at(a, add(r, seed) % n, n));
        free(a);
    }
#else
#error select MODE 1, 2 or 3
#endif
    printf("%" PRId64 "\n", total);
    return 0;
}
