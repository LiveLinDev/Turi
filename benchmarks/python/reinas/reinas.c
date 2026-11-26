/* N reinas, C experto: el mismo árbol de búsqueda con columnas y diagonales en máscaras de bits. */
#include <stdio.h>
#include <stdlib.h>

static long long solve(unsigned cols, unsigned d1, unsigned d2, unsigned all) {
    if (cols == all) return 1;
    long long count = 0;
    unsigned avail = all & ~(cols | d1 | d2);
    while (avail) {
        unsigned bit = avail & (0u - avail);
        avail -= bit;
        count += solve(cols | bit, (d1 | bit) << 1, (d2 | bit) >> 1, all);
    }
    return count;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 8;
    printf("%lld\n", solve(0, 0, 0, (1u << n) - 1));
    return 0;
}
