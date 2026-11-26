/* Juego de la vida sin bordes, C experto: celdas vivas en un arreglo de claves de 64 bits y vecinos
   contados en una tabla hash de direccionamiento abierto que se reutiliza en cada generación. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;
static inline u64 pack(long x, long y) { return ((u64)(unsigned)(int)x << 32) | (unsigned)(int)y; }
static inline u64 mix(u64 k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    return k;
}

typedef struct { u64 key; int count; int used; } Slot;
static Slot *table;
static size_t cap;

static Slot *find(u64 key) {
    size_t at = mix(key) & (cap - 1);
    while (table[at].used && table[at].key != key) at = (at + 1) & (cap - 1);
    return &table[at];
}

int main(int argc, char **argv) {
    int generations = argc > 1 ? atoi(argv[1]) : 50;
    size_t n = 0, ncap = 1 << 12;
    u64 *live = malloc(ncap * sizeof(u64));
    unsigned long long seed = 7;
    for (int x = 0; x < 64; x++)
        for (int y = 0; y < 64; y++) {
            seed = (seed * 1103515245ULL + 12345ULL) % 2147483648ULL;
            if ((seed >> 16) % 3 == 0) live[n++] = pack(x, y);
        }
    for (int g = 0; g < generations; g++) {
        if (g % 10 == 0) printf("%d %zu\n", g, n);
        size_t need = 1;
        while (need < n * 16 + 16) need <<= 1;
        if (need != cap) {
            free(table);
            cap = need;
            table = calloc(cap, sizeof(Slot));
        } else {
            memset(table, 0, cap * sizeof(Slot));
        }
        for (size_t i = 0; i < n; i++) {
            long x = (int)(live[i] >> 32), y = (int)(unsigned)live[i];
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++) {
                    if (!dx && !dy) continue;
                    Slot *s = find(pack(x + dx, y + dy));
                    s->used = 1;
                    s->key = pack(x + dx, y + dy);
                    s->count++;
                }
        }
        for (size_t i = 0; i < n; i++) {
            Slot *s = find(live[i]);
            if (s->used) s->count |= 1 << 8; /* marca: la celda estaba viva */
        }
        size_t m = 0;
        for (size_t i = 0; i < cap; i++) {
            if (!table[i].used) continue;
            int c = table[i].count & 0xff, alive = table[i].count >> 8;
            if (c == 3 || (c == 2 && alive)) {
                if (m == ncap) {
                    ncap *= 2;
                    live = realloc(live, ncap * sizeof(u64));
                }
                live[m++] = table[i].key;
            }
        }
        n = m;
    }
    printf("%d %zu\n", generations, n);
    free(live);
    free(table);
    return 0;
}
