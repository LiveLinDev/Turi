/* Búsqueda con máscaras de columnas y diagonales. Se puede reflejar la
   segunda fila de la rama central y cerrar directamente las dos últimas. */
#include "../../c_extremo_tiempo.h"
#include <stdint.h>

#ifdef TZ_REINAS_CUATRO
#include "reinas_cuatro.h"
#endif
#ifdef TZ_REINAS_CINCO
#include "reinas_cinco.h"
#endif

static uint64_t search(uint32_t cols, uint32_t left, uint32_t right, int remaining) {
#ifdef TZ_REINAS_TRES
    uint32_t available = cols & ~(left | right);
#ifdef TZ_REINAS_CINCO
    if (remaining == 5 && cols < 4096) return closed_five(cols, left, right);
#endif
#ifdef TZ_REINAS_CUATRO
    if (remaining == 4 && cols < 65536) return closed_four(cols, left, right);
#endif
    if (remaining == 3) {
        uint32_t middle = cols & ~((left << 1) | (right >> 1));
        uint32_t end = cols & ~((left << 2) | (right >> 2));
        uint64_t total = 0;
        while (available) {
            uint32_t queen = available & (0u - available);
            available &= available - 1;
            uint32_t two = cols ^ queen;
            uint32_t low = two & (0u - two), high = two ^ low;
            if (high != (low << 1)) {
                uint32_t a = middle & two & ~((queen << 1) | (queen >> 1));
                uint32_t b = end & two & ~((queen << 2) | (queen >> 2));
                total += __builtin_popcount(a) * __builtin_popcount(b) - __builtin_popcount(a & b);
            }
        }
        return total;
    }
#elif defined(TZ_REINAS_FINAL)
    uint32_t available = cols & ~(left | right);
    if (remaining == 2) {
        if (!available) return 0;
        uint32_t first = cols & (0u - cols), second = cols ^ first;
        if (second == (first << 1)) return 0;
        uint32_t next = cols & ~((left << 1) | (right >> 1));
        return (!!(available & first) & !!(next & second)) + (!!(available & second) & !!(next & first));
    }
#else
    if (remaining == 0) return 1;
    uint32_t available = cols & ~(left | right);
    if (remaining == 1) return available != 0;
#endif
    uint64_t total = 0;
    while (available) {
        uint32_t queen = available & (0u - available);
        available &= available - 1;
        uint32_t next_cols = cols ^ queen;
        uint32_t next_left = (left | queen) << 1;
        uint32_t next_right = (right | queen) >> 1;
#if !defined(TZ_REINAS_FINAL) && !defined(TZ_REINAS_TRES)
        if (remaining == 2) {
            total += (next_cols & ~(next_left | next_right)) != 0;
            continue;
        }
#endif
        total += search(next_cols, next_left, next_right, remaining - 1);
    }
    return total;
}

static uint64_t count_solutions(int n) {
    if (n < 4) return n == 0 || n == 1;
    uint32_t cols = (UINT32_C(1) << n) - 1;
    uint64_t half = 0;
    for (int col = 0; col < n / 2; col++) {
        uint32_t queen = UINT32_C(1) << col;
        half += search(cols ^ queen, queen << 1, queen >> 1, n - 1);
    }
    uint64_t total = 2 * half;
    if (n % 2) {
        uint32_t center = UINT32_C(1) << (n / 2);
#ifdef TZ_REINAS_SIMETRIA
        uint32_t left = center << 1, right = center >> 1, rest = cols ^ center;
        uint32_t available = (center - 1) & rest & ~(left | right);
        while (available) {
            uint32_t queen = available & (0u - available);
            available &= available - 1;
            total += 2 * search(rest ^ queen, (left | queen) << 1, (right | queen) >> 1, n - 2);
        }
#else
        total += search(cols ^ center, center << 1, center >> 1, n - 1);
#endif
    }
    return total;
}

int main(int argc, char **argv) {
    long requested = argc > 1 ? strtol(argv[1], NULL, 10) : 8;
    if (requested > 20) return EXIT_FAILURE;
    int n = requested < 0 ? -1 : (int)requested;
    double start = c_extremo_inicio();
    uint64_t count = count_solutions(n);
    c_extremo_fin(start);
    printf("%llu\n", (unsigned long long)count);
    return EXIT_SUCCESS;
}
