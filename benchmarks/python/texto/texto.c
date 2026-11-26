/* Procesado de texto, C experto: el archivo entero en memoria con fread, palabras en minúsculas
   contadas en una tabla hash de direccionamiento abierto, y qsort de las distintas. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; int len; long long count; } Entry;
static Entry *table;
static size_t cap = 1 << 16, distinct;

static int cmp(const void *pa, const void *pb) {
    const Entry *a = pa, *b = pb;
    if (a->count != b->count) return a->count > b->count ? -1 : 1;
    int m = a->len < b->len ? a->len : b->len, c = memcmp(a->p, b->p, (size_t)m);
    return c ? c : a->len - b->len;
}

static int isspace_py(unsigned char c) { return c == ' ' || (c >= 9 && c <= 13) || (c >= 28 && c <= 31); }
static int strip(unsigned char c) { return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?'; }

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[size] = 0;
    char *lower = malloc((size_t)size + 1);
    for (long i = 0; i < size; i++) {
        char c = buf[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    table = calloc(cap, sizeof(Entry));
    long long lines = 0, words = 0, chars = 0;
    const char *longest = "";
    long longest_len = 0;
    long start = 0;
    while (start < size) {
        long end = start;
        while (end < size && buf[end] != '\n') end++;
        long text_len = end - start;
        long with_nl = end < size ? text_len + 1 : text_len;
        lines++;
        chars += with_nl; /* ASCII: un byte es un carácter */
        if (text_len > longest_len) {
            longest_len = text_len;
            longest = buf + start;
        }
        long i = start;
        while (i < end) {
            while (i < end && isspace_py((unsigned char)lower[i])) i++;
            long w0 = i;
            while (i < end && !isspace_py((unsigned char)lower[i])) i++;
            long a = w0, b = i;
            while (a < b && strip((unsigned char)lower[a])) a++;
            while (b > a && strip((unsigned char)lower[b - 1])) b--;
            if (b > a) {
                words++;
                int len = (int)(b - a);
                const char *p = lower + a;
                unsigned h = 2166136261u;
                for (int k = 0; k < len; k++) h = (h ^ (unsigned char)p[k]) * 16777619u;
                size_t at = h & (cap - 1);
                while (table[at].p && (table[at].len != len || memcmp(table[at].p, p, (size_t)len)))
                    at = (at + 1) & (cap - 1);
                if (!table[at].p) {
                    table[at].p = p;
                    table[at].len = len;
                    distinct++;
                }
                table[at].count++;
            }
        }
        start = end + 1;
    }
    printf("%lld %lld %lld\n", lines, words, chars);
    printf("%ld %.*s\n", longest_len, (int)(longest_len < 40 ? longest_len : 40), longest);
    Entry *all = malloc(distinct * sizeof(Entry));
    size_t m = 0;
    for (size_t i = 0; i < cap; i++)
        if (table[i].p) all[m++] = table[i];
    qsort(all, m, sizeof(Entry), cmp);
    for (size_t i = 0; i < m && i < 10; i++) printf("%12.*s %lld\n", all[i].len, all[i].p, all[i].count);
    free(buf);
    free(lower);
    free(table);
    free(all);
    return 0;
}
