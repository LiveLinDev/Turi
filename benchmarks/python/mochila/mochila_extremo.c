/* Mochila 0/1 con una fila descendente. Se leen los estados anteriores de
   16 capacidades antes de escribir el bloque actualizado. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <stdint.h>

#ifndef __AVX512F__
#error Este banco requiere AVX-512F
#endif

static void add_item(int32_t *best, int capacity, int weight, int value) {
    __m512i added = _mm512_set1_epi32(value);
    int start = capacity - 15;
    for (; start >= weight; start -= 16) {
        __m512i skip = _mm512_loadu_si512(best + start);
        __m512i previous = _mm512_loadu_si512(best + start - weight);
        __m512i take = _mm512_add_epi32(previous, added);
        _mm512_storeu_si512(best + start, _mm512_max_epi32(skip, take));
    }
    for (int at = start + 15; at >= weight; at--) {
        int take = best[at - weight] + value;
        if (take > best[at]) best[at] = take;
    }
}

#ifdef TZ_MOCHILA_COTA
#ifndef TZ_MOCHILA_SELECCION
static int increasing(const void* left, const void* right) {
    uint64_t a = *(const uint64_t*)left;
    uint64_t b = *(const uint64_t*)right;
    return (a > b) - (a < b);
}
#endif

static int knapsack_bound(const int* weights, const int* values, int n, int capacity) {
    uint64_t order[440];
    int count = 0;
#ifdef TZ_MOCHILA_SELECCION
    int total_weight = 0, total_value = 0;
#endif
    for (int i = 0; i < n; i++) {
        if (weights[i] > capacity) continue;
#ifdef TZ_MOCHILA_SELECCION
        total_weight += weights[i];
        total_value += values[i];
#endif
        uint64_t density = ((uint64_t)values[i] << 24) / (unsigned)weights[i];
        order[count++] = (density << 9) | (unsigned)i;
    }
    int remaining = capacity, lower = 0;
#ifdef TZ_MOCHILA_SELECCION
    if (total_weight <= capacity) return total_value;
    int lo = 0, hi = count, critical = -1;
    while (lo < hi) {
        int middle = lo + (hi - lo) / 2;
        uint64_t key = order[middle];
        order[middle] = order[lo];
        int split = hi - 1, weight_above = 0, value_above = 0;
        for (int at = hi - 1; at > lo; at--) {
            uint64_t current = order[at];
            if (current > key) {
                order[at] = order[split];
                order[split--] = current;
                int item = (int)(current & 511);
                weight_above += weights[item];
                value_above += values[item];
            }
        }
        order[lo] = order[split];
        order[split] = key;
        if (remaining < weight_above) {
            lo = split + 1;
        } else {
            remaining -= weight_above;
            lower += value_above;
            int item = (int)(key & 511);
            if (remaining < weights[item]) {
                critical = split;
                break;
            }
            remaining -= weights[item];
            lower += values[item];
            hi = split;
        }
    }
    if (critical < 0) return -1;
#else
    qsort(order, (size_t)count, sizeof(*order), increasing);
    int critical = count - 1;
    while (critical >= 0) {
        int item = (int)(order[critical] & 511);
        if (weights[item] > remaining) break;
        remaining -= weights[item];
        lower += values[item];
        critical--;
    }
    if (critical < 0) return lower;
#endif
    int pivot = (int)(order[critical] & 511);
    int q = weights[pivot], p = values[pivot];
    int64_t upper = (int64_t)q * lower + (int64_t)p * remaining;
    for (int i = critical - 1; i >= 0; i--) {
        int item = (int)(order[i] & 511);
        if (weights[item] <= remaining) {
            remaining -= weights[item];
            lower += values[item];
        }
    }
    int64_t gap = upper - (int64_t)q * lower;
    if (gap < q) return lower;
    int bound = capacity, forced = 0, core[440], used = 0;
    for (int i = 0; i < count; i++) {
        int item = (int)(order[i] & 511);
        int64_t gain = (int64_t)q * values[item] - (int64_t)p * weights[item];
        if (gain > gap) {
            bound -= weights[item];
            forced += values[item];
        } else if (gain >= -gap) {
            core[used++] = item;
        }
    }
    int32_t* best = calloc((size_t)bound + 1, sizeof(*best));
    if (!best) return -1;
    for (int i = 0; i < used; i++) {
        int item = core[i];
        add_item(best, bound, weights[item], values[item]);
    }
    int result = forced + best[bound];
    free(best);
    return result;
}
#endif

int main(int argc, char **argv) {
    long requested = argc > 1 ? strtol(argv[1], NULL, 10) : 100;
    if (requested > 100000) return EXIT_FAILURE;
    int n = requested > 0 ? (int)requested : 0;
    int capacity = 10 * n;
    double start = c_extremo_inicio();
#ifdef TZ_MOCHILA_COTA
    if (n <= 440) {
        int weights[440], values[440];
        for (int i = 0; i < n; i++) {
            weights[i] = 5 + (i * 37) % 41;
            values[i] = 10 + (i * 53) % 97;
        }
        int result = knapsack_bound(weights, values, n, capacity);
        if (result < 0) return EXIT_FAILURE;
        printf("%d\n", result);
        c_extremo_fin(start);
        return EXIT_SUCCESS;
    }
#endif
    int32_t *best = calloc((size_t)capacity + 1, sizeof(*best));
    if (!best) return EXIT_FAILURE;
    for (int i = 0; i < n; i++) {
        add_item(best, capacity, 5 + (i * 37) % 41, 10 + (i * 53) % 97);
    }
    int result = best[capacity];
    free(best);
    printf("%d\n", result);
    c_extremo_fin(start);
    return EXIT_SUCCESS;
}
