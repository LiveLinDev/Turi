/* k-mer counting, expert C: open-addressing table whose keys are slices of the sequence buffer
   (no per-key allocation), FNV-1a hash, power-of-two capacity. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define K 8

typedef struct { const char *key; long long count; } Slot;

int main(int argc, char **argv) {
    long long n = argc > 1 ? atoll(argv[1]) : 10000;
    char *seq = malloc((size_t)n + 1);
    long long seed = 42;
    for (long long i = 0; i < n; i++) {
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        seq[i] = "ACGT"[(seed / 65536) % 4];
    }
    size_t cap = 1 << 17;
    Slot *table = calloc(cap, sizeof(Slot));
    size_t used = 0;
    for (long long i = 0; i + K <= n; i++) {
        const char *key = seq + i;
        uint64_t h = 1469598103934665603ULL;
        for (int j = 0; j < K; j++) h = (h ^ (unsigned char)key[j]) * 1099511628211ULL;
        size_t at = (size_t)h & (cap - 1);
        while (table[at].key && memcmp(table[at].key, key, K) != 0) at = (at + 1) & (cap - 1);
        if (!table[at].key) {
            table[at].key = key;
            used++;
        }
        table[at].count++;
    }
    long long best = 0, checksum = 0;
    for (size_t i = 0; i < cap; i++) {
        long long c = table[i].count;
        if (c > best) best = c;
        checksum = (checksum + c * c) % 1000000007LL;
    }
    printf("%zu\n%lld\n%lld\n", used, best, checksum);
    free(table);
    free(seq);
    return 0;
}
