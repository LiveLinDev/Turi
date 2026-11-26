/* Frecuencia de palabras, C experto: las palabras se generan en un búfer, se cuentan en una tabla
   hash de direccionamiento abierto (FNV-1a) y las distintas se ordenan con qsort. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SYL[12] = {"ka", "lo", "mi", "ne", "ru", "sa", "to", "vi", "ze", "po", "an", "el"};

typedef struct { char word[8]; int len; long long count; } Entry;

static int cmp(const void *pa, const void *pb) {
    const Entry *a = pa, *b = pb;
    if (a->count != b->count) return a->count > b->count ? -1 : 1;
    return strcmp(a->word, b->word);
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 100000;
    size_t cap = 1 << 12;
    Entry *table = calloc(cap, sizeof(Entry));
    size_t distinct = 0;
    unsigned long long seed = 42;
    for (long k = 0; k < n; k++) {
        seed = (seed * 1103515245ULL + 12345ULL) % 2147483648ULL;
        int length = 1 + (int)((seed >> 16) % 3);
        char w[8];
        int len = 0;
        for (int s = 0; s < length; s++) {
            seed = (seed * 1103515245ULL + 12345ULL) % 2147483648ULL;
            const char *syl = SYL[(seed >> 16) % 12];
            w[len++] = syl[0];
            w[len++] = syl[1];
        }
        w[len] = 0;
        unsigned h = 2166136261u;
        for (int i = 0; i < len; i++) h = (h ^ (unsigned char)w[i]) * 16777619u;
        size_t at = h & (cap - 1);
        while (table[at].len && (table[at].len != len || memcmp(table[at].word, w, (size_t)len)))
            at = (at + 1) & (cap - 1);
        if (!table[at].len) {
            memcpy(table[at].word, w, (size_t)len + 1);
            table[at].len = len;
            distinct++;
        }
        table[at].count++;
    }
    Entry *all = malloc(distinct * sizeof(Entry));
    size_t m = 0;
    for (size_t i = 0; i < cap; i++)
        if (table[i].len) all[m++] = table[i];
    qsort(all, m, sizeof(Entry), cmp);
    for (size_t i = 0; i < m && i < 10; i++) printf("%s %lld\n", all[i].word, all[i].count);
    printf("%zu\n", distinct);
    free(table);
    free(all);
    return 0;
}
