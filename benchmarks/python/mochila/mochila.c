/* Mochila 0/1, C experto: la misma recursión memorizada, con la memoria en una tabla plana. */
#include <stdio.h>
#include <stdlib.h>

static int n, capacity;
static int *w, *v;
static long long *memo;

static long long best(int i, int cap) {
    if (i == n) return 0;
    long long *slot = &memo[(size_t)i * (size_t)(capacity + 1) + (size_t)cap];
    if (*slot >= 0) return *slot;
    long long skip = best(i + 1, cap), result = skip;
    if (w[i] <= cap) {
        long long take = v[i] + best(i + 1, cap - w[i]);
        if (take > result) result = take;
    }
    return *slot = result;
}

int main(int argc, char **argv) {
    n = argc > 1 ? atoi(argv[1]) : 100;
    capacity = 10 * n;
    w = malloc(sizeof(int) * (size_t)n);
    v = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        w[i] = 5 + (i * 37) % 41;
        v[i] = 10 + (i * 53) % 97;
    }
    size_t cells = (size_t)n * (size_t)(capacity + 1);
    memo = malloc(sizeof(long long) * cells);
    for (size_t i = 0; i < cells; i++) memo[i] = -1;
    printf("%lld\n", best(0, capacity));
    free(w);
    free(v);
    free(memo);
    return 0;
}
