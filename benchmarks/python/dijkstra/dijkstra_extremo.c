/* Los pesos son enteros entre 1 y 10. Once cubetas circulares bastan para
   extraer por distancia. Cada arista entrante en una celda tiene el mismo
   coste, así que la primera inserción ya fija su distancia mínima. */
#include "../../c_extremo_tiempo.h"
#include <stdint.h>

#ifdef TZ_DIJKSTRA_DIAGONALES
/* El peso solo depende de fila-columna. Los desvíos restantes en un
   camino mínimo son ciclos de coste 5, realizables junto al origen. */
int main(int argc, char **argv) {
    long input = argc > 1 ? strtol(argv[1], NULL, 10) : 100;
    if (input < 1 || input > 32768) return EXIT_FAILURE;
    double start = c_extremo_inicio();
    int64_t n = input, m = n - 1, total;
#ifdef TZ_DIJKSTRA_LINEAL
    total = 5 * n * m / 2;
    for (int64_t delta = 1; delta < n; delta++) {
        int64_t cells = n - delta;
        total += cells * (12 * delta - 10 * (delta / 10) + 5 * (cells - 1));
    }
#else
    int64_t full = m / 10;
    int64_t full_sum = full * (full + 1) / 2;
    int64_t full_squares = full * (full + 1) * (2 * full + 1) / 6;
    int64_t squares = m * n * (2 * m + 1) / 6;
    int64_t diagonals = n * m * (n + 1) / 6;
    int64_t repeated = (full * n * (n + 1) - 10 * (2 * n + 1) * full_sum + 100 * full_squares) / 2;
    total = 12 * diagonals - 10 * repeated + 5 * squares;
#endif
    c_extremo_pareja("%lld %lld\n", (long long)(5 * m), (long long)total);
    c_extremo_fin(start);
    return EXIT_SUCCESS;
}
#else
enum { BUCKETS = 11 };

static inline int visit(uint8_t *weights, uint32_t *next, uint32_t *heads,
                        uint32_t cell, unsigned bucket) {
    unsigned weight = weights[cell];
    if (weight == 0) return 0;
    weights[cell] = 0;
    unsigned slot = bucket + weight;
    if (slot >= BUCKETS) slot -= BUCKETS;
    next[cell] = heads[slot];
    heads[slot] = cell;
    return 1;
}

int main(int argc, char **argv) {
    long requested = argc > 1 ? strtol(argv[1], NULL, 10) : 100;
    if (requested < 1 || requested > 32768) return EXIT_FAILURE;
    unsigned n = (unsigned)requested;
    unsigned stride = n + 2;
    size_t cells = (size_t)stride * stride;
    if (cells > SIZE_MAX / sizeof(uint32_t)) return EXIT_FAILURE;
    double start = c_extremo_inicio();
    uint8_t *weights = calloc(cells, sizeof(*weights));
    uint32_t *next = malloc(cells * sizeof(*next));
    if (!weights || !next) {
        free(weights);
        free(next);
        return EXIT_FAILURE;
    }
    /* El borde ya tiene peso cero. Un vecino nulo o visitado se descarta igual. */
    for (unsigned row = 0; row < n; row++) {
        uint8_t *line = weights + (size_t)(row + 1) * stride + 1;
        for (unsigned col = 0; col < n; col++) {
            line[col] = (uint8_t)((row * 7 + col * 13) % 10 + 1);
        }
    }
    uint32_t first = stride + 1, target = n * stride + n;
    uint32_t heads[BUCKETS] = {first};
    weights[first] = 0;
    next[first] = 0;
    size_t pending = 1;
    unsigned bucket = 0, distance = 0, last = 0;
    uint64_t total = 0;
    while (pending) {
        while (!heads[bucket]) {
            if (++bucket == BUCKETS) bucket = 0;
            distance++;
        }
        uint32_t cell = heads[bucket];
        heads[bucket] = next[cell];
        pending--;
        total += distance;
        if (cell == target) last = distance;
        pending += visit(weights, next, heads, cell - stride, bucket);
        pending += visit(weights, next, heads, cell + stride, bucket);
        pending += visit(weights, next, heads, cell - 1, bucket);
        pending += visit(weights, next, heads, cell + 1, bucket);
    }
    free(next);
    free(weights);
    c_extremo_pareja("%u %llu\n", last, (unsigned long long)total);
    c_extremo_fin(start);
    return EXIT_SUCCESS;
}
#endif
