/* Criba de Eratóstenes, C experto: un byte por número, memset y bucles sin comprobaciones. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    unsigned char *restrict p = malloc((size_t)n);
    memset(p, 1, (size_t)n);
    p[0] = p[1] = 0;
    for (long i = 2; i * i < n; i++)
        if (p[i])
            for (long j = i * i; j < n; j += i) p[j] = 0;
    long long count = 0, total = 0;
    for (long k = 0; k < n; k++)
        if (p[k]) {
            count++;
            total += k;
        }
    printf("%lld %lld\n", count, total);
    free(p);
    return 0;
}
